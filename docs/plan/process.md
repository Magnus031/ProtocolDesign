# ProtocolDesign — MVP 迭代开发计划

## 总体策略

采用**由内向外、由简至繁**的迭代路径：先打通协议解析，再逐步建立网络基础设施（MessageHandler、多线程模型），然后接入 AppHost 插件与渲染，最后完成客户端闭环。每个里程碑都必须能**独立编译、独立测试、有明确的通过标准**，不依赖后续里程碑的完成。

```
M1 协议层 ✅ → M2 IoPool+MessageHandler → M3a Gateway路由 → M3b 进程管理
                                                                  ↓
                      M6 GKC Client插件 ← M5 完整多线程闭环 ← M4 AppHost插件+ui_host_impl
```

**关键约束**：
- 项目全程使用 GKC `IoPool` 处理网络 I/O，**不设** `src/network/` 模块
- 三个组件（Client / Gateway / AppHost）全部基于 GKC，无 FLTK 依赖
- 脏矩形由 GKC `DoDraw` 回调的 `pDraw->rcPaint` 直接提供，**不做逐帧像素比对**
- Client 与 AppHost 对称：均为通用插件加载器，业务逻辑在 `.so`（Linux）/ `.dll`（Windows）中

---

## M1 — 协议层与 TCP 粘包解析 ✅ 已完成

**目标**：在没有任何业务逻辑的情况下，用单元测试验证协议的序列化/反序列化和粘包解析状态机完全正确。

### 1.1 实现的文件

| 文件 | 内容 |
|------|------|
| `src/common/byte_order.h` | 跨平台大端/小端转换宏（`BE32`, `BE16`, `LE32`, `LE16`）|
| `src/protocol/protocol.h` | `CmdType` 枚举、`Header` 结构体（13 字节定长头）、`Packet` 载体 |
| `src/protocol/protocol.cpp` | `serialize_header()` / `deserialize_header()` 序列化实现 |
| `src/protocol/message_parser.h` | `MessageParser` 类声明（状态机，Pull 模型） |
| `src/protocol/message_parser.cpp` | 状态机：`WAIT_HEADER` → `WAIT_BODY` → `POISONED` |

### 1.2 协议头格式（固定 13 字节）

```
 0       1       2       3       4       5       6       7
 +-------+-------+-------+-------+-------+-------+-------+-------+
 |  Magic (2B)   | SessionID (4B)                                 |
 +---------------+-----------------------------------------------+
 |  CmdType (1B) |  BodyLength (4B)                               |
 +---------------+-----------------------------------------------+
 |  BodyLength cont.     | Reserved (2B)                          |
 +-----------------------------------------------+---------------+
```

- Magic：固定 `0xBEEF`，大端序
- SessionID：4 字节无符号整数，大端序
- CmdType：1 字节枚举
- BodyLength：4 字节无符号整数，大端序
- Reserved：2 字节预留

### 1.3 CmdType 枚举

```cpp
// 与 src/protocol/protocol.h 完全对应
enum class CmdType : uint8_t {
    HEARTBEAT     = 0x01,  // 心跳，Body 为空
    SPAWN_APP     = 0x02,  // Client → Gateway：请求启动 AppHost
    SESSION_ACK   = 0x03,  // Gateway → Client：会话建立确认

    PIXEL_DATA    = 0x10,  // AppHost → Client：脏矩形像素数据

    INPUT_EVENT   = 0x20,  // Client → AppHost：鼠标或键盘（Body 首字节 eventType 区分）

    CLOSE_SESSION = 0xFE,  // 任意方向：关闭会话
    ERROR_RESP    = 0xFF,  // 任意方向：错误通知
};
```

### 1.4 完成标准（已通过）

- [x] `bazel test //src/protocol:message_parser_test` — 15 个测试用例全部 PASSED
- [x] `bazel test //src/protocol:protocol_test` — Header 序列化/反序列化往返测试通过
- [x] MessageParser 接口为 Pull 模型（`next_packet()`），POISONED 终态

---

## M2 — IoPool + MessageHandler 基础设施

**目标**：建立所有三个组件共用的网络事件处理骨架。引入 `MessageHandler`——坐在 IoPool 回调与业务逻辑之间的 CmdType 分发器。AppHost 监听端口，接受连接，发送静态 `PIXEL_DATA` 包；TestClient 使用阻塞 socket 验证收发。

