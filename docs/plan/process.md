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
    APPHOST_READY = 0x04,  // AppHost → Gateway：绑定 AppHost 连接到 SessionID

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

**目标**：引入 Gateway，专注验证路由正确性。Gateway 使用 `MessageParser`
处理 TCP 粘包/半包，只读取协议头中的 `CmdType` 和 `SessionID` 做控制包处理
或路由查表；除 `SPAWN_APP` / `HEARTBEAT` / `CLOSE_SESSION` 等少量控制包外，
业务包 Body 不在 Gateway 解析，按 SessionID 在 Client 和 AppHost 之间原样透传。

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
| `src/gateway/gateway.h/.cpp` | IoPool 监听两个端口（`public_port` 默认 19000，供 Client 连接；`apphost_internal_port` 默认 19001，供手动启动的 AppHost/TestAppHost 连接）；`free_list<ClientSession>` + `free_list<AppHostSession>`；每连接持有 `MessageParser`，按 SessionID 查 `SessionTable` 后转发原始协议包 |
| `src/gateway/main.cpp` | Gateway 入口 |
| `src/gateway/gateway_test.cpp` | 集成测试：2 个 TestClient + 2 个手动 TestAppHost，并发路由验证 |

### 3a.3 Gateway 的 MessageParser 路由模式

Gateway 有两类连接（Client 侧、AppHost 侧），但它们都只需要 `MessageParser`
把 TCP 字节流切成完整协议包。Gateway 不使用 `MessageHandler` 做 CmdType 回调分发，
因为它不是业务端点，不应该解析 `PIXEL_DATA` / `INPUT_EVENT` 的 Body。

Client 侧连接收到完整包后的处理：

```
SPAWN_APP      → M3a 中分配 SessionID，并等待/绑定手动连接的 TestAppHost
HEARTBEAT      → 回复 HEARTBEAT
INPUT_EVENT    → 查 SessionTable，转发原始包给对应 apphost_conn
CLOSE_SESSION  → 向对端发 CLOSE_SESSION，清理 Session
其他 CmdType   → 如果 SessionID 已绑定，则按路由表透传；否则丢弃或返回 ERROR_RESP
```

AppHost 侧连接收到完整包后的处理：

```
APPHOST_READY  → M3a 中用于完成手动 TestAppHost 与 SessionID 的绑定
PIXEL_DATA     → 查 SessionTable，转发原始包给对应 client_conn
HEARTBEAT      → 回复 HEARTBEAT
CLOSE_SESSION  → 向对端发 CLOSE_SESSION，清理 Session
其他 CmdType   → 如果 SessionID 已绑定，则按路由表透传；否则丢弃或返回 ERROR_RESP
```

推荐实现形态：

```cpp
void on_client_received(ClientSession* s, const uint8_t* data, size_t len) {
    s->parser_.feed(data, len);
    Packet pkt;
    while (s->parser_.next_packet(pkt) == ParseResult::OK) {
        switch (pkt.header.cmd_type) {
        case CmdType::SPAWN_APP:
            handle_spawn_app(s, pkt);
            break;
        case CmdType::HEARTBEAT:
            send_heartbeat(s->id_, pkt.header.session_id);
            break;
        case CmdType::CLOSE_SESSION:
            close_session(pkt.header.session_id);
            break;
        default:
            forward_to_apphost(pkt.header.session_id, pkt);
            break;
        }
    }
}
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

**目标**：在 M3a 纯路由基础上，把 Gateway 从“手动连接 TestAppHost”
推进到“收到 `SPAWN_APP` 后自动拉起真实 AppHost 进程，并管理完整生命周期”。

M3b 的核心不是新增业务渲染能力，而是补齐 Gateway 作为中枢必须承担的
**进程管理、启动握手、健康监测、超时关闭、僵尸进程回收**。

M3b 完成后，Client 侧仍可继续用测试 socket 模拟；AppHost 侧可以先使用一个
测试 AppHost 可执行程序或当前 `apphost_bin` 的最小连接模式。真实插件加载和
`ui_host_impl` 仍放在 M4。

---

### 3b.1 fork/exec 的语义

Gateway 在 Linux 上通过 `fork()` + `exec()` 启动 AppHost：

```
Gateway 进程
  └─ fork()
       ├─ 父进程：继续作为 Gateway 运行，记录 child pid
       └─ 子进程：调用 exec()，把自己替换成 apphost_bin
