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

---

## 1. 项目简介

本项目是一个用 **C++17** 编写的、基于 **TCP** 的应用级 UI 虚拟化通信协议实现系统。

### 核心理念

"**计算上云、交互下沉**"——将计算密集型的业务逻辑与图形渲染交由服务端完成，客户端仅负责像素重构与输入事件采集，实现轻量化的远程应用交互。

### 它能做什么？

- **应用级 UI 虚拟化**：以独立应用（而非整个桌面）为粒度，实现远程界面的实时传输与交互，区别于 RDP/VNC 的桌面镜像模式。
- **像素矩阵差分传输**：AppHost 在服务端本地完成渲染，通过脏矩形差分检测仅传输变化区域的像素数据，大幅降低带宽消耗。
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
│   (FLTK 客户端)  │◄──────────────►│   (智能网关)      │◄──────────────►│  (应用宿主进程)   │
│                  │                │                  │                │                  │
│ · 像素重构(BitBlt)│   单端口接入    │ · Session 管理    │   动态路由      │ · 加载业务 .so    │
│ · 输入事件采集    │                │ · 动态路由转发     │                │ · 本地渲染        │
│ · 窗口管理       │                │ · 进程生命周期     │                │ · 脏矩形检测      │
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
主线程 (epoll/select)  ──► 监听客户端连接、I/O 多路复用
路由分发               ──► 解析 Session ID，精准路由数据包
进程管理               ──► fork/exec 拉起 AppHost，回收孤儿进程
心跳监测               ──► 检测客户端/AppHost 存活状态
```

**AppHost（应用宿主）：**
```
主线程                 ──► 加载业务 .so 插件 (dlopen/dlsym)
渲染线程               ──► 执行业务逻辑，写入像素缓冲区
差分检测线程           ──► 逐帧对比，定位脏矩形区域
数据发送线程           ──► 压缩像素数据，封包发往 Gateway
事件接收线程           ──► 接收并分发客户端输入事件
```

**Client（客户端）：**
```
主事件循环 (FLTK)      ──► 接收像素数据包，BitBlt 贴图重构界面
输入采集               ──► 捕获鼠标/键盘 Raw Input 事件
网络接收线程           ──► 从 Gateway 接收数据，解包分发
心跳发送               ──► 定期发送心跳维持会话
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
│   │   ├── protocol.h         # 消息类型枚举、协议头结构体
│   │   ├── protocol.cpp       # 序列化/反序列化实现
│   │   ├── message_parser.h   # TCP 粘包/半包状态机解析器
│   │   ├── message_parser.cpp
│   │   └── BUILD
│   │
│   ├── gateway/               # 智能网关（可执行程序）
│   │   ├── gateway.h          # 网关核心类声明
│   │   ├── gateway.cpp        # 网关核心逻辑
│   │   ├── session_manager.h  # Session 管理（路由表维护）
│   │   ├── session_manager.cpp
│   │   ├── process_manager.h  # AppHost 进程生命周期管理
│   │   ├── process_manager.cpp
│   │   ├── main.cpp           # 入口
│   │   └── BUILD
│   │
│   ├── apphost/               # 应用宿主（可执行程序，由 Gateway 动态拉起）
│   │   ├── apphost.h          # AppHost 核心类声明
│   │   ├── apphost.cpp        # AppHost 核心逻辑
│   │   ├── ui_host_impl.h     # IUiHost 接口的远程模式实现
│   │   ├── ui_host_impl.cpp
│   │   ├── pixel_buffer.h     # 像素缓冲区管理
│   │   ├── pixel_buffer.cpp
│   │   ├── dirty_rect_detector.h   # 脏矩形差分检测
│   │   ├── dirty_rect_detector.cpp
│   │   ├── plugin_loader.h    # .so/.dll 插件动态加载
│   │   ├── plugin_loader.cpp
│   │   ├── main.cpp           # 入口
│   │   └── BUILD
│   │
│   ├── client/                # 客户端（可执行程序）
│   │   ├── client.h           # 客户端核心类声明
│   │   ├── client.cpp         # 客户端核心逻辑
│   │   ├── window.h           # FLTK 窗口管理
│   │   ├── window.cpp
│   │   ├── input_handler.h    # 鼠标/键盘输入采集
│   │   ├── input_handler.cpp
│   │   ├── pixel_renderer.h   # 像素重构（BitBlt 贴图）
│   │   ├── pixel_renderer.cpp
│   │   ├── main.cpp           # 入口
│   │   └── BUILD
│   │
│   └── common/                # 公共工具库
│       ├── types.h            # 通用类型定义（ui_rect, ui_point, ColorQuad）
│       ├── byte_order.h       # 大端序/主机字节序转换工具
│       ├── logger.h           # 日志接口
│       ├── logger.cpp         # 日志实现
│       └── BUILD
│
├── tests/                     # 测试目录
│   └── BUILD
│
└── third_party/               # 第三方依赖
    ├── BUILD
    ├── GKC/                   # GKC (General Kind C++) 框架
    │   ├── public/include/
    │   │   ├── base/          # 基础框架 (GkcDef.h, GkcFrame.h)
    │   │   ├── sys/           # 系统工具 (GkcSys.h: 文件、流、线程)
    │   │   └── ui/            # UI 框架 (IUiHost, IUiWindow 接口定义)
    │   │       └── system/
    │   │           ├── host_types.h   # i_ui_host, i_ui_window 接口
    │   │           └── basic_types.h  # ui_rect, ui_point, ColorQuad 等类型
    │   └── ...
    └── GKC_BUILD/             # GKC 预编译产物
        └── release/bin/Release/
            ├── GkcSys.dll     # Windows 动态库
            └── GkcSys.lib     # Windows 导入库