### 2.1 设计：per-connection 上下文（ConnContext 模式）

**这是 M2 的核心设计约束**，三个组件（AppHost / Gateway / Client）全部遵守。

IoPool 不管理连接上下文的生命周期。每个连接需要一个 `ConnContext` 结构体，
用 `free_list<ConnContext>` + 原子自旋锁统一管理，**不使用** `new`/`delete`：

```cpp
// Per-connection context: inherits node_base for free_list<> linkage.
struct ConnContext : public node_base {
    uintptr      id_          = 0;       // IoPool 连接句柄
    ConnContext* next_active_ = nullptr; // 侵入式活跃链表（关闭时清理用）
    _IoFunc      io_func_;              // 嵌入式回调，Exec 在构造时设置
    MessageParser parser_;              // 每连接独立的粘包解析状态
    Impl*        server_      = nullptr; // 反向指针

    ConnContext() noexcept { io_func_.Exec = on_io_event; }

    // ConnContext 把自己作为 pIoContext，所有事件直接回调到此函数
    static int on_io_event(void* ctx, int type, uintptr uparam) noexcept;
};

// 连接池 + 自旋锁（0=未持有，1=持有）
free_list<ConnContext> conn_pool_;
ConnContext*           active_head_ = nullptr;
volatile int           conn_lock_   = 0;
```

**分配节点的顺序**（严格遵守 `node_helper::ConstructNode` 范式）：
```
FetchFreeNode(conn)     // 查看空闲链表头，不摘除
call_constructor(*conn) // 就地构造（先于 Pick，构造失败则节点不丢失）
PickFreeNode()          // 摘除节点（只有构造成功才执行）
```

**绑定到 IoPool**（在 `IO_TYPE_ACCEPT_INIT` 中）：
```cpp
conn->id_ = conn_id;
pool_.GetFunc()->SetHandleFunc(
    pool_.GetContext(), conn_id, conn->io_func_, conn, cancelled);
// 此后所有事件：on_io_event(conn, type, uparam)
```

---

### 2.2 设计：MessageHandler

`MessageHandler` 是 IoPool 回调内部的分发层，负责把 `MessageParser` 解出的完整包按 `CmdType` 路由到对应处理函数：

```cpp
// src/common/message_handler.h
class MessageHandler {
public:
    using HandlerFn = std::function<void(const Packet&)>;

    // Register a handler for a specific CmdType.
    void register_handler(CmdType cmd, HandlerFn fn);

    // Feed raw bytes into the internal MessageParser, then dispatch any
    // complete packets to registered handlers. Called from IoPool callback.
    void feed(const uint8_t* data, size_t len);

    // Reset the internal parser (e.g. on new connection).
    void reset();

private:
    MessageParser parser_;
    std::unordered_map<uint8_t, HandlerFn> handlers_;
};
```

**在 ConnContext 中的使用（on_io_event 内部）：**

```cpp
case IO_TYPE_RECEIVED: {
    auto* info = reinterpret_cast<_IoRecvInfo*>(uparam);
    conn->parser_.feed(
        reinterpret_cast<const uint8_t*>(info->p), info->len);
    // 或使用 MessageHandler：conn->handler_.feed(info->p, info->len);
    break;
}
case IO_TYPE_BEFORE_CLOSE:
    conn->server_->release_conn(conn);  // 归还到 free_list，不 delete
    break;
```

### 2.3 需要实现的文件

| 文件 | 内容 |
|------|------|
| `src/common/message_handler.h/.cpp` | `MessageHandler`：包装 MessageParser，提供 CmdType 注册/分发 |
| `src/apphost/apphost.h/.cpp` | IoPool `StartListen`；`ConnContext`（继承 `node_base`，内嵌 `_IoFunc`）；`free_list<ConnContext>` + 自旋锁；注册 HEARTBEAT / CLOSE_SESSION 处理器；连接建立后发送静态 PIXEL_DATA |
| `src/apphost/pixel_capture.h/.cpp` | 工具函数：从 pBuffer 裁出指定矩形区域的像素字节，打包为 PIXEL_DATA Body |
| `src/apphost/main.cpp` | 解析 `--port` 参数，启动 AppHost |
| `src/apphost/apphost_test.cpp` | 集成测试：TestClient 用阻塞 POSIX socket 连接，断言 PIXEL_DATA 内容 |