```

`fork()` 只创建子进程；`exec()` 才真正加载 AppHost 可执行文件。
因此 Gateway 需要在父进程保存：

```
session_id -> apphost_pid
apphost_pid -> session_id
```

并在 AppHost 退出后通过 `waitpid()` 回收，避免僵尸进程。

---

### 3b.2 Gateway 启动 AppHost 的完整流程

M3a 中 `SPAWN_APP` 只分配 SessionID 并等待手动 TestAppHost 连接。
M3b 中流程必须改为：

```
1. Client 连接 Gateway public_port

2. Client -> Gateway:
     SPAWN_APP(session_id=0, body=[appNameLength:4 BE][appName:utf8])

3. Gateway:
     - 解析并校验 appName
     - 通过 allowlist / 配置表把 appName 映射到 plugin_path
     - 分配 session_id
     - 创建 SessionRuntime，状态设为 SPAWNING
     - 调用 ProcessManager::spawn_apphost(...)
     - 记录 apphost_pid
     - 暂时不要向 Client 发送 SESSION_ACK

4. AppHost 子进程启动:
     apphost_bin --session-id=<id>
                 --gateway-host=127.0.0.1
                 --gateway-port=<apphost_internal_port>
                 --plugin=<plugin_path>
                 ...

5. AppHost -> Gateway internal port:
     APPHOST_READY(header.session_id=<id>)

6. Gateway 收到 APPHOST_READY:
     - 找到 SessionRuntime
     - 绑定 AppHostSession / apphost_conn
     - 状态从 SPAWNING -> ACTIVE
     - 向 Client 发送 SESSION_ACK(header.session_id=<id>)

7. 后续:
     - Client -> Gateway -> AppHost: INPUT_EVENT
     - AppHost -> Gateway -> Client: PIXEL_DATA
     - 双方 HEARTBEAT 更新 last_seen
```

**关键约束**：`SESSION_ACK` 应该在 `APPHOST_READY` 之后发送。
否则 Client 会认为 session 已经可用，但 AppHost 可能还没有启动成功或尚未连接。

---

### 3b.3 SessionRuntime 状态机

M3a 的 `SessionTable` 只记录路由关系。M3b 不应再并行维护一份独立的
`SessionRuntime` map；否则路由状态和生命周期状态容易不一致。

M3b 的推荐做法是：**扩展现有 `SessionTable`，让它成为唯一权威的
Session registry**。也就是说，`SessionTable` 内部从保存
`Route{client_conn, apphost_conn}` 升级为保存 `SessionRuntime`，路由查询
只是从 `SessionRuntime` 派生出 `Route`。

推荐定义：

```cpp
enum class SessionState {
    SPAWNING,  // 已收到 SPAWN_APP，已 fork/exec，等待 APPHOST_READY
    ACTIVE,    // Client 与 AppHost 均已绑定，可正常路由
    CLOSING,   // 正在关闭 socket / 终止 AppHost / 清理映射
    DEAD,      // 已完成清理，不再参与路由
};

struct SessionRuntime {
    uint32_t session_id = 0;
    SessionState state = SessionState::SPAWNING;

    uintptr client_conn = 0;
    uintptr apphost_conn = 0;
    pid_t apphost_pid = -1;

    std::string app_name;
    std::string plugin_path;