```

> **构建系统**：Bazel 7.4.1，通过 `rules_foreign_cc` 集成 GKC 框架的 CMake 构建。
> 三个可执行目标：Gateway、AppHost、Client 分别独立编译。

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

```cpp
enum class CmdType : uint8_t {
    // 会话管理
    SPAWN_APP       = 0x01,  // 请求拉起 AppHost 进程
    SESSION_ACK     = 0x02,  // 会话建立确认，返回 Session ID
    HEARTBEAT       = 0x03,  // 心跳包

    // 输入事件
    MOUSE_EVENT     = 0x10,  // 鼠标事件（移动、点击、滚轮）
    KEYBOARD_EVENT  = 0x11,  // 键盘事件（按下/释放）

    // 像素传输
    DIRTY_RECT      = 0x20,  // 脏矩形像素数据
    FRAME_ACK       = 0x21,  // 帧确认（客户端已完成重绘）
    WINDOW_STATE    = 0x22,  // 窗口状态更新（尺寸、位置）

    // 控制指令
    APP_EXIT        = 0xF0,  // 应用退出通知
    ERROR           = 0xFF   // 错误消息
};
```

### 4.4 各类型载荷格式

**SPAWN_APP 请求载荷：**
```
┌──────────────────┬──────────────────────┐
│ appNameLength    │ appName              │
│ (4 bytes BE)     │ (variable, UTF-8)    │
└──────────────────┴──────────────────────┘
```

**鼠标事件（MOUSE_EVENT）载荷：**
```
┌──────────────┬──────────────────┬──────────────────┬──────────────────┐
│ eventType    │ x                │ y                │ timestamp        │
│ (1 byte)     │ (4 bytes BE)     │ (4 bytes BE)     │ (8 bytes BE)     │
└──────────────┴──────────────────┴──────────────────┴──────────────────┘
```

鼠标事件类型：`MOVE=0x01`、`LEFT_DOWN=0x02`、`LEFT_UP=0x03`、`RIGHT_DOWN=0x04`、`RIGHT_UP=0x05`、`SCROLL=0x06`

**键盘事件（KEYBOARD_EVENT）载荷：**
```
┌──────────────────┬──────────────┬──────────────────┐
│ keyCode          │ isPressed    │ timestamp        │
│ (2 bytes BE)     │ (1 byte)     │ (8 bytes BE)     │
└──────────────────┴──────────────┴──────────────────┘
```

**脏矩形像素数据（DIRTY_RECT）载荷：**
```
┌────────────┬────────────┬────────────┬────────────┬──────────┬──────────┬──────────┐
│ frameSeq   │ rectX      │ rectY      │ rectW      │ rectH    │ dataLen  │ pixelData│
│ (4 bytes)  │ (4 bytes)  │ (4 bytes)  │ (4 bytes)  │ (4 bytes)│ (4 bytes)│(variable)│
└────────────┴────────────┴────────────┴────────────┴──────────┴──────────┴──────────┘
```

- `frameSeq`：帧序列号，客户端可据此在丢包环境下执行有选择的脏矩形丢弃
- `rectX/Y/W/H`：脏矩形在窗口中的坐标与尺寸
- `pixelData`：该区域的 ARGB 像素矩阵数据（可压缩）

**窗口状态（WINDOW_STATE）载荷：**
```
┌──────────────┬──────────────┬──────────────┬──────────────┐
│ windowWidth  │ windowHeight │ posX         │ posY         │
│ (4 bytes)    │ (4 bytes)    │ (4 bytes)    │ (4 bytes)    │
└──────────────┴──────────────┴──────────────┴──────────────┘
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