> **注意**：`DirtyRectDetector`（逐帧比对）**不在本 Milestone 实现，也不在后续 Milestone 实现**。
> `pixel_capture` 只负责从已有的 pBuffer 中按矩形坐标裁出像素，不做帧间比较。

### 2.4 AppHost 静态帧发送流程

```
1. IoPool StartListen(port)
2. IO_TYPE_ACCEPT_INIT：alloc_conn() 取 ConnContext，SetHandleFunc 绑定 conn->io_func_ + conn
3. on_io_event 收到 IO_TYPE_ACCEPTED：BeginInput/EndInput 发送静态 PIXEL_DATA 包
4. on_io_event 收到 IO_TYPE_RECEIVED：MessageParser.feed() → 分发
   - HEARTBEAT → 回复 HEARTBEAT
   - CLOSE_SESSION → DisableHandle
5. IO_TYPE_BEFORE_CLOSE：release_conn(conn)，归还到 free_list
```

### 2.5 TestClient（阻塞 socket，无 IoPool）

```
1. connect("127.0.0.1", port)
2. recv() 循环 → MessageParser.feed() → next_packet()
3. 断言收到 PIXEL_DATA，像素值正确
4. 发送 HEARTBEAT，断言回复
5. 发送 CLOSE_SESSION
```

### 2.6 测试要求

| 测试用例 | 期望结果 |
|----------|----------|
| `ConnectAndReceiveFrame` | 收到完整 PIXEL_DATA，像素断言通过 |
| `HeartbeatRoundTrip` | 发送 HEARTBEAT，200ms 内收到回复 |
| `CleanShutdown` | 发送 CLOSE_SESSION，AppHost 正常退出 |
| `MessageHandlerDispatch` | 单元测试：注册两个处理器，feed 对应包，各自被调用一次 |

**通过标准**：`bazel test //src/apphost:apphost_test //src/common:message_handler_test` 全部 PASSED。

---

## M3a — Gateway 路由（纯路由，手动启动 AppHost）

**目标**：引入 Gateway，专注验证路由正确性。Gateway 同样使用 `MessageHandler` 分发，按 SessionID 在 Client 和 AppHost 之间透传数据包。

### 3a.1 设计约束：per-connection 上下文必须遵循 ConnContext 模式

Gateway 有两类连接（Client 侧、AppHost 侧），各需要独立的上下文类型，
但两者均必须遵循 M2 建立的 **`free_list<T> + 自旋锁 + 侵入式活跃链表`** 模式：

```cpp
// Client 侧连接上下文
struct ClientSession : public node_base {
    uintptr        id_           = 0;
    ClientSession* next_active_  = nullptr;
    _IoFunc        io_func_;
    MessageParser  parser_;
    uint32_t       session_id_   = 0;
    uintptr        apphost_conn_ = 0;   // 对应的 AppHost 连接句柄
    Impl*          server_       = nullptr;
    ClientSession() noexcept { io_func_.Exec = on_client_event; }
};

// AppHost 侧连接上下文
struct AppHostSession : public node_base {
    uintptr          id_          = 0;
    AppHostSession*  next_active_ = nullptr;
    _IoFunc          io_func_;
    MessageParser    parser_;
    uint32_t         session_id_  = 0;
    uintptr          client_conn_ = 0;  // 对应的 Client 连接句柄
    Impl*            server_      = nullptr;
    AppHostSession() noexcept { io_func_.Exec = on_apphost_event; }
};
```

### 3a.2 需要实现的文件

| 文件 | 内容 |
|------|------|
| `src/gateway/session_manager.h/.cpp` | `SessionTable`：`SessionID ↔ (client_conn_id, apphost_conn_id)` 双向映射 |
| `src/gateway/gateway.h/.cpp` | IoPool 监听两个端口（Client 端口 19000、AppHost 端口 19001）；`free_list<ClientSession>` + `free_list<AppHostSession>`；MessageHandler 按 SessionID 路由 |
| `src/gateway/main.cpp` | Gateway 入口 |
| `src/gateway/gateway_test.cpp` | 集成测试：2 个 TestClient + 2 个手动 TestAppHost，并发路由验证 |