    TimePoint created_at;
    TimePoint spawn_deadline;
    TimePoint last_client_seen;
    TimePoint last_apphost_seen;
};
```

推荐结构：

```cpp
class SessionTable {
public:
    uint32_t create_spawning_session(uintptr client_conn, std::string app_name);
    SessionRuntime* find_runtime(uint32_t session_id);
    Route find_route(uint32_t session_id) const;
    bool bind_apphost(uint32_t session_id, uintptr apphost_conn, pid_t pid);
    void remove_session(uint32_t session_id);

private:
    std::unordered_map<uint32_t, SessionRuntime> sessions_;
    std::unordered_map<uintptr, uint32_t> by_client_;
    std::unordered_map<uintptr, uint32_t> by_apphost_;
    std::unordered_map<pid_t, uint32_t> by_pid_;
    mutable std::mutex mutex_;
};
```

约束：

- `SessionTable` / `SessionRuntime` 是同一份权威状态，不维护第二套 runtime map。
- Gateway 路由转发、heartbeat timeout、process reap 都通过这份 registry 查询。
- `SessionRuntime` 的指针或引用不能在释放 mutex 后长期保存；需要复制必要字段，
  或在受控锁范围内完成状态更新。
- `SessionTable::mutex_` 是所有 `SessionRuntime` 字段的唯一保护锁，包括
  `state`、`last_client_seen`、`last_apphost_seen`、`apphost_pid` 和连接 handle。
- `last_seen` 时间戳不需要单独使用 `std::atomic`；统一通过 `SessionTable`
  方法加锁更新，避免时间戳和状态字段产生不一致。
- Monitor thread 不能裸遍历 `sessions_` 后长期持有 `SessionRuntime*`。
  推荐做法是加锁生成只读 snapshot，释放锁后做超时判断；需要关闭 session
  时再通过 `SessionTable` / Gateway 的方法按 `session_id` 二次加锁更新。

推荐线程安全接口：

```cpp
struct SessionSnapshot {
    uint32_t session_id = 0;
    SessionState state = SessionState::DEAD;
    uintptr client_conn = 0;
    uintptr apphost_conn = 0;
    pid_t apphost_pid = -1;
    TimePoint spawn_deadline;
    TimePoint last_client_seen;
    TimePoint last_apphost_seen;
};

class SessionTable {
public:
    void mark_client_seen(uint32_t session_id, TimePoint now);
    void mark_apphost_seen(uint32_t session_id, TimePoint now);
    std::vector<SessionSnapshot> snapshot_sessions() const;
    bool transition_to_closing(uint32_t session_id);
};
```

Monitor 线程流程：

```
1. snapshots = SessionTable::snapshot_sessions()
2. 不持锁遍历 snapshots，判断 timeout / exited pid
3. 对需要关闭的 session 调用 close_session(session_id)
4. close_session 内部再次加锁确认状态，避免重复清理
```

状态迁移：

```
SPAWNING
  ├─ 收到 APPHOST_READY        -> ACTIVE
  ├─ fork/exec 失败            -> CLOSING -> DEAD
  ├─ spawn_deadline 超时       -> CLOSING -> DEAD
  └─ Client 提前断开           -> CLOSING -> DEAD

ACTIVE
  ├─ Client CLOSE_SESSION      -> CLOSING -> DEAD
  ├─ AppHost CLOSE_SESSION     -> CLOSING -> DEAD
  ├─ Client heartbeat 超时     -> CLOSING -> DEAD
  ├─ AppHost heartbeat 超时    -> CLOSING -> DEAD
  ├─ AppHost 进程退出/crash    -> CLOSING -> DEAD
  └─ Gateway shutdown          -> CLOSING -> DEAD
```

---

### 3b.4 ProcessManager 设计

`ProcessManager` 只负责 OS 进程生命周期，不负责协议路由。

建议接口：

```cpp
struct AppHostLaunchConfig {
    uint32_t session_id = 0;
    std::string apphost_bin;
    std::string gateway_host;
    uint16_t gateway_port = 0;
    std::string plugin_path;
    int width = 800;
    int height = 600;
};

struct AppHostProcess {
    pid_t pid = -1;
};

struct ExitedProcess {
    pid_t pid = -1;
    int status = 0;       // Raw waitpid status for logging/debugging.

    bool exited = false;  // WIFEXITED(status)
    int exit_code = -1;   // WEXITSTATUS(status), valid when exited == true.

    bool signaled = false; // WIFSIGNALED(status)
    int signal = 0;        // WTERMSIG(status), valid when signaled == true.
};

class ProcessManager {
public:
    // fork/exec apphost_bin. Returns false if fork fails.
    bool spawn_apphost(const AppHostLaunchConfig& cfg, AppHostProcess* out);