IUiHost 是系统的核心抽象接口，实现业务逻辑对底层图形库的透明化：

```cpp
// GKC::i_ui_host — 窗口宿主抽象接口
class i_ui_host {
public:
    virtual int  Loop()                  noexcept = 0;  // 主事件循环
    virtual void Quit()                  noexcept = 0;  // 退出事件循环
    virtual i_ui_window* Create(int iType) noexcept = 0;  // 创建窗口
    virtual void Destroy(i_ui_window* p) noexcept = 0;  // 销毁窗口
};

// GKC::i_ui_window — 窗口接口
class i_ui_window {
public:
    virtual void GetInfo(ui_window_info& info) noexcept = 0;
};

// 窗口类型
enum {
    UW_TYPE_TOPLEVEL = 0,  // 顶层窗口
    UW_TYPE_DIALOG,        // 对话框
    UW_TYPE_POPUP          // 弹出窗口
};
```

**接口透明化原理**：业务逻辑（.so/.dll 插件）仅调用 IUiHost 接口进行绑定、渲染相关的操作。当运行在本地模式时，IUiHost 直接驱动本地图形库（如 GTK/Win32）；当运行在远程模式时，AppHost 实现的 IUiHost 将渲染结果写入内存像素缓冲区，再通过协议传输至客户端。业务代码无需任何修改。

### 5.2 IGuiApplication 接口

```cpp
// 业务应用接口
class IGuiApplication {
public:
    virtual bool Initialize(IUiHost* pHost) noexcept = 0;  // 初始化，注入 IUiHost
    virtual int  Run()                      noexcept = 0;  // 运行业务逻辑
    virtual void Cleanup()                  noexcept = 0;  // 清理资源
};
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

### 5.6 输入事件

```cpp
struct MouseEvent {
    uint8_t   eventType;    // MOVE/LEFT_DOWN/LEFT_UP/RIGHT_DOWN/RIGHT_UP/SCROLL
    int32_t   x, y;         // 坐标
    uint64_t  timestamp;    // 时间戳（微秒）
};

struct KeyboardEvent {
    uint16_t  keyCode;      // 按键码
    uint8_t   isPressed;    // 1=按下, 0=释放
    uint64_t  timestamp;    // 时间戳（微秒）
};
```

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

AppHost 是服务端的核心执行单元，负责加载业务插件、执行渲染、检测差分并发送像素数据。

#### 启动流程

```
1. 由 Gateway 通过 fork/exec 拉起
2. 通过 dlopen() 加载指定的业务 .so 插件
3. 通过 dlsym() 解析 IGuiApplication 入口符号
4. 创建 IUiHost 实现实例，注入业务逻辑
5. 初始化虚拟像素缓冲区
6. 启动渲染循环与差分检测
7. 建立与 Gateway 的 TCP 连接
```

#### 像素差分传输流程

```
业务逻辑调用 IUiHost 接口
    ↓
AppHost 在本地完成真实渲染 → 写入内存像素缓冲区（当前帧）
    ↓
逐帧差分检测：对比当前帧与上一帧 → 定位脏矩形区域
    ↓
提取脏矩形内的像素数据 → 压缩
    ↓
封装为 DIRTY_RECT 协议包（含帧序列号、矩形坐标、像素数据）
    ↓
通过 TCP 发送至 Gateway → 路由到对应 Client
```

#### 接收处理

- 收到 `MOUSE_EVENT`：转换为业务逻辑可识别的输入事件，注入 IUiHost 事件队列
- 收到 `KEYBOARD_EVENT`：同上，支持多键组合与拖拽等复杂操作
- 收到 `APP_EXIT`：清理资源，退出进程

### 6.3 Client（客户端）

**目录**：`src/client/`

客户端基于 **FLTK (Fast Light Toolkit)** 图形库构建，负责像素重构与输入采集。

#### 启动流程

```
1. 初始化 FLTK 窗口
2. 建立与 Gateway 的 TCP 连接
3. 发送 SPAWN_APP 请求
4. 收到 SESSION_ACK，获得 Session ID
5. 进入主事件循环
```

#### 像素重构流程

```
从 Gateway 接收 DIRTY_RECT 数据包
    ↓
解析帧序列号、脏矩形坐标与像素数据
    ↓
（可选）丢弃过期帧的脏矩形（帧序列号过时）
    ↓
解压像素数据
    ↓
通过 BitBlt（内存贴图）直接写入窗口对应区域
    ↓
