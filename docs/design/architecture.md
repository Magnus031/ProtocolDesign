# ProtocolDesign — 服务器绘制界面程序的客户端通信协议

## 目录

1. [项目简介](#1-项目简介)
2. [系统架构](#2-系统架构)
3. [目录结构](#3-目录结构)
4. [协议设计](#4-协议设计)
5. [核心接口与数据结构](#5-核心接口与数据结构)
6. [各模块详解](#6-各模块详解)
7. [关键算法与机制](#7-关键算法与机制)
8. [配置参数](#8-配置参数)
9. [编译与运行](#9-编译与运行)
10. [设计亮点与已知局限](#10-设计亮点与已知局限)
11. [端到端交互 Demo](#11-端到端交互-demo)

---

## 1. 项目简介

本项目是一个用 **C++14** 编写的、基于 **TCP** 的应用级 UI 虚拟化通信协议实现系统。

### 核心理念

"**计算上云、交互下沉**"——将计算密集型的业务逻辑与图形渲染交由服务端完成，客户端仅负责像素重构与输入事件采集，实现轻量化的远程应用交互。

### 它能做什么？

- **应用级 UI 虚拟化**：以独立应用（而非整个桌面）为粒度，实现远程界面的实时传输与交互，区别于 RDP/VNC 的桌面镜像模式。
- **脏矩形增量传输**：AppHost 在服务端本地完成渲染，GKC 的 `DoDraw` 回调天然携带本次重绘的脏矩形（`rcPaint`），仅传输变化区域的像素数据，大幅降低带宽消耗，无需逐帧像素比对。
- **输入事件实时同步**：客户端捕获鼠标/键盘事件，标准化封装后通过网关精准路由至对应的后端 AppHost 进程。
- **智能网关调度**：网关支持多客户端并发接入、动态路由、AppHost 进程的生命周期管理，天然支持水平扩展。
- **业务逻辑解耦**：通过 IUiHost 接口抽象，使业务代码（.so/.dll 插件）无需感知网络存在，可在本地渲染与远程传输模式间自由切换。

### 适用场景

- 云原生应用的轻量化部署与交付
- 远程桌面 / 云游戏 / 工业数字孪生的应用级虚拟化
- 跨平台协作工具
- 学习自定义网络协议设计与应用级虚拟化架构

---

## 2. 系统架构

### 2.1 整体拓扑

系统采用 **Client → Gateway → AppHost** 三层架构，所有通信均通过智能网关中转：

```
┌──────────────────┐                ┌──────────────────┐                ┌──────────────────┐
│     Client       │    TCP 长连接   │     Gateway      │    TCP 长连接   │    AppHost(s)    │
│   (GKC 客户端)   │◄──────────────►│   (智能网关)      │◄──────────────►│  (应用宿主进程)   │
│                  │                │                  │                │                  │
│ · 像素重构(DoDraw)│   单端口接入    │ · Session 管理    │   动态路由      │ · 加载业务 .so    │
│ · 输入事件采集    │                │ · 动态路由转发     │                │ · 本地渲染        │
│ · GKC 窗口管理   │                │ · 进程生命周期     │                │ · 脏矩形检测      │
│                  │                │ · epoll 多路复用   │                │ · 像素差分发送     │
└──────────────────┘                └──────────────────┘                └──────────────────┘
```

### 2.2 数据流向

| 方向 | 内容 | 说明 |
|------|------|------|
| Client → Gateway → AppHost | 鼠标/键盘事件、窗口控制指令 | 输入事件实时透传 |
| AppHost → Gateway → Client | 脏矩形像素数据、窗口状态更新 | 仅传输变化区域 |
| Client → Gateway | SPAWN_APP 请求、心跳包 | 会话建立与维护 |
| Gateway → Client | Session ID 分配、错误通知 | 会话管理响应 |
| Gateway ↔ AppHost | 进程拉起/回收、健康检查 | 生命周期管理 |

### 2.3 会话模型

```
                    ┌─────────────────────────────────┐
                    │           Gateway               │
  Client_1 ────────►│                                 │────────► AppHost_A (app1.so)
  (Session 0x01)    │   Session 路由表                 │
                    │   ┌───────────────────────────┐ │
  Client_2 ────────►│   │ Client_Socket ↔ Session_ID│ │────────► AppHost_B (app2.so)
  (Session 0x02)    │   │ Session_ID ↔ AppHost_Socket│ │
                    │   └───────────────────────────┘ │
  Client_3 ────────►│                                 │────────► AppHost_C (app1.so)
  (Session 0x03)    │                                 │
                    └─────────────────────────────────┘
```

- 每个 Client 连接分配唯一 Session ID
- Gateway 维护 `Client_Socket ↔ Session_ID ↔ AppHost_Socket` 双向映射表
- 支持多客户端并发接入同一端口
- AppHost 进程按需动态拉起，支持水平扩展

### 2.4 线程 / 并发模型

**Gateway（智能网关）：**
```
主线程                 ──► 初始化；启动后调用 _IoPool_Fetch() 接管事件驱动
IoPool 线程 (GKC 内部) ──► epoll 监听客户端/AppHost 连接；_IoFunc 回调分发
路由分发               ──► _IoFunc 回调内：MessageParser 解包 → 按 SessionID 路由转发
进程管理               ──► 收到 SPAWN_APP 后 fork/exec 拉起 AppHost，waitpid 回收
心跳监测               ──► WorkPool 慢速池定时任务检测客户端/AppHost 存活
```

**AppHost（应用宿主）：**
```
主线程                 ──► 加载业务 .so 插件 (dlopen/dlsym)；运行 GuiHelper::Loop() 或事件循环
IoPool 线程 (GKC 内部) ──► 与 Gateway 的网络 I/O 事件回调（RECEIVED / SENT / BEFORE_CLOSE）
WorkPool 快速池        ──► PIXEL_DATA 打包发送（DoDraw 触发后投递，无逐帧像素比对）
WorkPool 慢速池        ──► 像素数据压缩任务（耗时操作）
PostWork               ──► IoPool 回调将输入事件安全投递给主线程的 ui_host_impl
```

**Client（客户端）：**
```
主线程 (GKC GuiHelper::Loop)  ──► UI 事件循环；DoDraw 写像素，DoMouse/DoKeyboard 采集输入
IoPool 线程 (GKC 内部)        ──► 网络 I/O 事件回调（IO_TYPE_RECEIVED / IO_TYPE_SENT 等）
回调分发                      ──► IoPool 收到数据 → MessageParser.Feed() → 按 CmdType 分发
PostWork                      ──► 从 IoPool 线程安全地向主线程投递像素更新任务
WorkPool（可选）              ──► 像素解压缩等 CPU 密集任务
心跳发送                      ──► 通过 TimerImpl 在主线程定时触发，经 IoPool 发送
```

---

## 3. 目录结构

```
ProtocolDesign/
├── .bazelversion              # Bazel 版本 (7.4.1)
├── MODULE.bazel               # Bazel 模块配置
├── WORKSPACE                  # Bazel 工作区配置
├── README.md                  # 项目说明
│
├── docs/                      # 文档目录
│   ├── Doxyfile               # Doxygen 配置
│   ├── api/
│   │   └── API_reference.md   # API 参考文档
│   ├── design/
│   │   ├── architecture.md    # 架构设计文档（本文件）
│   │   └── protocol_spec.md   # 协议规范文档
│   └── html/                  # Doxygen 生成的文档
│
├── src/                       # 源代码目录
│   ├── protocol/              # 协议层：消息定义与序列化
│   │   ├── protocol.h         # CmdType 枚举、Header 结构体、Packet 载体
│   │   ├── protocol.cpp       # 序列化/反序列化实现
│   │   ├── message_parser.h   # TCP 粘包/半包状态机解析器（Pull 模型）
│   │   ├── message_parser.cpp
│   │   └── BUILD
│   │
│   ├── common/                # 公共工具库（三个组件共用）
│   │   ├── byte_order.h       # 大端序/主机字节序转换宏
│   │   ├── message_handler.h  # CmdType 分发器：包装 MessageParser，注册/触发处理器
│   │   ├── message_handler.cpp
│   │   ├── logger.h           # 日志接口
│   │   ├── logger.cpp
│   │   └── BUILD
│   │
│   ├── gateway/               # 智能网关（可执行程序）
│   │   ├── gateway.h          # 网关核心类：IoPool 监听，MessageHandler 路由
│   │   ├── gateway.cpp
│   │   ├── session_manager.h  # SessionID ↔ (client_conn, apphost_conn) 双向映射
│   │   ├── session_manager.cpp
│   │   ├── process_manager.h  # fork/exec 拉起 AppHost，waitpid 回收
│   │   ├── process_manager.cpp
│   │   ├── main.cpp
│   │   └── BUILD
│   │
│   ├── apphost/               # AppHost（可执行程序，由 Gateway 动态拉起）
│   │   ├── apphost.h          # IoPool + MessageHandler；加载插件；管理 ui_host_impl
│   │   ├── apphost.cpp
│   │   ├── ui_host_impl.h     # 假的 IUiHost：拦截 DoDraw→rcPaint→PIXEL_DATA；dispatch 输入事件
│   │   ├── ui_host_impl.cpp
│   │   ├── pixel_capture.h    # capture_rect(pBuffer, width, rcPaint)→像素字节；供 ui_host_impl 使用
│   │   ├── pixel_capture.cpp
│   │   ├── plugin_loader.h    # dlopen/dlsym("sa_ui_main")，调用 Exec()
│   │   ├── plugin_loader.cpp
│   │   ├── main.cpp
│   │   └── BUILD
│   │
│   ├── client/                # Client（可执行程序，通用插件加载器，与 AppHost 对称）
│   │   ├── client.h           # IoPool StartConnect；MessageHandler 注册 SESSION_ACK/PIXEL_DATA
│   │   ├── client.cpp
│   │   ├── plugin_loader.h    # dlopen（Linux）/ LoadLibrary（Windows）加载 .so/.dll
│   │   ├── plugin_loader.cpp
│   │   ├── pixel_renderer.h   # 线程安全像素缓冲区（mutex）；供 client_viewer 插件使用
│   │   ├── pixel_renderer.cpp
│   │   ├── main.cpp           # 解析 --plugin/--gateway-host/--gateway-port；加载插件
│   │   └── BUILD
│   │
│   └── (无 src/network/ 模块)  # 网络 I/O 直接由 GKC IoPool 提供
│
├── plugins/                   # 业务插件（独立共享库，可热替换）
│   ├── demo_app/              # 服务端应用插件（AppHost 加载）
│   │   ├── demo_app.cpp       # ToplevelImpl：DoDraw 画色块，DoMouse 点击换色，DoKeyboard Space 切色
│   │   └── BUILD              # 构建为 libdemo_app.so
│   └── client_viewer/         # 客户端查看器插件（Client 加载）
│       ├── client_viewer.cpp  # ToplevelImpl：DoDraw 贴 PixelRenderer 缓冲，DoMouse/DoKeyboard 打包发送
│       └── BUILD              # 构建为 libclient_viewer.so（Linux）/ client_viewer.dll（Windows）
│
├── tests/                     # 顶层测试目录
│   └── BUILD
│
└── third_party/               # 第三方依赖
    ├── BUILD
    └── GKC/                   # GKC (General Kind C++) 框架
        ├── public/include/
        │   ├── base/
        │   │   ├── GkcDef.h       # 基础类型、LiteCom/RefPtr/UniquePtr、UI 接口定义
        │   │   ├── GkcGui.h       # GUI 包装层：_Window/_Toplevel, ToplevelImpl<T>, WorkImpl, TimerImpl
        │   │   ├── GkcCga.h       # CGA / raster 入口（上游 51049a4 新增）
        │   │   └── system/
        │   │       └── ui_types.h # UiSize, UiPoint, UiRect, ColorQuad 等几何/像素类型
        │   └── sys/
        │       └── GkcSys.h       # 系统工具入口
        ├── RT/GkcSys/public/
        │   └── _GkcSys.h          # IoPool / WorkPool 运行时接口
        ├── doxygen/               # 上游分层与组件文档
        └── util/
            ├── private/include/ui/ # UIHost 平台私有头（uihost 可执行程序依赖）
            └── gui/
                ├── uihost/        # GKC UIHost 实现（Wayland/Win32 后端）
                └── LocalDesk/     # GKC GUI 程序范式示例（ToplevelImpl 用法）
```

> **构建系统**：Bazel 7.4.1。可执行目标：`gateway`、`apphost`、`client`；共享库目标：`demo_app`、`client_viewer`。
> `src/common/` 中的 `MessageHandler` 被三个可执行目标共同依赖。
> 当前仓库记录的 GKC submodule 快照为 `51049a4`。相较之前的 `1152096`，上游新增了基础 CGA 头文件和 multi-dimensional array 相关类型。

---

## 4. 协议设计

### 4.1 设计原则

本协议基于 **纯 TCP 长连接**，采用自定义二进制格式，设计目标：
- 通过固定长度 Header 解决 TCP 流式传输的**粘包**与**半包**问题
- 通过 Magic Number 过滤非法数据包
- 通过 Session ID 支持多应用会话复用
- 保证指令解析的原子性

### 4.2 协议头（Header）格式

所有消息均以一个 **固定长度 Header** 开头，后跟可变长度载荷（Body）：

```
┌──────────────────────────────────────────────────────────────────────┐
│  Bytes 0-3     │  Bytes 4-7     │  Byte 8       │  Bytes 9-12      │
│  Magic Number  │  Session ID    │  Cmd Type     │  Body Length      │
│  (4 bytes)     │  (4 bytes BE)  │  (1 byte)     │  (4 bytes BE)    │
└──────────────────────────────────────────────────────────────────────┘
│  Body (bodyLength bytes)                                            │
└─────────────────────────────────────────────────────────────────────┘
```

| 字段 | 大小 | 说明 |
|------|------|------|
| **Magic Number** | 4 bytes | 固定标识，用于非法包过滤与协议版本识别 |
| **Session ID** | 4 bytes (BE) | 会话标识，用于 Gateway 多应用路由映射 |
| **Cmd Type** | 1 byte | 指令类型枚举 |
| **Body Length** | 4 bytes (BE) | 后续载荷的字节长度 |

> **BE = Big-Endian（大端序/网络字节序）**，所有多字节整数均使用大端序。

### 4.3 指令类型（Cmd Type）

以下枚举与 `src/protocol/protocol.h` 中的实际实现完全对应：

```cpp
enum class CmdType : uint8_t {
    // 会话管理
    HEARTBEAT     = 0x01,  // 心跳包，Body 为空，双向
    SPAWN_APP     = 0x02,  // Client → Gateway：请求启动 AppHost 进程
    SESSION_ACK   = 0x03,  // Gateway → Client：会话建立确认，携带 session_id
    APPHOST_READY = 0x04,  // AppHost → Gateway：绑定 AppHost 连接到 session_id

    // 像素传输
    PIXEL_DATA    = 0x10,  // AppHost → Client：脏矩形像素数据

    // 输入事件（鼠标与键盘合并为一个 CmdType，靠 Body 首字节 eventType 区分）
    INPUT_EVENT   = 0x20,  // Client → AppHost：鼠标或键盘事件

    // 控制
    CLOSE_SESSION = 0xFE,  // 任意方向：优雅关闭会话
    ERROR_RESP    = 0xFF,  // 任意方向：协议级错误通知
};
```

### 4.4 各类型载荷格式

**SPAWN_APP 载荷：**
```
┌──────────────────┬──────────────────────┐
│ appNameLength    │ appName              │
│ (4 bytes BE)     │ (variable, UTF-8)    │
└──────────────────┴──────────────────────┘
```

**PIXEL_DATA 载荷（AppHost → Client）：**
```
┌──────────┬──────────┬──────────┬───────────┬───────────┬─────────┬──────────┐
│ frameSeq │ rectLeft │ rectTop  │ rectRight │ rectBottom│ dataLen │pixelData │
│ (4 BE)   │ (4 BE)   │ (4 BE)   │ (4 BE)   │ (4 BE)   │ (4 BE)  │(variable)│
└──────────┴──────────┴──────────┴───────────┴───────────┴─────────┴──────────┘
```

- `frameSeq`：帧序列号，Client 可据此丢弃过期帧
- `rectLeft/Top/Right/Bottom`：脏矩形坐标（大端序 32 位整数）
- `pixelData`：该区域的 ARGB 像素数据，行优先，与 `apphost.cpp` 中 `send_pixel_data()` 写法一致

**INPUT_EVENT 载荷（Client → AppHost）：**

鼠标与键盘共用 `CmdType::INPUT_EVENT = 0x20`，Body 首字节 `eventType` 区分具体事件：

鼠标事件 (`eventType = 0x01~0x06`)：
```
┌──────────────┬──────────────────┬──────────────────┬──────────────────┐
│ eventType    │ x                │ y                │ timestamp        │
│ (1 byte)     │ (4 bytes BE)     │ (4 bytes BE)     │ (8 bytes BE)     │
└──────────────┴──────────────────┴──────────────────┴──────────────────┘
eventType: MOVE=0x01, LEFT_DOWN=0x02, LEFT_UP=0x03,
           RIGHT_DOWN=0x04, RIGHT_UP=0x05, SCROLL=0x06
```

键盘事件 (`eventType = 0x10~0x11`)：
```
┌──────────────┬──────────────────┬──────────────────┐
│ eventType    │ keyCode          │ timestamp        │
│ (1 byte)     │ (2 bytes BE)     │ (8 bytes BE)     │
└──────────────┴──────────────────┴──────────────────┘
eventType: KEY_DOWN=0x10, KEY_UP=0x11
keyCode: GKC KB_* 枚举值，Client 侧直接从 DoKeyboard(pKb->btKey) 填入
```

### 4.5 TCP 粘包/半包处理

由于 TCP 是流式协议，接收端采用**状态机**模式解包：

```
                ┌──────────┐
                │  IDLE    │
                └────┬─────┘
                     │ 接收数据
                     ▼
           ┌────────────────────┐
           │  READ_HEADER       │ ◄── 累积读满 Header 长度
           │  (缓冲区累积)       │
           └────────┬───────────┘
                    │ Header 完整
                    ▼
         ┌──────────────────────┐
         │ VALIDATE_MAGIC       │ ◄── 校验 Magic Number
         │                      │     失败则丢弃并重新同步
         └────────┬─────────────┘
                  │ 校验通过
                  ▼
         ┌──────────────────────┐
         │  READ_BODY           │ ◄── 根据 Body Length 继续累积
         │  (缓冲区累积)         │
         └────────┬─────────────┘
                  │ Body 完整
                  ▼
         ┌──────────────────────┐
         │  DISPATCH            │ ◄── 按 Cmd Type 分发处理
         └──────────────────────┘
```

---

## 5. 核心接口与数据结构

### 5.1 IUiHost 接口（来自 GKC 框架）

`IUiHost` 是 GKC 的 UI 宿主抽象，通过 `LcInterface<IUiHost>` 持有，对业务逻辑屏蔽底层平台（Wayland / Win32）。
接口本体定义在 `GkcDef.h`，`GkcGui.h` 只是在其上提供 `_Window` / `ToplevelImpl<T>` /
`WorkImpl<T>` / `TimerImpl<T>` 等薄包装。核心 API 如下：

```cpp
struct IUiHost {
    int  (*Loop)(void* pContext) noexcept;                   // 进入主事件循环（阻塞）
    void (*Quit)(void* pContext) noexcept;                   // 退出事件循环
    void (*PostWork)(void* pContext,
                     const WorkProc& work, void* pData) noexcept; // 跨线程安全投递任务到主线程
    uintptr (*AddTimer)(void* pContext, int iPeriod,
                        const WorkProc& work, void* pData) noexcept;
    void (*RemoveTimer)(void* pContext, uintptr id) noexcept;
    uintptr (*CreateToplevel)(void* pContext, bool bResizable,
                              int iWidth, int iHeight,
                              IUiToplevel** ppInterface) noexcept;
    // ... CreateDialogBox / CreatePopup / Destroy
};

// 业务代码通过全局实例访问
extern LcInterface<IUiHost> g_ui_host;

// GkcGui.h 中最薄的宿主包装
class GuiHelper {
    static int  Loop()  noexcept;
    static void Quit()  noexcept;
};

template <class T>
class WorkImpl {
    void PostWork() noexcept;
};

template <class T>
class TimerImpl {
    uintptr AddTimer(int iPeriod) noexcept;
    void RemoveTimer(uintptr id) noexcept;
};
```

**窗口 UI 消息**：通过 `UiMessageHandler` 回调接收，消息类型：
`UI_MESSAGE_DRAW`、`UI_MESSAGE_CLOSE`、`UI_MESSAGE_MOUSE`、`UI_MESSAGE_KEYBOARD`、`UI_MESSAGE_TEXT`。

业务代码通常继承 `ToplevelImpl<T>` 并重写对应的 `DoDraw / DoMouse / DoKeyboard / DoClose` 方法，GKC 内部自动注册回调并分发。

**IUiHost 是纯接口，不是平台代码**

`IUiHost` 本身只是一组函数指针的结构体，不绑定任何平台。它有两套具体实现，
分别供 Client 和 AppHost 使用：

| | Client | AppHost |
|---|---|---|
| 谁提供 IUiHost | GKC 运行时（已实现好） | 你自己写的 `ui_host_impl` |
| `CreateToplevel` | 创建真实 Wayland/Win32 窗口 | 分配像素缓冲区，创建假窗口结构体 |
| `Loop` | 进入真实 OS 事件循环 | 等待 IoPool 事件或退出信号 |
| `PostWork` | 向真实 UI 线程投递任务 | 向主线程消息队列投递任务 |
| 像素去哪里 | Wayland/Win32 合成器 → 屏幕 | 截获 → 打包 PIXEL_DATA → 网络 |
| 事件从哪来 | OS 鼠标/键盘 | 网络 MOUSE_EVENT/KEYBOARD_EVENT |

**接口透明化原理**：业务 `.so` 插件只调用 `IUiHost` 接口写像素和接收事件，
完全不知道底层是真实屏幕还是网络传输：

```
插件调用:  host.CreateToplevel(800, 600)  →  不管真假
          window.DoDraw → 写像素          →  不管谁消费
          window.DoMouse → 处理坐标       →  不管哪来的

Client 里: 像素 → Wayland/Win32 → 屏幕
AppHost 里: 像素 → ui_host_impl → 截获 → 发网络
```

### 5.2 SA_UIMain 插件接口（GKC 标准）

`.so` 插件的入口约定，AppHost 通过 `dlsym("sa_ui_main")` 加载：

```cpp
struct SA_UIMain {
    // AppHost 调用此函数，传入 IUiHost 接口和启动参数
    int (*Exec)(const GKC::LcInterface<GKC::IUiHost>& lcHost,
                const GKC::ConstArray<GKC::ConstStringS>& args) noexcept;
};

extern "C" GKC::SA_UIMain* sa_ui_main();  // 插件必须以 C 链接导出此符号
```

### 5.3 UI 基础类型

```cpp
// 脏矩形描述
struct ui_rect {
    int left, top, right, bottom;
    // 支持 inflate/deflate/intersect/union 等几何操作
};

// 坐标点
struct ui_point {
    int x, y;
};

// ARGB 颜色
using ColorQuad = uint32_t;  // 0xAARRGGBB
```

### 5.4 协议头结构

```cpp
#pragma pack(push, 1)
struct ProtocolHeader {
    uint32_t  magicNumber;    // 固定标识，过滤非法包
    uint32_t  sessionId;      // 会话 ID（大端序）
    uint8_t   cmdType;        // 指令类型 (CmdType 枚举)
    uint32_t  bodyLength;     // 载荷长度（大端序）
};
#pragma pack(pop)
```

### 5.5 脏矩形数据

```cpp
struct DirtyRectData {
    uint32_t              frameSeq;      // 帧序列号
    ui_rect               rect;          // 脏矩形区域
    std::vector<uint8_t>  pixelData;     // ARGB 像素数据（可压缩）
};
```

### 5.6 输入事件（INPUT_EVENT 子类型）

鼠标与键盘共用 `CmdType::INPUT_EVENT = 0x20`，Body 首字节 `eventType` 决定后续格式：

```cpp
// 鼠标事件 Body（eventType = 0x01~0x06），总计 13 字节
struct MouseEventBody {
    uint8_t   event_type;  // 0x01=MOVE, 0x02=LEFT_DOWN, 0x03=LEFT_UP,
                           // 0x04=RIGHT_DOWN, 0x05=RIGHT_UP, 0x06=SCROLL
    int32_t   x;           // 大端序，相对窗口左上角
    int32_t   y;           // 大端序；SCROLL 时 y 为滚动增量
    uint64_t  timestamp;   // 大端序，微秒
};

// 键盘事件 Body（eventType = 0x10~0x11），总计 11 字节
struct KeyboardEventBody {
    uint8_t   event_type;  // 0x10=KEY_DOWN, 0x11=KEY_UP
    uint16_t  key_code;    // 大端序，直接使用 GKC KB_* 枚举值
    uint64_t  timestamp;   // 大端序，微秒
};
```

> Client 侧直接从 GKC 回调填入：`key_code = pKb->btKey`，无需额外键码映射。

---

## 6. 各模块详解

### 6.1 Gateway（智能网关）

**目录**：`src/gateway/`

Gateway 是系统的中枢节点，职责远超简单转发，具备以下核心能力：

#### I/O 多路复用

- Linux 环境下采用 `epoll` 实现高并发连接监听
- 非阻塞 I/O 配合线程池的 Reactor 模式
- 单端口对多客户端连接的实时监听与数据分发

#### 动态路由与 Session 管理

```
1. Client 连接 Gateway
2. Client 发送 SPAWN_APP 指令（携带应用名）
3. Gateway 通过 fork/exec 拉起对应 AppHost 进程
4. Gateway 分配 Session ID，建立双向映射：
   Client_Socket ↔ Session_ID ↔ AppHost_Socket
5. Gateway 向 Client 返回 SESSION_ACK
6. 后续数据按 Session ID 精准路由
```

#### 进程生命周期管理

- **动态拉起**：解析 `SPAWN_APP` 指令后，通过 `fork()` + `exec()` 创建独立 AppHost 进程
- **健康监测**：定期检测 AppHost 进程存活状态
- **孤儿回收**：自动回收崩溃或断连的 AppHost 进程，防止资源泄露
- **进程隔离**：每个 AppHost 运行在独立进程空间，单个崩溃不影响其他会话

### 6.2 AppHost（应用宿主）

**目录**：`src/apphost/`

AppHost 是服务端的核心执行单元，负责加载业务插件并提供假的 `IUiHost`，拦截插件的像素输出发往网络，同时把网络输入事件注入给插件。

#### 启动流程

```
1. 由 Gateway 通过 fork/exec 拉起
2. 通过 dlopen() 加载指定的业务 .so 插件
3. 通过 dlsym("sa_ui_main") 解析插件入口
4. 创建 ui_host_impl（假的 IUiHost），拦截 DoDraw 输出
5. 建立与 Gateway 的 TCP 连接（IoPool StartConnect）
6. 调用插件 Exec(ui_host_impl, args)，进入 GuiHelper::Loop()
```

#### 像素传输流程（rcPaint 直接传输，无逐帧比对）

GKC 的 `DoDraw(pDraw)` 回调天然携带脏矩形 `pDraw->rcPaint`，无需保存上一帧做像素比对：

```
GKC 触发 DoDraw(pDraw)
    pDraw->rcPaint  = 本次脏矩形（GKC 自维护，等于触发重绘的区域）
    pDraw->pBuffer  = 整窗口像素缓冲区
    ↓
ui_host_impl 拦截：
    pixel_capture(pDraw->pBuffer, pDraw->iWidth, pDraw->rcPaint)
        → 裁出 rcPaint 区域的 ARGB 字节
    ↓
WorkPool 快速池：异步打包 PIXEL_DATA
    [frameSeq(4)][rcLeft(4)][rcTop(4)][rcRight(4)][rcBottom(4)][dataLen(4)][pixels...]
    ↓
IoPool BeginInput/EndInput → Gateway → Client
```

> **不存在** `DirtyRectDetector`（逐帧像素比对）。脏矩形职责由 GKC 的 `DoDraw` 回调机制承担。

#### 输入事件接收（PostWork 模式）

IoPool 线程收到输入事件后，**必须通过 PostWork 投递到主线程**再注入插件，不能跨线程直接操作：

```
IoPool 线程
  IO_TYPE_RECEIVED → MessageHandler.feed()
    MOUSE_EVENT 处理器 → 反序列化 → PostWork(inject_mouse, new UiMessageMouse)
    KEYBOARD_EVENT 处理器 → 反序列化 → PostWork(inject_keyboard, new UiMessageKeyboard)

主线程（PostWork 回调）
  inject_mouse → ui_host_impl.dispatch_mouse() → 插件 DoMouse()
  inject_keyboard → ui_host_impl.dispatch_keyboard() → 插件 DoKeyboard()
```

#### ui_host_impl 详解：假的 IUiHost

`ui_host_impl` 是 AppHost 的核心设计，它实现 `IUiHost` 接口但不触及任何真实 OS/GUI API。
它有**两个方向**的职责：

**方向 1：插件 → 网络（像素输出）**

插件写像素时，`ui_host_impl` 拦截并发往网络：

```
插件 DoDraw(pDraw)
    └─ 向 pDraw->pBuffer 写像素
ui_host_impl 主动调用存储的 handler.Process(ctx, UI_MESSAGE_DRAW, &drawInfo)
    └─ pDraw->rcPaint  = ui_host_impl 传入的脏矩形
    └─ pDraw->pBuffer  = ui_host_impl 持有的像素缓冲区
    └─ 插件把像素写进来
ui_host_impl 捕获像素
    └─ pixel_capture(pBuffer, width, rcPaint) → 裁出 ARGB 字节块
    └─ WorkPool → 打包 PIXEL_DATA → IoPool → Gateway → Client
```

**方向 2：网络 → 插件（事件注入）**

网络收到输入事件时，`ui_host_impl` 把它转成 UI 消息注入插件：

```
IoPool 线程收到 MOUSE_EVENT
    └─ MessageHandler 分发 → 反序列化 Body → UiMessageMouse
    └─ PostWork(inject_mouse, pMouse)          ← 跨线程投递

主线程（PostWork 回调）
    └─ ui_host_impl.dispatch_mouse(pMouse)
        └─ 构造 ui_message_mouse
        └─ handler.Process(ctx, UI_MESSAGE_MOUSE, &uiMouse)
            └─ 插件的 DoMouse() 被调用，坐标正确
```

键盘事件同理：`KEYBOARD_EVENT → dispatch_keyboard() → UI_MESSAGE_KEYBOARD → DoKeyboard()`。

**ui_host_impl 需要存储什么**

```cpp
class UiHostImpl {
    // 假窗口的状态
    struct VirtualWindow {
        ui_message_handler handler;   // 插件通过 SetMessageHandler 注册的回调
        void*              ctx;       // 对应的 pContext
        uint8_t*           pixel_buf; // 虚拟像素缓冲区（代替 Wayland shm）
        int                width, height;
    };
    VirtualWindow  window_;

    // 与 IoPool 共享的连接，用于发送 PIXEL_DATA
    uintptr        gateway_conn_;
};
```

`SetMessageHandler` 由插件调用（通常通过 `WindowImpl<T>::Create()` 内部触发），
`ui_host_impl` 只是把 `handler` 和 `ctx` 存下来。之后：
- **注入事件时**：调用 `window_.handler.Process(window_.ctx, UI_MESSAGE_MOUSE/KEYBOARD, ...)`
- **触发重绘时**：调用 `window_.handler.Process(window_.ctx, UI_MESSAGE_DRAW, &drawInfo)`，
  `drawInfo.pBuffer = window_.pixel_buf_`，插件写完像素后截获并发送

---

### 6.3 Client（客户端）

**目录**：`src/client/`（通用插件加载器）+ `plugins/client_viewer/`（查看器 UI 插件）

Client 与 AppHost **完全对称**：`src/client/` 是通用插件加载器可执行程序，实际的查看器 UI 逻辑在 `plugins/client_viewer/` 插件中，通过 `dlopen`（Linux）/ `LoadLibrary`（Windows）动态加载。

#### Client 可执行程序职责

- 解析命令行参数（`--plugin`、`--gateway-host`、`--gateway-port`）
- 加载 `.so`/`.dll` 插件，调用 `sa_ui_main()` 的 `Exec(real_gkc_ui_host, args)`
- 提供 `PixelRenderer`（线程安全像素缓冲区）供插件使用

#### client_viewer 插件启动流程

```
Exec(real_gkc_ui_host, args)
    ↓
创建 ViewerWindow（ToplevelImpl 子类）
    ↓
IoPool StartConnect → Gateway
    ↓
MessageHandler 注册：SESSION_ACK / PIXEL_DATA / ERROR_RESP 处理器
    ↓
发送 SPAWN_APP（携带要启动的 AppHost 插件名）
    ↓
ViewerWindow.Show(true) + GuiHelper::Loop()
```

#### 像素重构流程（插件内）

```
IoPool 线程收到 PIXEL_DATA
    → MessageHandler.feed() → PIXEL_DATA 处理器
        → PixelRenderer.apply_dirty_rect(rcPaint, pixels) [mutex 保护]
        → PostWork() 向主线程投递

主线程 DoDraw(pDraw) 回调
    → PixelRenderer.blit(pDraw->pBuffer, pDraw->iWidth, pDraw->rcPaint)
    （只做 memcpy，无本地绘图逻辑）
```

#### 输入事件采集（插件内）

- `DoMouse(pMouse)`：直接用 `pMouse->uEvent`、`x`、`y` 打包 MOUSE_EVENT，经 IoPool 发出
- `DoKeyboard(pKb)`：直接用 `pKb->btKey`（`KB_*`）、`pKb->btState`（`KB_STATE_*`）打包，**无需键码映射**

### 6.4 GKC 异步回调范式

GKC 的整个事件系统基于**回调函数 + 事件驱动**，全面异步，无任何阻塞调用：

| 回调类型 | 触发线程 | 签名 | 用途 |
|---------|---------|------|------|
| `_IoFunc::Exec` | IoPool 内部线程 | `int(void* pCtx, int iType, uintptr uParam)` | 网络事件（收包、连接、断开） |
| `UiMessageHandler::Process` | 主线程 | `void(void* pCtx, uint uMsg, uintptr uParam)` | UI 事件（绘制、鼠标、键盘） |
| `WorkProc::Exec` | WorkPool 线程 | `void(void* pCtx)` | CPU 任务（pixel_capture 打包、像素解压缩） |

**跨线程协作**：IoPool/WorkPool 线程完成数据处理后，通过 `g_ui_host.GetFunc()->PostWork()`，
或对象自身继承 `WorkImpl<T>` / `TimerImpl<T>` 这两个包装类，将任务安全投递到主线程，避免竞争条件。

**典型路径（Client 收到像素包）：**
```
IoPool 线程
  IO_TYPE_RECEIVED 触发 _IoFunc::Exec
    → MessageParser.Feed() → next_packet() 得到完整协议包
    → 写入 PixelRenderer（mutex 保护）
    → PostWork() 投递到主线程
主线程
  WorkImpl::DoWork() 执行
    → DoDraw() 回调：把像素 memcpy 到 pDraw->pBuffer
```

### 6.5 网络传输层

网络 I/O 直接使用 **GKC `IoPool`**（`GKC/RT/GkcSys/public/_GkcSys.h`），不设独立的 `src/network/` 模块。

- Linux 底层：epoll 事件驱动
- Windows 底层：IOCP 事件驱动
- 接口统一：`StartListen` / `StartConnect` / `BeginInput` / `DisableHandle`

各组件用法：Gateway 调用 `StartListen` 监听客户端并 `StartConnect` 对接 AppHost；AppHost / Client 均调用 `StartConnect` 连接 Gateway。

### 6.6 协议层

**目录**：`src/protocol/`

负责消息的**序列化**（结构体 → 字节流）和**反序列化**（字节流 → 结构体）。

- 所有多字节整数在序列化时转换为大端序（网络字节序），反序列化时转回主机字节序
- 通过状态机模式实现 TCP 粘包/半包的完整解析
- Magic Number 校验确保协议一致性

---

## 7. 关键算法与机制

### 7.1 脏矩形传输（GKC DoDraw 回调驱动）

AppHost **不做逐帧像素比对**。脏矩形由 GKC 的 `DoDraw` 回调机制天然提供：

```
事件触发（鼠标点击 / 键盘输入 / 计时器等）
    ↓ GKC 内部标记对应区域 damage
    ↓ GKC 调用 DoDraw(pDraw)
        pDraw->rcPaint = 本次需重绘的最小区域

ui_host_impl 拦截 DoDraw：
    pixel_capture(pDraw->pBuffer, pDraw->iWidth, pDraw->rcPaint)
        → 按 rcPaint 坐标裁出 ARGB 字节块
    ↓
WorkPool 快速池：打包 PIXEL_DATA，IoPool 发往 Gateway → Client
```

脏矩形的精度由插件自身的 `DoDraw` 实现决定——插件只在 `rcPaint` 范围内写像素，天然形成最小更新区域。在千兆内网带宽下，即使偶尔 rcPaint 覆盖较大区域，传输代价也可接受。

### 7.2 per-connection 上下文设计（ConnContext + free_list + 自旋锁）

IoPool 不管理上下文对象的生命周期，调用者必须自行分配和释放。三个组件（Gateway、AppHost、Client）统一采用以下设计模式：

#### 结构布局

```
ConnContext : public node_base
├── uintptr      id_            // IoPool 返回的连接句柄，用于 BeginInput/DisableHandle
├── ConnContext* next_active_   // 活跃连接的侵入式单链表（用于关闭时清理）
├── _IoFunc      io_func_       // 嵌入式回调，Exec 指向静态函数，this 作为 pIoContext
├── MessageParser/MessageHandler parser_  // 粘包解析器，每连接独立状态
└── Impl*        server_        // 反向指针，用于访问 IoPool、连接池等共享资源
```

- 继承 `node_base`（`GkcDef.h`）：提供 `m_pNext` 字段，供 `free_list<ConnContext>` 链接空闲节点
- 嵌入 `_IoFunc io_func_`：每个连接拥有独立的回调指针，在 `IO_TYPE_ACCEPT_INIT` 时通过 `SetHandleFunc` 绑定到本连接，`pIoContext` 设为 `this`

#### 内存管理

```cpp
free_list<ConnContext> conn_pool_;    // 预分配节点池，无堆分配
ConnContext*           active_head_;  // 活跃节点的侵入式单链表
volatile int           conn_lock_;   // 原子自旋锁（0=未持有，1=持有）
```

分配顺序（严格遵守，参照 `node_helper::ConstructNode`）：
```
FetchFreeNode(conn)    → 查看空闲链表头，conn 指向该节点，但节点尚未摘除
call_constructor(*conn) → 就地构造（placement new），重置所有字段
PickFreeNode()          → 将节点从空闲链表摘除（只有构造成功才执行）
```
若 `call_constructor` 失败，`PickFreeNode` 不会执行，节点留在空闲链表头，不会丢失。

释放顺序：
```
call_destructor(*conn)  → 析构对象
lock 保护:
    从 active_head_ 链表摘除
    PutFreeNode(conn)    → 将节点头插回空闲链表
```

#### 关闭时清理

`_IoPool_Disable()` 不保证在线程退出之前为所有活跃连接触发 `BEFORE_CLOSE`。
在 `stop()` 调用 `_IoPool_Disable()` 后，需遍历 `active_head_` 链表，
对每个残留节点调用 `call_destructor + PutFreeNode`，确保无泄漏。

#### 三个组件均遵循此模式

| 组件 | ConnContext 类型 | 额外字段 |
|------|----------------|---------|
| AppHost | `ConnContext` | `MessageParser parser_` |
| Gateway（Client 侧） | `ClientSession` | `session_id_`、`apphost_conn_` |
| Gateway（AppHost 侧） | `AppHostSession` | `session_id_`、`client_conn_` |
| Client | `ClientConn` | `MessageParser parser_`、`PixelRenderer* renderer_` |

---

### 7.4 帧序列号

每个 PIXEL_DATA 包携带 `frameSeq`，供 Client 处理乱序/重复：

- 正常情况：按序更新对应区域
- 收到旧序列号的包（网络重传）：可选择丢弃（该区域已被更新版本覆盖）
- 输入事件携带 `timestamp`，AppHost 可据此做事件顺序排队

### 7.5 动态进程调度

Gateway 的进程调度流程：

```
收到 SPAWN_APP("app1.so")
    ↓
检查 app1.so 是否存在且合法
    ↓
fork() → 子进程
    ↓
子进程: exec() 加载 AppHost 可执行文件，传入 app1.so 路径
    ↓
父进程 (Gateway):
  · 记录子进程 PID
  · 等待 AppHost 建立 TCP 连接
  · 建立 Session 映射
  · 向 Client 返回 SESSION_ACK
```

---

## 8. 配置参数

### 网络配置

| 参数 | 值 | 说明 |
|------|----|------|
| GATEWAY_PORT | 可配置 | 网关监听端口 |
| MAGIC_NUMBER | 0x50445347 | 协议标识 ("PDSG") |

### 协议参数

| 参数 | 说明 |
|------|------|
| HEADER_SIZE | 协议头固定长度 (13 bytes) |
| MAX_BODY_SIZE | 单包最大载荷大小 |
| HEARTBEAT_INTERVAL | 心跳包发送间隔 |
| SESSION_TIMEOUT | 会话无活动超时时间 |
| FRAME_RATE_LIMIT | 帧率上限（脏矩形发送频率） |

### 构建配置

| 参数 | 值 | 说明 |
|------|----|------|
| Bazel 版本 | 7.4.1 | 构建系统版本 |
| C++ 标准 | C++17 | 编译标准 |
| GKC 框架 | git submodule | 第三方依赖 |

---

## 9. 编译与运行

### 依赖

- C++17 编译器（GCC / Clang / MSVC）
- Bazel 7.4.1
- GKC 框架（git submodule）：三个组件（Client / Gateway / AppHost）全部依赖
  - Linux：GKC 内部使用 epoll、Wayland、POSIX threads、dlopen
  - Windows：GKC 内部使用 IOCP、Win32 GDI

### 构建

```bash
# 构建全部目标
bazel build //...

# 单独构建可执行程序
bazel build //src/gateway:gateway
bazel build //src/apphost:apphost
bazel build //src/client:client

# 构建插件
bazel build //plugins/demo_app:demo_app        # → libdemo_app.so
bazel build //plugins/client_viewer:client_viewer  # → libclient_viewer.so / client_viewer.dll
```

### 运行顺序

```bash
# 1. 启动网关
./bazel-bin/src/gateway/gateway --client-port=19000 --apphost-port=19001

# 2. 启动客户端（指定插件和 Gateway 地址）
#    Gateway 会自动 fork/exec AppHost，无需手动启动
./bazel-bin/src/client/client \
  --plugin=./bazel-bin/plugins/client_viewer/libclient_viewer.so \
  --gateway-host=127.0.0.1 \
  --gateway-port=19000
```

> AppHost 由 Gateway 动态拉起，命令行参数由 Gateway 传入（包括 `--plugin=libdemo_app.so`）。

---

## 10. 设计亮点与已知局限

### 设计亮点

| 亮点 | 说明 |
|------|------|
| **应用级虚拟化** | 以独立应用为粒度，区别于 RDP/VNC 的桌面镜像模式 |
| **IUiHost 接口解耦** | 业务逻辑与渲染引擎彻底分离，支持本地/远程模式无缝切换 |
| **GKC rcPaint 驱动传输** | 脏矩形由 GKC DoDraw 回调天然提供，无逐帧比对，传输最小更新区域 |
| **双端插件对称** | AppHost 和 Client 均为通用插件加载器，业务逻辑在 .so/.dll 中，可热替换 |
| **MessageHandler 分发** | IoPool 回调内统一通过 MessageHandler 按 CmdType 分发，三个组件共用模式 |
| **智能网关调度** | Session 管理 + 动态路由 + 进程生命周期，支持水平扩展 |
| **TCP 可靠传输** | 自定义二进制协议，状态机解包，解决粘包/半包问题 |
| **进程级隔离** | fork/exec 独立进程空间，单点故障不影响全局 |
| **全 GKC 统一** | Client / Gateway / AppHost 三个组件均基于 GKC（IoPool + WorkPool + GkcGui），无 FLTK 依赖，跨 Linux/Windows |
| **回调异步范式** | _IoFunc / UiMessageHandler / WorkProc 三套回调全面异步，PostWork 线程安全跨线程调度 |
| **帧序列号机制** | 支持客户端在弱网下选择性丢弃过期帧 |

### 已知局限

| 局限 | 说明 |
|------|------|
| 像素压缩算法 | 当前使用基础压缩，未集成硬件加速编码 |
| 音频传输 | 协议暂不支持音频流传输 |
| 多显示器 | 暂不支持多显示器场景 |
| GPU 加速 | 服务端渲染未利用 GPU 硬件加速 |

---

## 11. 端到端交互 Demo

**场景**：用户在 Client 窗口点击鼠标 → 触发服务端 AppHost 重绘 → 像素回传至 Client 并显示。

此 Demo 完整串联五个关键组件：**GKC GUI API、MessageParser、MessageHandler、IUiHost、ui_host_impl**。

### 11.1 全链路时序图

```
Client 主线程        Client IoPool 线程    Gateway IoPool 线程    AppHost IoPool 线程    AppHost 主线程
      │                       │                      │                      │                    │
① DoMouse(pMouse)             │                      │                      │                    │
  打包 INPUT_EVENT             │                      │                      │                    │
  BeginInput/EndInput ────────►│                      │                      │                    │
                               │──IO_TYPE_RECEIVED────►                      │                    │
② Gateway:                     │               MessageParser.feed()          │                    │
                               │               next_packet(pkt)             │                    │
                               │               按 SessionId 路由             │                    │
                               │               BeginInput/EndInput ─────────►│                    │
                               │                      │               IO_TYPE_RECEIVED            │
③ AppHost:                     │                      │          MessageParser.feed()             │
                               │                      │          next_packet(pkt)                │
                               │                      │          MessageHandler 分发              │
                               │                      │          on_input_event() ───────────────►│
                               │                      │                      │   ④ PostWork
                               │                      │                      │      dispatch_mouse()
                               │                      │                      │      IUiHost.Process(MOUSE)
                               │                      │                      │      插件 DoMouse()
                               │                      │                      │      GKC → DoDraw(pDraw)
                               │                      │                      │◄─ ⑤ pixel_capture(rcPaint)
                               │                      │                      │      打包 PIXEL_DATA
                               │                      │                      │      BeginInput/EndInput
                               │                      │◄─────────────────────│
⑥ Gateway:                     │               IO_TYPE_RECEIVED              │
                               │               MessageParser.feed()          │
                               │               next_packet(pkt)             │
                               │               按 SessionId 路由             │
                               │◄─────────────────────                      │
⑦ IO_TYPE_RECEIVED             │                      │                      │
  MessageParser.feed()         │                      │                      │
  next_packet(pkt)             │                      │                      │
  MessageHandler 分发          │                      │                      │
  on_pixel_data()              │                      │                      │
  PixelRenderer.apply()        │                      │                      │
  PostWork() ──────────────────►（通知主线程重绘）       │                      │
      │                       │                      │                      │                    │
⑧ DoDraw(pDraw)               │                      │                      │                    │
  PixelRenderer.blit() → 屏幕  │                      │                      │                    │
```

---

### 11.2 各组件在链路中的角色

#### ① GKC GUI API — Client 采集鼠标事件

GKC 的 `ToplevelImpl<ClientWindow>` 在用户点击时自动调用 `DoMouse`，
这是 **GKC GUI API** 的标准回调机制（底层是 `UiMessageHandler`）。
开发者只需继承 `ToplevelImpl<T>` 并实现 `DoMouse`，GKC 负责平台事件适配（Wayland / Win32）：

```cpp
// plugins/client_viewer/client_viewer.cpp
void DoMouse(GKC::UiMessageMouse* pMouse) noexcept {
    // pMouse 由 GKC 从 OS 鼠标事件填充，开发者无需任何平台代码
    uint8_t buf[13];
    buf[0] = (pMouse->uEvent == MOUSE_EVENT_DOWN) ? 0x02 : 0x03;  // eventType
    write_be32(buf + 1, static_cast<uint32_t>(pMouse->x));
    write_be32(buf + 5, static_cast<uint32_t>(pMouse->y));
    write_be64(buf + 9, timestamp_us());
    // 通过 IoPool BeginInput/EndInput 发送 INPUT_EVENT（CmdType=0x20）
    send_packet(conn_id_, CmdType::INPUT_EVENT, buf, sizeof(buf));
}
```

**GKC GUI API 的作用**：`ToplevelImpl<T>` 封装了 `UI_MESSAGE_MOUSE` 的注册与分发，
`DoMouse` / `DoDraw` / `DoKeyboard` 是固定签名的虚回调，GKC 内部通过 `UiMessageHandler::Process`
分发所有 UI 消息，开发者只需实现这三个方法。

---

#### ② Gateway — 纯路由，只用 MessageParser 定帧

Gateway 收到 Client 的字节流后，唯一目的是找到完整协议包并按 SessionId 转发。
它 **不需要 MessageHandler**（不处理业务语义），只需 `MessageParser`：

```cpp
// src/gateway/gateway.cpp — ClientSession 的 IO_TYPE_RECEIVED 处理
void on_received(ClientSession* s, const uint8_t* data, size_t len) {
    s->parser_.feed(data, len);              // MessageParser: 字节流 → 协议帧
    Packet pkt;
    while (s->parser_.next_packet(pkt) == ParseResult::OK) {
        // 按 SessionId 查找目标 AppHost 连接句柄
        uintptr apphost_id = session_mgr_.find_apphost(pkt.header.session_id);
        if (apphost_id == 0) continue;
        forward_raw(apphost_id, pkt);        // 原包转发，不解析 Body
    }
}
```

**MessageParser 的作用**：TCP 是流式协议，单次 `IO_TYPE_RECEIVED` 可能只收到半个 Header，
也可能包含多个完整包。`MessageParser` 内部状态机负责：
1. `READ_HEADER`：累积到够 13 字节
2. `VALIDATE_MAGIC`：校验 `0xBEEF`，非法则丢弃并重新同步
3. `READ_BODY`：按 `body_length` 累积 Body

`feed()` 推入原始字节，`next_packet()` 以 Pull 方式拉出完整帧，调用者无需关心边界。

---

#### ③ AppHost — MessageParser + MessageHandler 双层解析

AppHost 不仅要定帧，还要按 `CmdType` 分发到不同的业务处理器。
因此在 `MessageParser` 之上再包一层 **MessageHandler**：

```cpp
// src/apphost/apphost.cpp — 初始化时注册处理器
void Impl::register_handlers() {
    handler_.on(CmdType::INPUT_EVENT,   [this](const Packet& p){ on_input_event(p); });
    handler_.on(CmdType::HEARTBEAT,     [this](const Packet& p){ on_heartbeat(p);   });
    handler_.on(CmdType::CLOSE_SESSION, [this](const Packet& p){ on_close(p);       });
}

// IO_TYPE_RECEIVED 回调只需一行
void on_received(ConnContext* conn, const uint8_t* data, size_t len) {
    conn->handler_.feed(data, len);  // MessageHandler 内部调用 MessageParser，再自动 dispatch
}
```

**MessageHandler 的作用**：是 `MessageParser` 的上层路由器，
维护 `unordered_map<uint8_t, HandlerFn>`（`CmdType` → 处理函数）。
`feed()` 先驱动 `MessageParser` 定帧，帧完整后立即按 `cmd_type` 查表并调用对应处理器。
调用者只需 `handler_.on(CmdType::X, callback)` 注册，之后 `feed()` 一行搞定全部分发。

---

#### ④ AppHost 主线程 — IUiHost::PostWork + ui_host_impl 注入事件

AppHost 的 IoPool 线程不能直接操作插件（插件在主线程），
必须通过 **IUiHost::PostWork** 跨线程投递：

```cpp
// src/apphost/apphost.cpp — INPUT_EVENT 处理器（IoPool 线程内执行）
void Impl::on_input_event(const Packet& pkt) {
    uint8_t event_type = pkt.body[0];

    if (event_type >= 0x01 && event_type <= 0x06) {        // 鼠标事件
        auto* msg = new UiMessageMouse{};
        msg->uEvent = event_type;
        msg->x      = read_be32(pkt.body.data() + 1);
        msg->y      = read_be32(pkt.body.data() + 5);
        // IUiHost::PostWork — 跨线程安全投递到主线程
        ui_host_.GetFunc()->PostWork(ui_host_.GetContext(), inject_mouse, msg);
    }
}

// 主线程执行（PostWork 回调）
static void inject_mouse(void* ctx) noexcept {
    auto* msg = static_cast<UiMessageMouse*>(ctx);
    // ui_host_impl.dispatch_mouse 构造 UI_MESSAGE_MOUSE，
    // 调用插件注册的 UiMessageHandler.Process
    ui_host_impl_.dispatch_mouse(msg);
    delete msg;
}
```

**IUiHost 接口的作用**：`PostWork` 是 `IUiHost` 的跨线程调度 API。
- Client 使用 GKC 真实实现（底层是 eventfd / Win32 PostMessage）
- AppHost 使用 `ui_host_impl`（假实现，内部维护自己的任务队列）

两者对调用者接口完全相同，插件 `.so` 代码无需关心。

---

#### ⑤ ui_host_impl — 双向粘接层，拦截 DoDraw 捕获像素

插件的 `DoMouse` 修改内部状态后，`ui_host_impl` 主动触发 `DoDraw`，
这是捕获像素的关键时刻：

```cpp
// src/apphost/ui_host_impl.cpp
void UiHostImpl::trigger_draw(ui_rect dirty) noexcept {
    UiMessageDraw draw_info{};
    draw_info.pBuffer = window_.pixel_buf;   // 虚拟像素缓冲区（替代真实屏幕）
    draw_info.iWidth  = window_.width;
    draw_info.rcPaint = dirty;               // GKC 自维护的脏矩形，直接传入

    // 调用插件注册的 DoDraw：插件向 draw_info.pBuffer 写像素
    window_.handler.Process(window_.ctx, UI_MESSAGE_DRAW,
                            reinterpret_cast<uintptr>(&draw_info));

    // 插件写完后，ui_host_impl 截获 rcPaint 区域的 ARGB 字节
    pixel_capture(draw_info.pBuffer, window_.width, draw_info.rcPaint,
                  [this](const uint8_t* argb, size_t len, ui_rect rc) {
                      send_pixel_data(gateway_conn_, session_id_, rc, argb, len);
                  });
}
```

**ui_host_impl 的作用（双向）**：
- **插件 → 网络**：拦截 `DoDraw`，从 `pDraw->rcPaint` 读脏矩形，裁出像素，打包 `PIXEL_DATA` 发网络
- **网络 → 插件**：把网络 `INPUT_EVENT` 转成 `UI_MESSAGE_MOUSE/KEYBOARD`，
  调用插件的 `UiMessageHandler.Process`，触发 `DoMouse` / `DoKeyboard`

插件 `.so`（`demo_app.cpp`）完全不知道底层是真实屏幕还是网络，只按 GKC GUI API 编程。

---

#### ⑥ Gateway — 对称路由 PIXEL_DATA 回 Client

AppHost → Gateway 的路径与步骤 ② 完全对称：Gateway 对 AppHost 连接同样只做
`MessageParser` 定帧 + SessionId 路由，不解析像素内容。

---

#### ⑦ Client IoPool — MessageParser + MessageHandler 接收 PIXEL_DATA

```cpp
// plugins/client_viewer/client_viewer.cpp — 初始化时注册
handler_.on(CmdType::SESSION_ACK, [this](const Packet& p){ on_session_ack(p); });
handler_.on(CmdType::PIXEL_DATA,  [this](const Packet& p){ on_pixel_data(p);  });

void on_pixel_data(const Packet& pkt) {
    // 反序列化 PIXEL_DATA Body：
    // [frameSeq(4)][rectLeft(4)][rectTop(4)][rectRight(4)][rectBottom(4)][dataLen(4)][pixelData...]
    uint32_t rect_left  = read_be32(pkt.body.data() +  4);
    uint32_t rect_top   = read_be32(pkt.body.data() +  8);
    uint32_t rect_right = read_be32(pkt.body.data() + 12);
    uint32_t rect_bot   = read_be32(pkt.body.data() + 16);
    uint32_t data_len   = read_be32(pkt.body.data() + 20);
    const uint8_t* pixels = pkt.body.data() + 24;

    ui_rect rc{(int)rect_left, (int)rect_top, (int)rect_right, (int)rect_bot};
    renderer_.apply_dirty_rect(rc, pixels, data_len); // ← 写 PixelRenderer（mutex 保护）

    // 通知主线程重绘（GKC GUI API）
    ui_host_.GetFunc()->PostWork(ui_host_.GetContext(), trigger_redraw, this);
}
```

---

#### ⑧ Client 主线程 — DoDraw 贴像素到屏幕

```cpp
// plugins/client_viewer/client_viewer.cpp
void DoDraw(GKC::UiMessageDraw* pDraw) noexcept {
    // GKC GUI API：DoDraw 由 GKC 触发
    // pDraw->pBuffer = GKC 管理的屏幕共享内存（Wayland wl_shm / Win32 GDI）
    // pDraw->rcPaint = GKC 告知本次需重绘的区域
    renderer_.blit(pDraw->pBuffer, pDraw->iWidth, pDraw->rcPaint);
    // blit 只做 memcpy，把 PixelRenderer 缓冲区对应区域复制进去
    // GKC 随后将 pDraw->pBuffer 合成到 Wayland / Win32 窗口，用户看到更新
}
```

**GKC GUI API 在 Client 侧的作用**：`DoDraw` 只需做 `memcpy`，
GKC 负责所有窗口合成与刷新细节，Client 插件无任何平台代码。

---

### 11.3 五大组件总结

| 组件 | 所在位置 | 核心职责 |
|------|---------|---------|
| **GKC GUI API** (`ToplevelImpl<T>` / `DoDraw` / `DoMouse` / `DoKeyboard`) | client_viewer 插件（Client）；demo_app 插件（AppHost） | 统一抽象 OS GUI 事件（Wayland/Win32），开发者只实现 `DoXxx` 回调，GKC 负责平台适配和 `UiMessageHandler` 注册/分发 |
| **MessageParser** | 三个组件均有，每个 TCP 连接独立实例 | TCP 字节流 → 完整协议帧：内部状态机（READ_HEADER → VALIDATE_MAGIC → READ_BODY），`feed()` 推入字节，`next_packet()` 拉出帧，解决粘包/半包 |
| **MessageHandler** | AppHost、Client 有；Gateway 不需要（只做路由） | 协议帧 → 业务回调：包装 `MessageParser`，维护 `CmdType → HandlerFn` 映射，`feed()` 后自动 dispatch，调用者只需 `on(CmdType, fn)` 注册 |
| **GKC IUiHost** | Client 用 GKC 真实实现；AppHost 用假实现 | GUI 宿主接口（`Loop` / `PostWork` / `CreateToplevel` / `AddTimer`），插件与底层平台的唯一接触点，两端接口相同 |
| **ui_host_impl**（`IUiHost` 假实现） | AppHost（`src/apphost/ui_host_impl.cpp`） | 双向粘接层：① 插件 `DoDraw` → `pDraw->rcPaint` → `pixel_capture` → `PIXEL_DATA` → 网络；② 网络 `INPUT_EVENT` → `PostWork` → `dispatch_mouse/keyboard` → 插件 `DoMouse/DoKeyboard` |