    // Ask the process to exit. Implementation may send SIGTERM first.
    bool terminate(pid_t pid);

    // Force kill after graceful shutdown timeout.
    bool kill_force(pid_t pid);

    // Non-blocking reap. Returns any exited child pids and exit statuses.
    std::vector<ExitedProcess> reap_exited();
};
```

字段含义必须严格区分：

| 字段 | 含义 | 由谁使用 | 示例 |
|------|------|----------|------|
| `session_id` | Gateway 分配的业务会话 ID；AppHost 启动后用它发送 `APPHOST_READY` | Gateway 生成；AppHost 读取 | `42` |
| `apphost_bin` | AppHost 宿主程序的可执行文件路径；这是 `exec()` 要启动的二进制，不是插件 | ProcessManager / OS `exec` | `bazel-bin/src/apphost/apphost_bin` |
| `gateway_host` | AppHost 启动后要反连的 Gateway 内部监听地址 | AppHost 读取并用于 TCP connect | `127.0.0.1` |
| `gateway_port` | AppHost 启动后要反连的 Gateway `apphost_internal_port` | AppHost 读取并用于 TCP connect | `19001` |
| `plugin_path` | AppHost 进程内要加载的业务插件 `.so/.dll` 路径；由 `apphost_bin` 运行时 `dlopen`/`LoadLibrary` | AppHost 读取并加载 | `./plugins/demo_app/libdemo_app.so` |
| `width` / `height` | 远程应用初始画布/窗口尺寸，M4 `ui_host_impl` 使用 | AppHost / 插件宿主逻辑 | `800` / `600` |

三者关系：

```
Gateway
  └─ ProcessManager exec(apphost_bin)
        └─ apphost_bin 进程启动
              ├─ connect(gateway_host, gateway_port)
              ├─ send APPHOST_READY(session_id)
              └─ dlopen(plugin_path)
```

换句话说：

- `apphost_bin` 是“谁来运行”的宿主可执行文件。
- `gateway_host` / `gateway_port` 是“AppHost 启动后连回哪里”。
- `plugin_path` 是“AppHost 进程内部加载哪个业务应用库”。

`apphost_bin` 和 `plugin_path` 不能混用。Gateway `exec()` 的目标永远是
`apphost_bin`；业务 `.so/.dll` 永远由已经运行起来的 AppHost 进程加载。

推荐配置模板：

```cpp
struct GatewayConfig {
    uint16_t public_port = 19000;
    std::string apphost_bind_host = "127.0.0.1";
    uint16_t apphost_internal_port = 19001;

    std::string apphost_bin = "bazel-bin/src/apphost/apphost_bin";

    struct AppEntry {
        std::string plugin_path;
        int width = 800;
        int height = 600;
    };
    std::unordered_map<std::string, AppEntry> app_allowlist;
};
```

配置来源：

```
public_port / apphost_internal_port / apphost_bin / allowlist
  均来自 Gateway 启动配置或 Gateway 命令行参数。

Client 的 SPAWN_APP Body 只能携带 appName。
Client 不能指定 apphost_bin。
Client 不能指定 plugin_path。
```

Gateway CLI 建议：

```bash
gateway \
  --public-port=19000 \
  --apphost-internal-port=19001 \
  --apphost-bind-host=127.0.0.1 \
  --apphost-bin=bazel-bin/src/apphost/apphost_bin
```

M3b 可以先用 hardcoded allowlist；后续再扩展为配置文件。

示例配置：

```cpp
GatewayConfig cfg;
cfg.apphost_bin = "bazel-bin/src/apphost/apphost_bin";
cfg.apphost_bind_host = "127.0.0.1";
cfg.apphost_internal_port = 19001;
cfg.app_allowlist = {
    {"demo_app", {"./plugins/demo_app/libdemo_app.so", 800, 600}},
    {"paint_app", {"./plugins/paint_app/libpaint_app.so", 1024, 768}},
};
```

收到 `SPAWN_APP("demo_app")` 后，Gateway 生成：

```cpp
AppHostLaunchConfig launch;
launch.session_id = allocated_session_id;
launch.apphost_bin = cfg.apphost_bin;
launch.gateway_host = cfg.apphost_bind_host;
launch.gateway_port = cfg.apphost_internal_port;
launch.plugin_path = cfg.app_allowlist["demo_app"].plugin_path;
launch.width = cfg.app_allowlist["demo_app"].width;
launch.height = cfg.app_allowlist["demo_app"].height;
```

对应 `exec` 参数：

```bash
bazel-bin/src/apphost/apphost_bin \
  --session-id=42 \
  --gateway-host=127.0.0.1 \
  --gateway-port=19001 \
  --plugin=./plugins/demo_app/libdemo_app.so \
  --width=800 \
  --height=600