触发局部重绘，完成界面实时重构
```

> 客户端**无需任何本地绘图逻辑**，仅做像素搬运。

#### 输入事件采集

- 捕获 Raw Input 原始鼠标/键盘事件
- 标准化封装（坐标偏移、按键码、时间戳）
- 附加 Session ID 后发送至 Gateway 透传

### 6.4 网络传输层

网络 I/O 直接使用 **GKC `IoPool`**（`GKC/RT/GkcSys/public/_GkcSys.h`），不设独立的 `src/network/` 模块。

- Linux 底层：epoll 事件驱动
- Windows 底层：IOCP 事件驱动
- 接口统一：`StartListen` / `StartConnect` / `BeginInput` / `DisableHandle`

各组件用法：Gateway 调用 `StartListen` 监听客户端并 `StartConnect` 对接 AppHost；AppHost / Client 均调用 `StartConnect` 连接 Gateway。

### 6.5 协议层

**目录**：`src/protocol/`

负责消息的**序列化**（结构体 → 字节流）和**反序列化**（字节流 → 结构体）。

- 所有多字节整数在序列化时转换为大端序（网络字节序），反序列化时转回主机字节序
- 通过状态机模式实现 TCP 粘包/半包的完整解析
- Magic Number 校验确保协议一致性

---

## 7. 关键算法与机制

### 7.1 脏矩形差分检测

AppHost 维护两帧像素缓冲区（当前帧与前一帧），逐帧对比定位变化区域：

```
1. 业务逻辑渲染完成 → 当前帧写入缓冲区 A
2. 逐行扫描对比缓冲区 A 与缓冲区 B（前一帧）
3. 记录所有发生变化的像素坐标
4. 将变化像素聚合为最小外接矩形（脏矩形）
5. 可进一步拆分为多个不相交的脏矩形以减少冗余传输
6. 提取脏矩形内像素数据 → 压缩 → 封包发送
7. 交换缓冲区：A → B，准备下一帧
```

**优势**：相较于全量位图/视频流传输（如 RDP/VNC），仅传输实际变化的像素区域，在界面变化较少时（如文本编辑、表单操作）可将带宽降低一个数量级。

### 7.2 帧序列号与选择性丢弃

每个 DIRTY_RECT 包携带帧序列号（frameSeq），客户端据此处理：

- 正常情况：按序重绘
- 网络拥塞/丢包：如果收到更新帧的脏矩形，可安全丢弃旧帧的待处理矩形（因为新帧已覆盖该区域）
- 时间戳同步：输入事件携带时间戳，用于服务端精确回放

### 7.3 动态进程调度

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
- FLTK（客户端图形库）
- GKC 框架（git submodule，自动拉取）
- Linux：POSIX threads、epoll、dlopen
- Windows：Winsock2、LoadLibrary

### 构建

```bash
# 构建全部目标
bazel build //...

# 单独构建
bazel build //src/gateway:gateway
bazel build //src/apphost:apphost
bazel build //src/client:client
```

### 运行顺序

```bash
# 1. 启动网关（必须最先启动）
./bazel-bin/src/gateway/gateway

# 2. 客户端连接网关，发送 SPAWN_APP 请求后
#    网关自动拉起 AppHost 进程，无需手动启动
./bazel-bin/src/client/client
```

> AppHost 进程由 Gateway 动态管理，无需手动启动。

---

## 10. 设计亮点与已知局限

### 设计亮点

| 亮点 | 说明 |
|------|------|
| **应用级虚拟化** | 以独立应用为粒度，区别于 RDP/VNC 的桌面镜像模式 |
| **IUiHost 接口解耦** | 业务逻辑与渲染引擎彻底分离，支持本地/远程模式无缝切换 |
| **像素差分传输** | 脏矩形检测 + 压缩，大幅降低带宽消耗 |
| **智能网关调度** | Session 管理 + 动态路由 + 进程生命周期，支持水平扩展 |
| **TCP 可靠传输** | 自定义二进制协议，状态机解包，解决粘包/半包问题 |
| **进程级隔离** | fork/exec 独立进程空间，单点故障不影响全局 |
| **跨平台支持** | 传输层封装 Windows/Linux 差异，GKC 框架统一 API |
| **帧序列号机制** | 支持客户端在弱网下选择性丢弃过期帧 |

### 已知局限

| 局限 | 说明 |
|------|------|
| 像素压缩算法 | 当前使用基础压缩，未集成硬件加速编码 |
| 音频传输 | 协议暂不支持音频流传输 |
| 多显示器 | 暂不支持多显示器场景 |
| GPU 加速 | 服务端渲染未利用 GPU 硬件加速 |