### 3a.3 Gateway 的 MessageHandler 注册模式

Gateway 有两类连接（Client 侧、AppHost 侧），各注册各自的处理器：

```
Client 侧连接的 MessageHandler：
  SPAWN_APP      → 分配 SessionID，等待 AppHost 连接
  HEARTBEAT      → 回复 HEARTBEAT
  MOUSE_EVENT    → 查路由表，转发给对应 apphost_conn
  KEYBOARD_EVENT → 同上
  CLOSE_SESSION  → 向对端发 CLOSE_SESSION，清理 Session

AppHost 侧连接的 MessageHandler：
  SESSION_ACK    → （AppHost 启动后发来，完成握手）
  PIXEL_DATA     → 查路由表，转发给对应 client_conn
  HEARTBEAT      → 回复
  CLOSE_SESSION  → 向对端发 CLOSE_SESSION，清理 Session
```

### 3a.4 测试要求

| 测试用例 | 期望结果 |
|----------|----------|
| `SingleClientFlow` | SPAWN_APP → SESSION_ACK → PIXEL_DATA，全流程通过 |
| `TwoClientsIsolation` | 两个 Client 并发，SessionID 独立，数据不串包 |
| `ClientDisconnect` | Client 断开，Gateway 向 AppHost 发 CLOSE_SESSION，Session 清理完毕 |

**通过标准**：`bazel test //src/gateway:gateway_test` 全部 PASSED。

---

## M3b — 进程管理 + 心跳 + 崩溃恢复

**目标**：在 M3a 路由基础上加入 AppHost 进程自动拉起与回收、心跳超时检测。

### 3b.1 需要实现的文件

| 文件 | 内容 |
|------|------|
| `src/gateway/process_manager.h/.cpp` | `fork/exec` 拉起 AppHost，记录 PID，`waitpid` 回收，防僵尸 |
| `src/gateway/gateway.h/.cpp`（更新） | 集成 ProcessManager；心跳超时（90s）通过 WorkPool 慢速池定时检查 |
| `src/gateway/gateway_lifecycle_test.cpp` | 生命周期专项测试 |

### 3b.2 AppHost 启动参数规范

```bash
./apphost \
  --session-id=<SessionID 十六进制> \
  --gateway-host=127.0.0.1 \
  --gateway-port=19001 \
  --plugin=./plugins/demo_app/libdemo_app.so \
  --width=800 \
  --height=600
```

### 3b.3 测试要求

| 测试用例 | 期望结果 |
|----------|----------|
| `AutoSpawnAppHost` | Client 发 SPAWN_APP，Gateway 自动拉起 AppHost，Client 收到 SESSION_ACK |
| `AppHostCrashRecovery` | kill AppHost，Gateway 检测断开，发 ERROR_RESP，无僵尸进程 |
| `GatewayShutdown` | SIGTERM，优雅关闭所有 Session，回收所有 AppHost 进程 |
| `HeartbeatTimeout` | Client 停止心跳 90s，Gateway 主动断开并回收 AppHost |

**通过标准**：`bazel test //src/gateway:gateway_lifecycle_test` 全部 PASSED。

---

## M4 — AppHost 插件系统 + ui_host_impl（rcPaint 直接传输）

**目标**：AppHost 通过 `plugin_loader` 加载业务 `.so` 插件，`ui_host_impl` 实现"假的" GKC `IUiHost`，拦截 `DoDraw()` 回调，直接使用 `pDraw->rcPaint` 和 `pDraw->pBuffer` 打包 PIXEL_DATA 发出。**不做逐帧像素比对**。

### 4.1 脏矩形传输的新设计

GKC 的 `DoDraw()` 回调已经携带了当次重绘的脏矩形：

```
插件调用（或 GKC 事件驱动触发）DoDraw(pDraw)
    pDraw->rcPaint  = 本次脏矩形区域（GKC 自维护）
    pDraw->pBuffer  = 整个窗口像素缓冲区
    pDraw->iWidth/iHeight = 窗口尺寸

ui_host_impl 捕获：
    pixel_capture(pDraw->pBuffer, pDraw->iWidth, pDraw->rcPaint)
        → 裁出 rcPaint 区域的 ARGB 字节
        → 打包 PIXEL_DATA Body：
          [frameSeq(4)][rcLeft(4)][rcTop(4)][rcRight(4)][rcBottom(4)][dataLen(4)][pixelData...]
        → WorkPool 快速池：BeginInput/EndInput 发往 Gateway
```