```

实现要求：

- 子进程中调用 `execv()` 或 `execvp()` 后不得继续执行 Gateway 逻辑。
- 子进程 `exec` 失败时应 `_exit(127)`，避免复制出来的 Gateway 子进程继续跑。
- 父进程必须记录 `pid -> session_id`。
- Gateway shutdown 时必须对所有仍存活 AppHost 先 `SIGTERM`，超时后再 `SIGKILL`。
- 必须周期性 `waitpid(pid, &status, WNOHANG)`，防止僵尸进程。

---

### 3b.5 AppHost 启动参数规范

Gateway 通过命令行参数把 session 和连接信息传给 AppHost。

```bash
./apphost_bin \
  --session-id=<SessionID 十进制或十六进制> \
  --gateway-host=127.0.0.1 \
  --gateway-port=<apphost_internal_port> \
  --plugin=./plugins/demo_app/libdemo_app.so \
  --width=800 \
  --height=600
```

M3b 对 AppHost 的最低要求：

```
1. 解析 --session-id / --gateway-host / --gateway-port
2. 连接 Gateway 的 apphost_internal_port
3. 发送 APPHOST_READY(header.session_id=<SessionID>)
4. 能收 HEARTBEAT / CLOSE_SESSION
5. 可以在测试中主动退出或被 kill
```

如果 AppHost 启动后连接 Gateway internal port 失败：

```
1. AppHost 可以进行少量短间隔重试（建议 5 次，每次间隔 200ms）
2. 仍失败则以非 0 exit code 退出
3. Gateway 不依赖具体重试次数；spawn_timeout 内未收到 APPHOST_READY 即判定启动失败
```

`spawn_timeout` 是 Gateway 侧的权威启动超时机制。

真实插件加载、`DoDraw`、`PIXEL_DATA` 生产仍属于 M4。

---

### 3b.6 Heartbeat 与 timeout 策略

M3b 需要检测两类对象：

| 对象 | 为什么要检测 |
|------|--------------|
| Client | 用户关闭客户端、网络断开、Client 卡死不再发 HEARTBEAT |
| AppHost | 插件 crash、进程退出、死循环、启动后不发 APPHOST_READY、连接断开 |

Heartbeat 不用于判断“任务是否完成”。AppHost 是长生命周期远程应用，不是一次性任务。
Heartbeat 只用于判断连接/进程是否还活着。

推荐时间参数：

| 参数 | 建议值 | 说明 |
|------|--------|------|
| `spawn_timeout` | 5s | fork/exec 后等待 APPHOST_READY 的最长时间 |
| `heartbeat_interval` | 30s | Client/AppHost 正常发送 HEARTBEAT 的周期 |
| `client_timeout` | 90s | 超过该时间未收到 Client 任意包则关闭 session |
| `apphost_timeout` | 90s | 超过该时间未收到 AppHost 任意包则关闭 session |
| `shutdown_grace` | 3s | SIGTERM 后等待 AppHost 自行退出的时间 |

`last_client_seen` 和 `last_apphost_seen` 应在收到任意合法协议包时更新，
不必只在 HEARTBEAT 时更新。这样活跃的数据流不会被误判为超时。

Monitor 推荐实现：

```cpp
while (running_) {
    sleep(1s);
    process_manager.reap_exited();
    scan_sessions_for_timeout(now);
}
```

也可以后续替换为 GKC WorkPool/Timer，但 M3b 可以先用一个 Gateway 内部
`std::thread` 实现，确保测试可控。

---

### 3b.7 错误处理策略

#### SPAWN_APP Body 或应用名非法

M3b 明确 `SPAWN_APP` Body 格式为：

```
[appNameLength:4 bytes BE] [appName:UTF-8 bytes]
```

校验规则：

```
1. body 至少 4 字节
2. appNameLength == body.size() - 4
3. appNameLength > 0
4. appNameLength <= 255
5. appName 只允许 [A-Za-z0-9_.-]
6. appName 不允许包含 '/', '\', '..'
7. appName 必须存在于 Gateway allowlist / 配置表
8. 映射出的 apphost_bin 必须可执行，plugin_path 必须可读
```

失败处理：

```
Gateway -> Client: ERROR_RESP
不 fork AppHost
不发送 SESSION_ACK
```

M3b 可以先让 `ERROR_RESP` Body 为空；后续如需更细错误原因，再扩展错误码 Body。

安全边界：

```
Client 只允许提交逻辑 appName。
Client 提交的任何路径形式（例如 "/tmp/x.so"、"../x"、"plugins/x.so"）都必须拒绝。
Gateway 只能从自己的 allowlist / 配置表中解析 plugin_path。
Gateway 只能使用自己的 apphost_bin 配置启动宿主程序。
```

#### 同一 Client 重复发送 SPAWN_APP

M3b 明确采用 MVP 限制：

```
一个 Client TCP 连接最多绑定一个 SPAWNING 或 ACTIVE session。
```

如果同一 Client 连接在已有 `SPAWNING` / `ACTIVE` session 时再次发送 `SPAWN_APP`：

```
Gateway -> Client: ERROR_RESP
不创建新 session
不关闭旧 session
```

一个 Client 连接多 Session、多远程应用窗口、多路复用属于 M7 optional。

#### fork/exec 失败

```
Gateway -> Client: ERROR_RESP(session_id=0 or allocated session_id)
清理 SessionRuntime
不发送 SESSION_ACK
```

#### AppHost 启动超时

```
SPAWNING 超过 spawn_timeout 未收到 APPHOST_READY
Gateway -> Client: ERROR_RESP(session_id)
terminate/kill AppHost pid
清理 SessionRuntime
```

#### AppHost crash

```
waitpid 发现 AppHost 退出
如果 session 仍 ACTIVE/SPAWNING:
  Gateway -> Client: ERROR_RESP(session_id)
  DisableHandle apphost_conn
  DisableHandle client_conn
  清理 ClientSession 中的 session_id_ / apphost_conn_
  清理 AppHostSession 中的 session_id_ / client_conn_
  清理 SessionRuntime
```

M3b 采用 MVP 简化策略：AppHost 异常退出时关闭整个 Client TCP 连接。
虽然理论上可以保留 Client 连接并等待它再次 `SPAWN_APP`，但当前 M3b 明确限制
一个 Client 连接最多一个 active session，关闭整个连接更简单，也避免无 session
的半活跃 Client 状态。M7 多 session 扩展时再改为“只关闭出问题的 session”。

#### Client 断开或超时

```
关闭 Client socket
向 AppHost 发 CLOSE_SESSION（如果仍连接）
terminate/kill AppHost pid
清理 ClientSession 中的 session_id_ / apphost_conn_
清理 AppHostSession 中的 session_id_ / client_conn_
清理 SessionRuntime
```

#### Gateway shutdown

```
1. 停止接收新连接
2. 对所有 ACTIVE/SPAWNING session 标记 CLOSING
3. 向 Client/AppHost 发送 CLOSE_SESSION（尽力而为，不等待 ACK）
4. 给已提交的 CLOSE_SESSION 一个很短的 flush window，或只保证 BeginInput/EndInput 已尽力调用
5. DisableHandle 所有 socket
6. SIGTERM 所有 AppHost pid
7. shutdown_grace 后 SIGKILL 未退出 pid
8. waitpid 回收所有子进程
9. _IoPool_Disable()
```

必须先尝试发送 `CLOSE_SESSION`，再 `DisableHandle`。一旦 handle 被禁用，
后续 `BeginInput/EndInput` 可能无法再提交数据。

---

### 3b.8 SESSION_ACK 是否需要重试

不需要为 `SESSION_ACK` 设计应用层重试机制。

理由：

- TCP 已经保证已提交字节的可靠、有序传输。
- 如果连接断开，IoPool 会给 Gateway 关闭/错误事件。
- 如果 Client 进程卡死但连接未立即断开，heartbeat timeout 会处理。

M3b 只需要保证：

```
APPHOST_READY 之前不发送 SESSION_ACK
SESSION_ACK 之后依赖 HEARTBEAT/last_seen 维护连接存活
SESSION_ACK Body 为空，分配的 SessionID 写在 Header.session_id 中
```

---

### 3b.9 需要实现的文件

| 文件 | 内容 |
|------|------|
| `src/gateway/process_manager.h/.cpp` | `fork/exec` 拉起 AppHost；记录 PID；`waitpid(WNOHANG)` 回收；`SIGTERM/SIGKILL` 关闭 |
| `src/gateway/gateway.h/.cpp`（更新） | 集成 ProcessManager；维护 SessionRuntime 状态；`SPAWN_APP -> fork/exec -> APPHOST_READY -> SESSION_ACK`；monitor thread 扫描超时 |
| `src/gateway/gateway_lifecycle_test.cpp` | 生命周期专项测试 |
| `src/apphost/main.cpp`（更新或测试替身） | 支持 M3b 启动参数；连接 Gateway internal port；发送 APPHOST_READY |

### 3b.10 测试要求

| 测试用例 | 期望结果 |
|----------|----------|
| `AutoSpawnAppHost` | Client 发 SPAWN_APP；Gateway fork/exec AppHost；AppHost 发 APPHOST_READY；Client 收到 SESSION_ACK |
| `SessionAckWaitsForAppHostReady` | AppHost 未连接或未发 APPHOST_READY 前，Client 不应收到 SESSION_ACK |
| `AppHostReadyTimeout` | fork 后超过 spawn_timeout 未收到 APPHOST_READY；Client 收到 ERROR_RESP；AppHost 进程被回收 |
| `AppHostCrashRecovery` | kill AppHost；Gateway 通过 waitpid/连接关闭检测到；Client 收到 ERROR_RESP；无僵尸进程 |
| `ClientDisconnectCleansAppHost` | Client 断开；Gateway 关闭 session 并 terminate 对应 AppHost |
| `HeartbeatTimeout` | Client 或 AppHost 超过 timeout 未发任何合法包；Gateway 主动关闭并清理 session |
| `GatewayShutdown` | Gateway stop/shutdown；所有 Session 关闭；所有 AppHost 先 SIGTERM 后必要时 SIGKILL；waitpid 全部回收 |

**通过标准**：`bazel test //src/gateway:gateway_lifecycle_test` 全部 PASSED。