因为触发 `DoDraw` 的原因（鼠标事件、键盘事件、计时器等）就决定了哪块区域需要重绘，`rcPaint` 天然是最小更新区域，**无需保存上一帧做比对**。

### 4.2 需要实现/修改的文件

| 文件 | 内容 |
|------|------|
| `src/apphost/ui_host_impl.h/.cpp` | 实现 GKC `IUiHost` 接口：拦截 DoDraw，调用 pixel_capture，经 WorkPool 异步发 PIXEL_DATA |
| `src/apphost/pixel_capture.h/.cpp` | `capture_rect(pBuffer, width, rcPaint)` → `std::vector<uint8_t>` 像素字节 |
| `src/apphost/plugin_loader.h/.cpp` | `dlopen` 加载 `.so`，`dlsym("sa_ui_main")`，调用 `Exec(g_ui_host, args)` |
| `src/apphost/apphost.h/.cpp`（更新） | 集成 plugin_loader；移除静态帧逻辑；输入事件处理 stub（M5 完善） |
| `plugins/demo_app/demo_app.cpp` | 示例插件：创建 Toplevel，DoDraw 画纯色背景，DoClose 退出 |
| `plugins/demo_app/BUILD` | 构建为 `.so` 共享库 |
| `src/apphost/m4_plugin_test.cpp` | 集成测试：加载插件，TestClient 收到 PIXEL_DATA |

> `src/apphost/dirty_rect_detector.h/.cpp` **在本 Milestone 删除**，不再使用。
> `src/apphost/pixel_buffer.h/.cpp` **简化**：不再需要双帧（prev/current）存储，仅保留单帧辅助结构（如有必要）。

### 4.3 多线程模型（本 Milestone 明确）

```
主线程
  └── 加载 .so → plugin.Exec(g_ui_host, args) → GuiHelper::Loop()
         │
         ├── DoDraw() 回调（GKC 主线程触发）
         │     └── ui_host_impl 捕获 rcPaint + 像素
         │           └── _WorkPool_Submit(false, send_pixel_task, ctx)
         └── DoClose() → GuiHelper::Quit()

WorkPool 快速池线程
  └── send_pixel_task：BeginInput/EndInput 发 PIXEL_DATA

IoPool 线程
  └── IO_TYPE_RECEIVED → MessageHandler.feed()
        └── 处理器注册的回调（HEARTBEAT / CLOSE_SESSION）
              └── 需要更新 UI 状态时：g_ui_host.GetFunc()->PostWork(...)
```

### 4.4 测试要求

| 测试用例 | 期望结果 |
|----------|----------|
| `PluginLoads` | `plugin_loader` 加载 `demo_app.so`，`sa_ui_main()` 返回非空，`Exec()` 不 crash |
| `PixelDataReceived` | TestClient 连接，收到 PIXEL_DATA，rcPaint 坐标合法，像素值与 demo 插件一致 |
| `PixelCaptureUnit` | 单元测试：`capture_rect` 裁出正确的子矩形字节 |

**通过标准**：`bazel test //src/apphost:m4_plugin_test //src/apphost:pixel_capture_test` 全部 PASSED。

---

## M5 — 完整输入事件闭环 + 多线程 PostWork 模型

**目标**：打通 `INPUT_EVENT`（鼠标和键盘）从 Client → Gateway → AppHost → 插件的完整路径。在 AppHost 中明确实现 IoPool 线程 → `PostWork()` → 主线程 → 插件事件注入的多线程安全模式。

### 5.1 输入事件协议 Body 格式

所有输入事件共用 `CmdType::INPUT_EVENT = 0x20`，Body 首字节 `eventType` 区分鼠标与键盘：

**鼠标事件 Body（13 字节，eventType = 0x01~0x06）：**