### 3b.11 如何运行和验证 M3b

#### 自动测试（推荐）

优先使用 Bazel 测试验证 M3b：

```bash
bazel test //src/gateway:gateway_lifecycle_test
```

查看完整测试输出：

```bash
bazel test //src/gateway:gateway_lifecycle_test --test_output=all
```

该测试会自动完成：

```
1. 启动 Gateway
2. TestClient 发送 SPAWN_APP("demo_app")
3. Gateway fork/exec apphost_bin
4. apphost_bin 连接 Gateway apphost_internal_port
5. apphost_bin 发送 APPHOST_READY
6. Gateway 向 TestClient 返回 SESSION_ACK
```

M3a + M3b 一起验证：

```bash
bazel test //src/gateway:gateway_test //src/gateway:gateway_lifecycle_test
```

#### 手动运行 Gateway

先构建二进制：

```bash
bazel build //src/gateway:gateway_bin //src/apphost:apphost_bin
```

启动 Gateway：

```bash
bazel-bin/src/gateway/gateway_bin \
  --public-port=19000 \
  --apphost-internal-port=19001 \
  --apphost-bin=bazel-bin/src/apphost/apphost_bin \
  --app=demo_app=/bin/true
```

参数含义：

| 参数 | 含义 |
|------|------|
| `--public-port` | Client/TestClient 连接 Gateway 的端口 |
| `--apphost-internal-port` | AppHost 反连 Gateway 的内部端口 |
| `--apphost-bin` | Gateway fork/exec 启动的 AppHost 宿主可执行文件 |
| `--app=name=plugin_path` | Gateway allowlist：Client 只能请求 `name`，Gateway 映射到 `plugin_path` |

当前 M3b 只验证自动启动和握手链路，`plugin_path` 可以先用 `/bin/true`
作为可读占位路径。真实插件加载、`ui_host_impl`、`DoDraw -> PIXEL_DATA`
属于 M4。

手动运行 Gateway 后，还需要一个 Client/TestClient 发送二进制协议包：

```
SPAWN_APP(session_id=0, body=[appNameLength:4 BE]["demo_app"])
```

目前仓库还没有独立的手动 Client CLI，所以完整手动发包建议后续新增
`tools/spawn_client` 或继续使用 `gateway_lifecycle_test`。M3b 手动运行主要用于
观察 Gateway 进程启动、AppHost 子进程拉起和日志/进程状态。

当前 M3b 手动链路能验证到：

```
Client/TestClient -> Gateway: SPAWN_APP
Gateway -> fork/exec apphost_bin
apphost_bin -> Gateway: APPHOST_READY
Gateway -> Client/TestClient: SESSION_ACK
```

不能验证真实像素链路：

```
plugin_loader / ui_host_impl / DoDraw -> PIXEL_DATA
```

该部分属于 M4。

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

## M7 — Optional Hardening / Multi-session 扩展

M7 不属于当前 MVP 主链路。只有在 M1-M6 端到端链路跑通后，再考虑以下增强。

### 7.1 AppHost 启动身份校验（launch_token）

M3b 中 `APPHOST_READY(header.session_id)` 只靠 `session_id` 绑定 AppHost 连接。
在本机测试和 MVP 中可以接受，但安全边界较弱：任何能连接
`apphost_internal_port` 且知道 `session_id` 的进程，都可能伪装成 AppHost。

M7 可增加一次性启动 token：

```
Gateway:
  - 分配 session_id
  - 生成随机 launch_token
  - fork/exec AppHost 时传入 --launch-token=<token>

AppHost:
  - APPHOST_READY Body 携带 launch_token

Gateway:
  - 校验 session_id + launch_token 同时匹配
  - 校验成功后才绑定 AppHostSession
```

建议 Body：

```
[tokenLength:4 bytes BE] [token:UTF-8 or raw random bytes]
```

### 7.2 一个 Client 连接多个 Session

M3b 明确限制一个 Client TCP 连接最多一个 `SPAWNING` / `ACTIVE` session。
M7 可以扩展为：

```
一个 Client TCP 连接
  ├── Session A -> AppHost A
  ├── Session B -> AppHost B
  └── Session C -> AppHost C
```

需要的重构：

```
ClientSession::session_id_   -> std::unordered_set<uint32_t> session_ids_
ClientSession::apphost_conn_ -> 不再保存单个 AppHost handle
SessionTable                 -> 支持一个 client_conn 对应多个 session
Client UI                    -> 根据 session_id 分发到不同窗口/标签页
```

这会影响 Client、Gateway、测试模型，不放入 M3b。

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
- [ ] MessageHandler 作为独立类存在于 `src/common/`，供 AppHost / Client 等业务端点复用；Gateway 在 M3a 中只使用 `MessageParser` 做定帧和路由
- [ ] `ConnContext` 继承 `node_base`，内嵌 `_IoFunc`，由 `free_list<ConnContext>` + 自旋锁管理，无 `new`/`delete`
- [ ] `alloc_conn()` 顺序严格为：`FetchFreeNode → call_constructor → PickFreeNode`
- [ ] 关闭时遍历 `active_head_` 主动清理残留 ConnContext
- [ ] 无 DirtyRectDetector（逐帧比对），不在任何 Milestone 引入

### M3a 完成标准
- [ ] `bazel test //src/gateway:gateway_test` — 3 个测试用例全部 PASSED
- [ ] 两个并发 Client 数据互不干扰

### M3b 完成标准
- [ ] `bazel test //src/gateway:gateway_lifecycle_test` — M3b 生命周期测试全部 PASSED
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