```
┌──────────────┬──────────────────┬──────────────────┬──────────────────┐
│ eventType    │ x                │ y                │ timestamp        │
│ (1 byte)     │ (4 bytes BE)     │ (4 bytes BE)     │ (8 bytes BE)     │
└──────────────┴──────────────────┴──────────────────┴──────────────────┘
eventType: MOVE=0x01, LEFT_DOWN=0x02, LEFT_UP=0x03,
           RIGHT_DOWN=0x04, RIGHT_UP=0x05, SCROLL=0x06
```

**键盘事件 Body（11 字节，eventType = 0x10~0x11）：**

```
┌──────────────┬──────────────────┬──────────────────┐
│ eventType    │ keyCode          │ timestamp        │
│ (1 byte)     │ (2 bytes BE)     │ (8 bytes BE)     │
└──────────────┴──────────────────┴──────────────────┘
eventType: KEY_DOWN=0x10, KEY_UP=0x11
keyCode: GKC KB_* 枚举值，直接从 DoKeyboard(pKb->btKey) 填入，无需映射
```

### 5.2 AppHost 输入事件注入：PostWork 模式

IoPool 收到 `INPUT_EVENT` 后，**不能**直接操作插件（插件在主线程），必须用 `PostWork` 投递：

```
IoPool 线程
  IO_TYPE_RECEIVED → MessageHandler.feed()
    INPUT_EVENT 处理器：
      1. 读取 Body[0]（eventType），判断是鼠标还是键盘
      2a. 鼠标（0x01~0x06）：反序列化 → UiMessageMouse
          PostWork(inject_mouse, new UiMessageMouse{...})
      2b. 键盘（0x10~0x11）：反序列化 → UiMessageKeyboard
          PostWork(inject_keyboard, new UiMessageKeyboard{...})

主线程（PostWork 回调）
  inject_mouse(pCtx):
      auto* pMouse = static_cast<UiMessageMouse*>(pCtx);
      ui_host_impl_.dispatch_mouse(pMouse);   // 注入插件 DoMouse()
      delete pMouse;
  inject_keyboard(pCtx): 同理，注入 DoKeyboard()
```

### 5.3 需要实现/修改的文件

| 文件 | 内容 |
|------|------|
| `src/protocol/protocol.h/.cpp`（确认） | INPUT_EVENT Body 的鼠标/键盘子格式序列化/反序列化 |
| `src/apphost/ui_host_impl.h/.cpp`（更新） | 新增 `dispatch_mouse()` / `dispatch_keyboard()`：构造 ui_message_* 并调用插件的窗口消息处理器 |
| `src/apphost/apphost.h/.cpp`（更新） | MessageHandler 注册 INPUT_EVENT 处理器；内部按 eventType 分流；PostWork 注入主线程 |
| `plugins/demo_app/demo_app.cpp`（更新） | DoMouse：鼠标点击改变背景色；DoKeyboard：Space 键切换颜色 |
| `src/apphost/m5_input_test.cpp` | 集成测试：经 Gateway 发送 INPUT_EVENT（鼠标），验证 PIXEL_DATA 像素变化 |

### 5.4 测试要求

| 测试用例 | 期望结果 |
|----------|----------|
| `MouseClickChangesPixel` | TestClient 发 MOUSE_EVENT(DOWN, x=100, y=100)，收到 PIXEL_DATA，对应区域颜色变化 |
| `KeyboardEventRouted` | 发 KEYBOARD_EVENT(KB_Space)，插件响应，像素变化 |
| `NoInputNoPixelData` | 不发输入，AppHost 不发 PIXEL_DATA（静止不重传） |
| `PostWorkThreadSafety` | 快速连续发 100 个 MOUSE_EVENT，全部被正确处理，无 crash 无数据竞争 |

**通过标准**：`bazel test //src/apphost:m5_input_test` 全部 PASSED。

---

## M6 — GKC Client（客户端插件加载器）

**目标**：Client 与 AppHost 完全对称——也是一个通用插件加载器。Client 可执行程序加载 `.so`（Linux）/ `.dll`（Windows）客户端插件，插件负责创建 GKC 窗口、渲染像素、采集输入并发送。完成完整端到端可视化测试。

### 6.1 Client 插件接口

Client 插件使用与 AppHost 插件相同的 GKC 标准接口：

```cpp
// plugins/client_viewer/client_viewer.cpp
// Client 插件同样导出 sa_ui_main，Client 可执行程序通过 dlopen/LoadLibrary 加载

extern "C" GKC::SA_UIMain* sa_ui_main();
```

Client 可执行程序传入的 `IUiHost` 是真实的 GKC UIHost（Wayland / Win32），而不是像 AppHost 那样的假实现。插件在 `Exec()` 内：
1. 从 `args` 解析 Gateway 地址、要启动的 AppHost 插件名
2. 创建 `ToplevelImpl` 窗口
3. 通过 IoPool 连接 Gateway，发送 `SPAWN_APP`
4. 在 IoPool 回调中（通过 `MessageHandler`）接收 `PIXEL_DATA`，写入本地缓冲区，`PostWork()` 触发重绘
5. 在 `DoDraw` 中把缓冲区 `memcpy` 到 `pDraw->pBuffer`
6. 在 `DoMouse` / `DoKeyboard` 中打包事件发往 Gateway
7. 进入 `GuiHelper::Loop()`

### 6.2 需要实现的文件

| 文件 | 内容 |
|------|------|
| `src/client/client.h/.cpp` | IoPool `StartConnect` 连接 Gateway；MessageHandler 注册 SESSION_ACK / PIXEL_DATA / ERROR_RESP 处理器 |
| `src/client/plugin_loader.h/.cpp` | `dlopen`（Linux）/ `LoadLibrary`（Windows）加载 `.so`/`.dll`，`dlsym("sa_ui_main")` |
| `src/client/pixel_renderer.h/.cpp` | 线程安全像素缓冲区（mutex 保护），支持脏矩形局部写入；供插件使用 |
| `src/client/main.cpp` | 解析 `--plugin`、`--gateway-host`、`--gateway-port` 参数；加载插件；调用 `Exec()` |
| `plugins/client_viewer/client_viewer.cpp` | `ToplevelImpl<ViewerWindow>`：DoDraw 贴像素、DoMouse/DoKeyboard 打包发送、DoClose 退出 |
| `plugins/client_viewer/BUILD` | 构建为 `.so`（Linux）/ `.dll`（Windows）共享库 |
| `src/client/m6_client_test.cpp` | headless 集成测试：不启动 GKC GUI 循环，只验证协议收发 |

### 6.3 Client 插件加载流程

```
Client main()
  ↓
plugin_loader.load("./plugins/client_viewer/libclient_viewer.so")
  ↓
sa_ui_main() → SA_UIMain::Exec(real_gkc_ui_host, args)
  ↓
插件创建 ViewerWindow（ToplevelImpl）
  ↓
IoPool StartConnect → Gateway
  ↓
发送 SPAWN_APP("demo_app")
  ↓
收到 SESSION_ACK → 开始接收 PIXEL_DATA
  ↓
ViewerWindow.Show(true) + GuiHelper::Loop()
```

### 6.4 GKC 窗口绘制与输入流程（插件内）

```
IoPool 线程收到 PIXEL_DATA
    → MessageHandler.feed() → PIXEL_DATA 处理器
        → PixelRenderer.apply_dirty_rect(rcPaint, pixels) [mutex 保护]
        → PostWork() 投递主线程

主线程 PostWork 回调
    → 标记窗口脏矩形 → GKC 触发 DoDraw

DoDraw(pDraw)
    → PixelRenderer.blit(pDraw->pBuffer, pDraw->iWidth, pDraw->rcPaint)

DoMouse(pMouse)
    → 序列化 MOUSE_EVENT Body（直接用 pMouse->uEvent, x, y）
    → IoPool BeginInput/EndInput 发往 Gateway

DoKeyboard(pKb)
    → 序列化 KEYBOARD_EVENT Body（直接用 pKb->btKey, btState, btDown）
    → IoPool BeginInput/EndInput 发往 Gateway
```

### 6.5 测试要求

| 测试类型 | 测试用例 | 期望结果 |
|----------|----------|----------|
| 自动化集成 | `m6_client_test` | PIXEL_DATA 正确写入 PixelRenderer，FRAME_ACK 正确发送 |
| 手动端到端 | Gateway + AppHost(demo_app) + Client(client_viewer) | 窗口显示蓝色背景，鼠标点击颜色切换，键盘 Space 切换颜色，稳定运行 60 秒无 crash |

**通过标准**：自动化测试 PASSED + 手动 60 秒稳定运行。

---

## 依赖关系图

```
M1 ✅ (协议层)
    │
    ▼
M2 (IoPool + MessageHandler 基础设施)
    │
    ▼
M3a (Gateway 路由) ──► M3b (进程管理 + 心跳)
                              │
                              ▼
                         M4 (AppHost 插件 + ui_host_impl + rcPaint 传输)
                              │
                              ▼
                         M5 (完整输入事件 + PostWork 多线程)
                              │
                              ▼
                         M6 (GKC Client 插件加载器)
```

并行开发机会：
- **M6 的 client_viewer 插件**（GKC 窗口结构）可在 M2 完成后并行开发，M6 时与网络层合并
- **M6 的 plugin_loader**（Client 侧）可在 M4 完成后并行开发（参考 AppHost 的 plugin_loader）
- M3a 和 M3b 顺序依赖，不可并行

---

## 开发环境约定

| 组件 | 开发/测试环境 | 说明 |
|------|------------|------|
| 协议层 / MessageParser / MessageHandler（M1-M2） | Linux 云服务器 | 纯逻辑，无平台依赖 |
| AppHost / Gateway（M2–M5） | Linux 云服务器 | 依赖 GKC（Linux epoll / Wayland 后端） |
| TestClient headless（M2–M5） | Linux 云服务器 | 无 GUI，可在无显示的 CI 环境运行 |
| GKC Client + 插件（M6） | 本地有显示的机器（Linux/Windows） | GKC 支持 Linux（Wayland）和 Windows（Win32/IOCP）；不支持 macOS |

**关键约定**：M1–M5 的所有测试均为 headless，可在 Linux 云服务器 CI 环境自动执行。M6 的 GKC GUI 窗口测试需要本地机器手动验证。

---

## 里程碑完成检查清单

### M1 完成标准 ✅
- [x] `bazel test //src/protocol:message_parser_test` — 15 个测试用例全部 PASSED
- [x] `bazel test //src/protocol:protocol_test` — 往返序列化测试通过
- [x] MessageParser 为 Pull 模型，POISONED 终态

### M2 完成标准
- [ ] `bazel test //src/apphost:apphost_test` — 3 个测试用例全部 PASSED
- [ ] `bazel test //src/common:message_handler_test` — MessageHandler 单元测试 PASSED
- [ ] MessageHandler 作为独立类存在于 `src/common/`，Gateway / AppHost / Client 均可复用
- [ ] `ConnContext` 继承 `node_base`，内嵌 `_IoFunc`，由 `free_list<ConnContext>` + 自旋锁管理，无 `new`/`delete`
- [ ] `alloc_conn()` 顺序严格为：`FetchFreeNode → call_constructor → PickFreeNode`
- [ ] 关闭时遍历 `active_head_` 主动清理残留 ConnContext
- [ ] 无 DirtyRectDetector（逐帧比对），不在任何 Milestone 引入

### M3a 完成标准
- [ ] `bazel test //src/gateway:gateway_test` — 3 个测试用例全部 PASSED
- [ ] 两个并发 Client 数据互不干扰

### M3b 完成标准
- [ ] `bazel test //src/gateway:gateway_lifecycle_test` — 4 个测试用例全部 PASSED
- [ ] `ps aux | grep apphost` 无残留进程

### M4 完成标准
- [ ] `bazel test //src/apphost:m4_plugin_test` — PASSED
- [ ] `bazel test //src/apphost:pixel_capture_test` — PASSED
- [ ] `dirty_rect_detector.h/.cpp` 已从代码库删除
- [ ] ui_host_impl 使用 pDraw->rcPaint，不保存上一帧

### M5 完成标准
- [ ] `bazel test //src/apphost:m5_input_test` — 4 个测试用例全部 PASSED
- [ ] MOUSE_EVENT / KEYBOARD_EVENT 经 PostWork 安全注入插件，无数据竞争

### M6 完成标准
- [ ] `bazel test //src/client:m6_client_test` — PASSED
- [ ] 手动测试：GKC 窗口稳定运行 60 秒，鼠标/键盘响应正常，无 crash
- [ ] Client 是插件加载器，client_viewer.so/.dll 独立可替换
- [ ] 键码直接使用 GKC `KB_*`，未引入额外映射层
