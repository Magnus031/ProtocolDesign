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

**目标**：AppHost 通过 `plugin_loader` 加载业务 `.so` 插件，`ui_host_impl` 实现一套 **headless 的假 `IUiHost`**：不开真实窗口、不依赖 Wayland，自己在内存里维护窗口缓冲区、主动派发 `UI_MESSAGE_DRAW`。每次 `DoDraw` 之后立即用 `pDraw->rcPaint` 和 `pDraw->pBuffer` 打包 PIXEL_DATA 发往 Gateway。**不做逐帧像素比对**。

> **架构原则（不对称）**：AppHost 端用我们自己实现的 fake/headless `ui_host_impl`，不创建真实窗口；Client 端（M6）用 GKC 提供的真实 GUI `IUiHost`。两端业务插件代码对称（都基于 `g_ui_host` / `ToplevelImpl`），但 IUiHost 实现不对称。M4 只负责服务端那一半。

### 4.1 脏矩形传输的新设计

GKC 的 `DoDraw()` 回调已经携带了当次重绘的脏矩形：

```
插件 Show(true) 或 Damage(rect) → fake host 派发 UI_MESSAGE_DRAW
    → 插件 DoDraw(pDraw)：
        pDraw->rcPaint        = 本次脏矩形（fake host 设置）
        pDraw->pBuffer        = WindowRuntime 持有的整窗 color_quad 缓冲
        pDraw->iWidth/iHeight = 窗口尺寸（来自 SPAWN_APP/--width/--height）
    → 插件向 pBuffer 写像素，DoDraw 返回

ui_host_impl 捕获并打包（同步，主线程内完成）：
    auto body = pack_pixel_data_rect(
        pDraw->pBuffer, pDraw->iWidth,
        rcPaint.left, rcPaint.top, rcPaint.right, rcPaint.bottom,
        frame_seq);
        // pack_pixel_data_rect 一站完成：
        //   1. 按 [left,right) × [top,bottom) 从 color_quad* 缓冲裁剪
        //   2. 把 GKC color_quad（little-endian B,G,R,A）重排为协议
        //      要求的 [A,R,G,B] 大端字节流
        //   3. 拼出 PIXEL_DATA Body：
        //      [frameSeq(4)][rcLeft(4)][rcTop(4)][rcRight(4)][rcBottom(4)][dataLen(4)][pixelData...]
    send_to_gateway(body);   // 同步 send 到 IoPool 句柄
```

`rcPaint` 由 fake host 在派发 `UI_MESSAGE_DRAW` 前设置：`Show(true)` 触发的首帧 `rcPaint = {0, 0, w, h}`，`Damage(rect)` 触发的局部帧 `rcPaint = rect`。M4 不做 coalesce，每次 `Damage` 就是一次 DoDraw 加一次 PIXEL_DATA。

> **像素格式注意**：GKC `pBuffer` 是 `color_quad*`（`color_quad = uint32_t`）。`COLOR_QUAD_MAKE(r,g,b,a)` 的位布局是 `bit31..0 = a r g b`，因此在 little-endian Linux 上的内存字节顺序是 `B, G, R, A`。协议 wire 顺序是 `[A, R, G, B]` 大端，必须在 `pack_pixel_data_rect` 内逐像素重排。`capture_rect` **保持现有"纯字节裁剪"语义不变**（M2 的契约），仅供旧测试 / 字节缓冲场景使用，不参与 M4 主路径打包。

### 4.2 需要实现/修改的文件

| 文件 | 内容 |
|------|------|
| `src/apphost/ui_host_impl.h/.cpp` | headless 假 `IUiHost`：内部 `WindowRuntime { std::vector<color_quad> pixels; UiMessageHandler handler; void* ctx; ... }`；实现 `CreateToplevel/Show/Destroy/Damage/Loop/Quit/PostWork/AddTimer`；`Show(true)` 自动派发首帧整窗 `UI_MESSAGE_DRAW`，`Damage(rect)` 同步派发局部 `UI_MESSAGE_DRAW`；DoDraw 返回后调用 `capture_rect` + `pack_pixel_data_rect` 并同步 `send` PIXEL_DATA |
| `src/apphost/pixel_capture.h/.cpp` | 保留现有 `capture_rect` 字节裁剪语义；**新增** `pack_pixel_data_rect(const color_quad* pixels, int stride, int left, int top, int right, int bottom, uint32_t frame_seq)` → `std::vector<uint8_t>`，负责"GKC color_quad 内存（B,G,R,A）→ 协议 ARGB wire（[A,R,G,B] 大端）"转换并拼出 PIXEL_DATA Body |
| `src/apphost/plugin_loader.h/.cpp` | `dlopen(path, RTLD_NOW \| RTLD_LOCAL)` 加载 `.so`，`dlsym("_SA_UIMain")` 取 `extern "C"` 入口，构造 `LcInterface<IUiHost>`（指向 AppHost 的 fake host），调用 `_SA_UIMain(lcHost, args)` |
| `src/apphost/apphost.h/.cpp`（更新） | 集成 plugin_loader 与 ui_host_impl；接收 `--plugin/--width/--height` 启动参数；删除现有静态 16×16 红帧逻辑；INPUT_EVENT 处理留 stub（M5 完善）；CLOSE_SESSION 收到后直接调用 fake host 的 Quit（线程安全） |
| `plugins/demo_app/demo_app.cpp` | 示例插件：实现 `program_entry_point::GuiMain`，创建 `ToplevelImpl<DemoWindow>`，`Show(true)` 后等待事件；DoDraw 用 `COLOR_QUAD_BLUE` 填整窗背景 + 在固定子矩形画 `COLOR_QUAD_RED`；外部触发或定时器引发一次 `Damage(small_rect)` 用于测试 |
| `plugins/demo_app/BUILD` | `cc_binary(name="libdemo_app.so", linkshared=True, deps=[...])`，依赖 GKC 头 + `:gkc_gui_runtime`（提供 `g_ui_host` 存储和 `_SA_UIMain` shim） |
| `third_party/BUILD`（或 `third_party/GKC/BUILD`） | 新增 `cc_library` 目标 `:gkc_gui_runtime`（含 `public/include/base/GkcGui.cpp`）。**仅供插件 .so 链接，AppHost 二进制不依赖它。** |
| `src/apphost/m4_plugin_test.cpp` | 集成测试：fake Gateway 监听 → 启动 AppHost 子进程（`--gateway-host/--gateway-port/--session-id/--plugin/--width/--height`）→ 收到 APPHOST_READY 与两包 PIXEL_DATA → 关闭 |
| `src/apphost/apphost_lifecycle_test.cpp`（保留旧 apphost_test 网络行为） | 连接建立、HEARTBEAT echo、CLOSE_SESSION 退出、多 PIXEL_DATA 包顺序——从被替换的 `apphost_test.cpp` 中迁移这些断言 |

> `src/apphost/dirty_rect_detector.h/.cpp` 已在 M2 之前删除，不在 M4 范围内（参见 memory）。
> `src/apphost/pixel_buffer.h/.cpp` **删除**。M4 主路径的窗口像素由 `ui_host_impl::WindowRuntime` 直接持有 `std::vector<color_quad>`，避免"窗口内存格式（B,G,R,A）"和"协议 wire 格式（[A,R,G,B]）"两层语义混淆。
> 旧的 `src/apphost/apphost_test.cpp`（M2 静态红帧测试）整体替换为上面两个新测试。

**链接拓扑要求（必须验证）**：

```
demo_app.so
  ├─ deps: //third_party:gkc_gui_runtime   (GkcGui.cpp → 提供插件本地的 g_ui_host + _SA_UIMain)
  └─ deps: //third_party:GkcSys 或 GKC 纯头依赖

apphost_bin
  ├─ deps: ui_host_impl + plugin_loader    (自己实现一套 IUiHost 函数表)
  └─ **不要** deps gkc_gui_runtime
```

风险点：如果 AppHost 也链接了 `GkcGui.cpp`，AppHost 与插件 .so 各自会有一份独立的 `GKC::g_ui_host` 全局变量；AppHost 设置的 fake host 不会被插件看到，导致插件调用 `g_ui_host.GetFunc()` 拿到空表 → 崩溃。M4 实现时要写一个小的链接探针：在 demo 插件 `GuiMain` 入口处 `assert(GKC::g_ui_host.GetFunc() != nullptr)`，确认它拿到的就是 AppHost 注入的 fake host。

### 4.3 多线程模型（本 Milestone 明确）

M4 **不引入** GKC `WorkPool` / `PostWork` 线程模型。捕获、打包、发送都在主线程同步完成，仅依赖一个线程安全的 `Quit` 让 IoPool 线程能唤醒主线程退出。M5 再把输入事件的 `PostWork` 路径补齐。

```
主线程
  ├── 解析 --gateway-host/port/session-id/plugin/width/height
  ├── connect Gateway, send APPHOST_READY
  ├── _IoPool_Fetch + SetHandleFunc(gateway_conn, io_func)
  ├── plugin_loader.load(plugin_path)
  │     └── _SA_UIMain(lcHost, args)  // lcHost 指向 fake host
  │           └── program_entry_point::GuiMain(args)
  │                 ├── window.Create(...)
  │                 ├── window.Show(true)
  │                 │     └── fake host 派发 UI_MESSAGE_DRAW(rcPaint=full)
  │                 │           └── 插件 DoDraw 写 pBuffer
  │                 │                 └── ui_host_impl 同步 capture_rect
  │                 │                       + pack_pixel_data_rect
  │                 │                       + send(PIXEL_DATA)  ← 主线程内同步
  │                 └── GuiHelper::Loop()  ← 阻塞主线程，等 quit_flag
  └── _IoPool_Disable + cleanup

IoPool 线程（GKC 内部）
  └── IO_TYPE_RECEIVED → MessageParser.feed()
        ├── HEARTBEAT  → 同线程直接 echo send（与 M2/M3b 一致）
        └── CLOSE_SESSION → 直接调用 fake host 的 Quit()
              （Quit 线程安全：set quit_flag + wake Loop）

业务输入事件（INPUT_EVENT）
  M4 仅 stub：IoPool 线程收到后直接丢弃或日志记录。
  M5 引入 PostWork：IoPool 线程 → fake host PostWork → 主线程 → 插件 DoMouse/DoKeyboard。
```

**Loop / Quit 语义（M4 假宿主必备）**

```cpp
// fake host 内部
std::mutex                    mtx_;
std::condition_variable       cv_;
bool                          quit_  = false;
std::deque<PendingWork>       queue_;   // M4 暂时只装"Damage 引发的同步 draw"占位；
                                        // PostWork 队列在 M5 启用

int  Loop(void*) {
    std::unique_lock<std::mutex> lk(mtx_);
    cv_.wait(lk, [&]{ return quit_ || !queue_.empty(); });
    // M4：唯一的退出条件是 quit_ = true
    return 0;
}
void Quit(void*) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        quit_ = true;
    }
    cv_.notify_all();
}
```

`Quit` 必须可从任意线程调用（包括 IoPool 线程）。`Loop` 在主线程阻塞，被 `Quit` 唤醒后返回，`_SA_UIMain` 随之返回，主线程进入 `_IoPool_Disable + cleanup` 收尾。

`PostWork` / `AddTimer` 在 M4 是简化实现而非空 stub：

- `PostWork(work, data)`：把 `(work, data)` 入队后 `cv_.notify_all()`；`Loop` 在 wait 唤醒后 drain 队列、依次同步执行。这样即使 demo 插件或 GKC wrapper 内部偷用了 `PostWork`，主线程仍能消费、不会卡死。
- `AddTimer`：M4 直接返回 0（不创建定时器）。demo 插件不依赖定时器；如果未来某个插件依赖，再补。
- M4 仍**不让** IoPool 线程通过 `PostWork` 注入 INPUT_EVENT —— 那是 M5 的工作；INPUT_EVENT 在 M4 的 IoPool 处理器里直接丢弃。

也就是说 M4 的 Loop 真实条件是"`quit_ == true` 或 `queue_` 非空（消费完继续 wait）"，与上面伪代码里给的最小骨架等价。

### 4.4 测试要求

测试采用 **反向连接 + fake Gateway** 模型：测试进程内一个轻量 fake Gateway `listen()` 在某个端口，启动 AppHost 子进程并通过 `--gateway-host/--gateway-port/--session-id/--plugin/--width/--height` 让其连入。fake Gateway 只需具备：accept、收 `APPHOST_READY`、收 PIXEL_DATA、按需回 HEARTBEAT/CLOSE_SESSION。

| 测试 | 用例 | 期望结果 |
|------|------|----------|
| `pixel_capture_test` | `CaptureRectByteOrder` | `capture_rect` 在给定 stride 下裁出正确的字节序列（M2 字节裁剪语义） |
| `pixel_capture_test` | `PackPixelDataConvertsBgraToArgb` | `pack_pixel_data_rect` 将 `color_quad`（B,G,R,A in mem）转换为 wire `[A,R,G,B]`，并正确拼出 PIXEL_DATA Body 头部字段 |
| `m4_plugin_test` | `PluginLoadsAndSeesFakeHost` | AppHost 子进程加载 `libdemo_app.so`，插件入口处 `assert(g_ui_host.GetFunc() != nullptr)` 通过，未 crash |
| `m4_plugin_test` | `InitialFullFrame` | 连接 + Show(true) 后收到 1 包 PIXEL_DATA：rect == 整窗，dataLen == w·h·4，像素全为 demo 背景蓝 |
| `m4_plugin_test` | `DirtyRectIsSubset` | 触发一次 `Damage(small_rect)` 后收到 1 包 PIXEL_DATA：`rectLeft/Top/Right/Bottom == small_rect`，dataLen == sw·sh·4，像素全为 demo 小色块红 |
| `m4_plugin_test` | `MultiPixelPacketOrdering` | 连续两次 `Damage` 触发的两包 PIXEL_DATA 按顺序到达、`frameSeq` 递增 |
| `apphost_lifecycle_test` | `ConnectAndReady` | AppHost 子进程能连入 fake Gateway 并发出 APPHOST_READY |
| `apphost_lifecycle_test` | `HeartbeatEcho` | fake Gateway 发 HEARTBEAT，AppHost echo 返回 |
| `apphost_lifecycle_test` | `CloseSessionExits` | fake Gateway 发 CLOSE_SESSION，AppHost 子进程在 `shutdown_grace` 内自行退出（exit 0） |
| `apphost_lifecycle_test` | `MultiPacketSendQueue` | 多包顺序发送不丢/不交错（继承自旧 apphost_test 的覆盖） |

**通过标准**：`bazel test //src/apphost:m4_plugin_test //src/apphost:pixel_capture_test //src/apphost:apphost_lifecycle_test` 全部 PASSED。

---

## M5 — 完整输入事件闭环 + 多线程 PostWork 模型

**目标**：在 M4 的“插件能绘制并发送 PIXEL_DATA”基础上，补齐反向输入链路：`INPUT_EVENT` 从 Client/TestClient 经 Gateway 路由到 AppHost，AppHost 在网络线程中解析事件，但必须通过 `PostWork()` 切回插件主线程，再注入 `UI_MESSAGE_MOUSE` / `UI_MESSAGE_KEYBOARD`，最终触发插件的 `DoMouse()` / `DoKeyboard()`，插件状态变化后调用 `Damage()`，产生新的 `PIXEL_DATA`。

> 术语说明：M3/M4 计划里常写“IoPool 线程”。当前 M4 AppHost 实现实际是一个 blocking socket `reader_` 线程，而不是 GKC IoPool。M5 的线程约束不依赖具体 I/O 实现，准确表述应是：**network reader thread → PostWork queue → plugin main thread**。后续如果 AppHost 再切回 GKC IoPool，这个线程边界仍然成立。

### 5.0 M5 与 M4 的差异

M4 完成的是**插件主动绘制**：

```
plugin GuiMain()
  ├── win.Show(true)
  │     └── fake host dispatch UI_MESSAGE_DRAW
  │           └── plugin DoDraw()
  │                 └── AppHost packs PIXEL_DATA
  └── win.Damage(rect)
        └── same path
```

M5 要完成的是**外部输入驱动绘制**：

```
TestClient / future Client
  └── INPUT_EVENT(mouse / keyboard)
        ↓
Gateway
  └── route by SessionID, do not parse Body
        ↓
AppHost reader thread
  └── parse INPUT_EVENT Body
  └── PostWork(inject_mouse / inject_keyboard)
        ↓
AppHost plugin main thread
  └── ui_host_impl.dispatch_mouse/keyboard()
        ↓
demo_app plugin
  └── DoMouse / DoKeyboard changes state
  └── Damage(rect)
        ↓
DoDraw → PIXEL_DATA
```

因此 M5 的核心不是“再发一包 PIXEL_DATA”，而是证明**远端输入可以安全地跨线程进入插件 UI 逻辑并驱动重绘**。

### 5.1 输入事件协议 Body 格式

所有输入事件共用 `CmdType::INPUT_EVENT = 0x20`，Header 的 `session_id` 仍由 Gateway 用于路由。Gateway 不解析 Body；Body 首字节 `eventType` 只由 Client/AppHost 端解释。

**鼠标事件 Body（13 字节，eventType = 0x01~0x06）：**

```
┌──────────────┬──────────────────┬──────────────────┬──────────────────┐
│ eventType    │ x                │ y                │ timestamp        │
│ (1 byte)     │ (4 bytes BE32)   │ (4 bytes BE32)   │ (8 bytes BE64)   │
└──────────────┴──────────────────┴──────────────────┴──────────────────┘
eventType: MOVE=0x01, LEFT_DOWN=0x02, LEFT_UP=0x03,
           RIGHT_DOWN=0x04, RIGHT_UP=0x05, SCROLL=0x06
```

字段语义：

| 字段 | 说明 |
|------|------|
| `eventType` | 鼠标子类型 |
| `x`, `y` | Client 窗口坐标，M5 不做 DPI/缩放映射，直接传给 GKC `UiMessageMouse::x/y` |
| `timestamp` | Client 侧产生事件的单调时间戳，单位为微秒，`uint64_t` 大端序；M5 解析但 AppHost demo 不依赖 |

映射到 GKC `UiMessageMouse`：

| 协议事件 | GKC 字段建议 |
|----------|--------------|
| `MOVE` | `uEvent=MOUSE_EVENT_MOVE`, `btButton=MOUSE_BUTTON_NONE` |
| `LEFT_DOWN` | `uEvent=MOUSE_EVENT_DOWN`, `btButton=MOUSE_BUTTON_LEFT`, `btState` 包含 `MOUSE_STATE_LEFT` |
| `LEFT_UP` | `uEvent=MOUSE_EVENT_UP`, `btButton=MOUSE_BUTTON_LEFT`, `btState` 清除 left |
| `RIGHT_DOWN` | `uEvent=MOUSE_EVENT_DOWN`, `btButton=MOUSE_BUTTON_RIGHT`, `btState` 包含 `MOUSE_STATE_RIGHT` |
| `RIGHT_UP` | `uEvent=MOUSE_EVENT_UP`, `btButton=MOUSE_BUTTON_RIGHT`, `btState` 清除 right |
| `SCROLL` | `uEvent=MOUSE_EVENT_WHEEL`；当前协议没有 wheel delta，M5 可先置 `btValue=0` 或暂不覆盖测试 |

> GKC 当前结构见 `third_party/GKC/public/include/base/system/ui_types.h`：`ui_message_mouse { uint uEvent; byte btButton; byte btState; byte btDouble; byte btValue; int x, y; }`。M5 实现时必须按真实字段构造，不能只按伪代码猜字段名。

**键盘事件 Body（11 字节，eventType = 0x10~0x11）：**

```
┌──────────────┬──────────────────┬──────────────────┐
│ eventType    │ keyCode          │ timestamp        │
│ (1 byte)     │ (2 bytes BE16)   │ (8 bytes BE64)   │
└──────────────┴──────────────────┴──────────────────┘
eventType: KEY_DOWN=0x10, KEY_UP=0x11
keyCode: GKC UiMessageKeyboard::btKey 值，直接从 DoKeyboard(pKb->btKey) 填入，无需映射
```

字段语义：

| 字段 | 说明 |
|------|------|
| `eventType` | `KEY_DOWN` 或 `KEY_UP` |
| `keyCode` | GKC `UiMessageKeyboard::btKey` 值。`btKey` 是 `byte`，M5 必须校验 `keyCode <= 255`；超出范围直接丢弃，不截断 |
| `timestamp` | Client 侧事件时间戳，单位为微秒，`uint64_t` 大端序；M5 解析但 demo 不依赖 |

映射到 GKC `UiMessageKeyboard`：

| 协议事件 | GKC 字段建议 |
|----------|--------------|
| `KEY_DOWN` | `btDown=1`, `btKey=keyCode`, `btState=0`, `btValue=0`, `chFull=0` |
| `KEY_UP` | `btDown=0`, `btKey=keyCode`, `btState=0`, `btValue=0`, `chFull=0` |

> GKC 当前结构：`ui_message_keyboard { byte btDown; byte btKey; byte btState; byte btValue; char_f chFull; }`。`btKey` 承载 `KB_Left`、`KB_F1` 等 `KB_*` / 虚拟键码；Space 这类可打印字符走 `chFull`，而 M5 键盘协议不传 `chFull`，因此 M5 demo/测试不要使用 Space。测试重点改为 `KB_F1` 或 `KB_Left` 这类真实 `btKey` 值。

### 5.1.1 协议 helper 与错误处理

M5 应避免在测试和 AppHost 中重复手写位移解析。建议新增轻量 helper：

| 位置 | 内容 |
|------|------|
| `src/common/input_event.h` | `enum class InputEventType`，`parse_input_event_body()`，`pack_mouse_input_event()`，`pack_keyboard_input_event()`；作为协议 helper 与 `message_handler.h` 同级，不放入核心 `protocol.h/.cpp` |
| 测试 helper | 可以复用生产 pack helper，减少 Body 字节布局不一致 |

解析策略：

| 情况 | 处理 |
|------|------|
| Body 为空 | 丢弃，不 crash |
| `eventType` 未知 | 丢弃，不 crash |
| 鼠标事件长度不是 13 | 丢弃，不 crash |
| 键盘事件长度不是 11 | 丢弃，不 crash |
| `keyCode > 255` | 丢弃并保持无副作用，不做截断 |

M5 不要求 AppHost 向 Client 返回 `ERROR_RESP`。输入事件是数据面消息，坏包直接忽略即可，避免一个坏输入杀死 session。

### 5.2 AppHost 输入事件注入：PostWork 模式

reader/network 线程收到 `INPUT_EVENT` 后，**不能**直接操作插件（插件在主线程），必须用 `PostWork` 投递。原因：

- 插件窗口对象、业务状态、`WindowRuntime::pixels` 都按主线程 UI 模型使用。
- M4 的 `Show()` / `Damage()` / `DoDraw()` 均同步发生在插件主线程。
- 如果 reader 线程直接调用 `handler.Process(UI_MESSAGE_MOUSE/KEYBOARD)`，会和主线程 `DoDraw()` / `Damage()` 产生数据竞争。

M5 的安全路径：

```
reader/network 线程
  recv() / IO_TYPE_RECEIVED
    → MessageHandler.feed()
    INPUT_EVENT 处理器：
      1. 读取 Body[0]（eventType），判断是鼠标还是键盘
      2. 反序列化到一个 owned payload（不能引用 Packet::body 内存）
      3. ui_host_impl.post_mouse_input(...) / post_keyboard_input(...)
      4. reader 线程立即返回继续收包

主线程（PostWork 回调）
  on_host_loop()
    → drain post_queue
    → inject_mouse / inject_keyboard
      → ui_host_impl.dispatch_mouse/keyboard()
        → window.handler.Process(window.handler_ctx, UI_MESSAGE_MOUSE/KEYBOARD, &msg)
          → GKC::_WindowImpl<T>::Process()
            → plugin DemoWindow::DoMouse/DoKeyboard()
```

### 5.2.1 生命周期与内存所有权

PostWork 投递的数据不能悬空，因为 `PostWork(work, data)` 不是立即执行 `work`，而是把 `WorkProc + void* data` 放进主线程队列，等 `on_host_loop()` 后续 drain 时再执行。因此 reader/network 线程不能把栈对象地址、`Packet::body.data()` 指针或临时解析 buffer 直接传给 `PostWork`。

M5 统一采用 `UiHostImpl` 内部 owns-data queue：AppHost 解析出 `UiMessageMouse` / `UiMessageKeyboard` 后，调用 `UiHostImpl::post_mouse_input()` / `post_keyboard_input()`。这些 API 立即把事件值复制进 `UiHostImpl::State` 持有的队列，再投递一个固定的 `WorkProc` 到主线程。`apphost.cpp` 不直接 `new/delete` 投递 payload。

建议结构：

```cpp
struct InputEvent {
    enum class Kind { Mouse, Keyboard } kind;
    GKC::UiMessageMouse mouse{};
    GKC::UiMessageKeyboard keyboard{};
};

struct State {
    std::mutex mtx;
    std::queue<InputEvent> input_queue;
    bool input_work_pending = false;
    // existing window/runtime fields...
};
```

键盘事件示例：

```cpp
// reader/network thread, inside AppHost INPUT_EVENT handler
void AppHost::handle_keyboard_input(uint16_t key_code, bool down) {
    if (key_code > 255)
        return;

    GKC::UiMessageKeyboard kb{};
    kb.btDown = down ? 1 : 0;
    kb.btKey = static_cast<GKC::byte>(key_code);
    kb.btState = 0;
    kb.btValue = 0;
    kb.chFull = 0;

    ui_host_.post_keyboard_input(kb);
}
```

`UiHostImpl` 内部复制事件并投递 work：

```cpp
void UiHostImpl::post_keyboard_input(const GKC::UiMessageKeyboard& kb) {
    bool should_post = false;
    {
        std::lock_guard<std::mutex> lock(state_.mtx);
        InputEvent ev{};
        ev.kind = InputEvent::Kind::Keyboard;
        ev.keyboard = kb;  // value copy; caller's stack object may disappear.
        state_.input_queue.push(ev);
        if (!state_.input_work_pending) {
            state_.input_work_pending = true;
            should_post = true;
        }
    }

    if (should_post)
        post_work(GKC::WorkProc{&UiHostImpl::drain_input_work}, this);
}

static void UiHostImpl::drain_input_work(void* p) noexcept {
    static_cast<UiHostImpl*>(p)->drain_input_queue_on_main_thread();
}
```

主线程执行 `WorkProc` 时 drain 队列：

```cpp
void UiHostImpl::drain_input_queue_on_main_thread() {
    std::queue<InputEvent> local;
    {
        std::lock_guard<std::mutex> lock(state_.mtx);
        std::swap(local, state_.input_queue);
        state_.input_work_pending = false;
    }

    while (!local.empty()) {
        InputEvent ev = local.front();
        local.pop();
        if (ev.kind == InputEvent::Kind::Keyboard)
            dispatch_keyboard(ev.keyboard);
        else
            dispatch_mouse(ev.mouse);
    }
}
```

完整调用链：

```
reader/network thread
  -> recv INPUT_EVENT(KEY_DOWN, KB_F1)
  -> parse body into UiMessageKeyboard kb
  -> ui_host_.post_keyboard_input(kb)
      -> copy kb into UiHostImpl::State::input_queue
      -> PostWork(drain_input_work, this)
  -> reader thread returns

plugin main thread
  -> on_host_loop() drains PostWork queue
  -> drain_input_work(this)
  -> drain_input_queue_on_main_thread()
  -> dispatch_keyboard(kb)
  -> UiMessageHandler.Process(..., UI_MESSAGE_KEYBOARD, &kb)
  -> DemoWindow::DoKeyboard(&kb)
  -> DemoWindow updates state and calls Damage(full_rect)
  -> fake host dispatches UI_MESSAGE_DRAW
  -> DemoWindow::DoDraw(pDraw)
  -> AppHost captures pDraw->rcPaint and sends PIXEL_DATA
```

`input_work_pending` 用来避免每个输入事件都投递一个新的 work。若多个输入事件在主线程 drain 前连续到达，它们会共享一个 pending `WorkProc`，主线程一次 drain 完当前队列。M5 也可以先不做这个合并优化，但不能牺牲事件所有权：事件内容必须由 `UiHostImpl` 持有到主线程消费完。

### 5.2.2 UiHostImpl 事件注入 API

`ui_host_impl` 需要新增公开方法：

```cpp
void dispatch_mouse(const GKC::UiMessageMouse& msg);
void dispatch_keyboard(const GKC::UiMessageKeyboard& msg);
```

行为：

```
dispatch_mouse(msg):
  if no window / destroyed / no handler:
      return
  window.handler.Process(
      window.handler_ctx,
      UI_MESSAGE_MOUSE,
      reinterpret_cast<uintptr>(&msg))
```

`dispatch_keyboard` 同理，使用 `UI_MESSAGE_KEYBOARD`。

注意点：

- 这两个函数必须只在主线程/PostWork callback 中调用。
- 如果插件 `DoMouse()` / `DoKeyboard()` 内调用 `Damage()`，会同步触发 `DoDraw()` 和 `PIXEL_DATA`，因此 `on_host_loop()` 执行 work 时必须不持有 `State::mtx`。M4 当前已经在执行 work 前 `unlock()`，这是正确基础。
- 如果窗口尚未创建或 handler 尚未注册，输入事件应被静默丢弃，不 crash。

### 5.3 demo_app 行为设计

M5 的 demo 插件不再只是启动时发两帧；它需要把输入转成可观察的像素变化。建议保持窗口仍为 `32 x 16`，继续使用固定小矩形，便于测试。

当前 M4 demo 行为：

```
Show(true):
  full frame = blue background + red small rect

Damage(small):
  dirty rect = green small rect
```

M5 建议扩展为一个明确状态机：

```
state:
  background_color
  small_rect_color
  mouse_click_count
  keyboard_toggle

DoMouse(pMouse):
  if pMouse->uEvent == MOUSE_EVENT_DOWN
     && pMouse->btButton == MOUSE_BUTTON_LEFT
     && point inside small rect:
        small_rect_color = next color
        Damage(small_rect)

DoKeyboard(pKb):
  if pKb->btDown == 1 && pKb->btKey == KB_F1:
        background_color = toggled color
        Damage(full window)
```

测试应尽量选容易断言的颜色：

| 触发 | 建议变化 | 断言 |
|------|----------|------|
| 左键点击小矩形 | small rect 从 green 变 yellow 或 red/green toggle | 下一包 `PIXEL_DATA` rect == small rect，像素全为新颜色 |
| `KB_F1` key down | 背景色从 blue 变 cyan/white | 下一包 `PIXEL_DATA` rect == full window 或背景区域颜色变化 |

`NoInputNoPixelData` 测试需要先 drain 掉 M4 启动时的固定两帧，然后进入短时间 recv timeout；期间 demo_app 不应自行定时重绘。

### 5.4 需要实现/修改的文件

| 文件 | 内容 |
|------|------|
| `src/common/input_event.h`（必要时配套 `.cpp`） | 定义 `InputEventType`、鼠标/键盘 Body pack/parse helper；使用 `src/common/byte_order.h` 的 `BE16/BE32/BE64` 读写多字节字段 |
| `src/apphost/ui_host_impl.h/.cpp` | 新增 `dispatch_mouse()` / `dispatch_keyboard()`；必要时新增 `post_work()` wrapper，避免 `apphost.cpp` 直接碰底层 `IUiHost` table |
| `src/apphost/apphost.h/.cpp` | 把 M4 的 INPUT_EVENT stub 改成 parser + PostWork；处理无效 Body；保证 reader 线程不直接调用插件 handler |
| `plugins/demo_app/demo_app.cpp` | 新增 `DoMouse()` / `DoKeyboard()`；输入改变颜色状态并调用 `Damage()` |
| `src/apphost/m5_input_test.cpp` | AppHost 局部集成测试：fake Gateway + apphost 子进程；发送 `INPUT_EVENT`；验证后续 `PIXEL_DATA` |
| `src/apphost/BUILD` | 新增 `m5_input_test` 目标；按需要加入新的 protocol/common helper deps |
| `src/gateway/gateway_m5_input_test.cpp` | Gateway 联立集成测试：真实 Gateway + 自动拉起 AppHost + fake Client；验证 `INPUT_EVENT` 经 M3 Gateway 路由后仍能驱动 demo_app 出帧 |
| `src/gateway/BUILD` | 新增 `gateway_m5_input_test` 目标；复用 `gateway_m4_plugin_test` 的 fake Client / runfile / packet helper 形态 |
| `docs/design/M4.md` 或新增 `docs/design/M5.md` | M5 完成后更新实际输入链路和 demo 行为 |

### 5.5 AppHost 集成测试设计

M5 测试建议分两层。第一层是 AppHost 局部集成测试，仍采用 M4 的 fake Gateway 模型，不需要真实 Client GUI：

```
apphost m5_input_test
  ├── listen(fake Gateway)
  ├── fork/exec apphost_bin --plugin=libdemo_app.so
  ├── accept AppHost connection
  ├── recv APPHOST_READY
  ├── drain startup PIXEL_DATA frames from demo_app
  ├── send INPUT_EVENT
  ├── recv new PIXEL_DATA caused by input
  └── send CLOSE_SESSION, wait child exit 0
```

这层测试的价值是隔离 AppHost：如果失败，问题基本集中在 `INPUT_EVENT` parser、`PostWork`、`ui_host_impl.dispatch_*` 或 demo 插件逻辑，不会被 Gateway 进程管理和 Session 路由干扰。

第二层建议新增 Gateway 联立集成测试。它不需要真实 GKC Client，只需要 fake Client socket，但使用真实 Gateway 和真实 AppHost：

```
gateway_m5_input_test
  ├── start real Gateway(auto_spawn_apphost=true, allowlist demo_app)
  ├── fake Client connect Gateway public_port
  ├── fake Client send SPAWN_APP("demo_app")
  ├── Gateway fork/exec apphost_bin and pass demo_app plugin path
  ├── AppHost connect Gateway internal port and send APPHOST_READY
  ├── fake Client recv SESSION_ACK and drain startup PIXEL_DATA frames
  ├── fake Client send INPUT_EVENT(session_id, LEFT_DOWN / KEY_DOWN)
  ├── Gateway route original INPUT_EVENT Body to AppHost
  ├── AppHost injects input through PostWork and sends new PIXEL_DATA
  └── fake Client recv input-caused PIXEL_DATA, then send CLOSE_SESSION
```

从可行性看，这个测试是合理的：`src/gateway/gateway_m4_plugin_test.cpp` 已经验证了“真实 Gateway + 自动拉起真实 AppHost + fake Client 接收 demo_app 像素”的链路，M5 只是在该形态上增加 fake Client 发送 `INPUT_EVENT` 并断言后续新帧。它覆盖 fake Gateway 测不到的内容：`SPAWN_APP -> SESSION_ACK` 后的真实 SessionID、Gateway public/internal 两侧连接绑定、Client→Gateway→AppHost 的原始 Body 转发，以及 AppHost→Gateway→Client 的输入响应帧回传。

但它不应替代 `src/apphost/m5_input_test.cpp`。Gateway 联立测试更接近端到端，失败面更大，适合作为 M5 的第二道验收；AppHost 局部测试仍是定位输入注入问题的主测试。

推荐 helper：

| Helper | 作用 |
|--------|------|
| `make_mouse_event(session_id, eventType, x, y, timestamp)` | 构造 `CmdType::INPUT_EVENT` packet |
| `make_keyboard_event(session_id, eventType, keyCode, timestamp)` | 构造 keyboard packet |
| `recv_until_pixel_after_seq(min_seq)` | 跳过旧帧，只取输入之后的新帧 |
| `expect_rect_color(pkt, rect, color)` | 验证 dirty rect 坐标和像素颜色 |
| `drain_startup_frames()` | 消费 M4 demo 启动时 `Show(true)` / initial `Damage()` 两包 |
| `spawn_demo_session_via_gateway()` | Gateway 联立测试 helper：fake Client 发 `SPAWN_APP("demo_app")`，等待 `SESSION_ACK` 并返回真实 `session_id` |

### 5.6 测试要求

| 测试用例 | 期望结果 |
|----------|----------|
| `MouseClickChangesPixel` | fake Gateway 发 `INPUT_EVENT(LEFT_DOWN, x,y)` 到 AppHost；插件 `DoMouse()` 被主线程调用；收到新的 `PIXEL_DATA`，rect 和颜色符合 mouse 状态变化 |
| `KeyboardEventRouted` | fake Gateway 发 `INPUT_EVENT(KEY_DOWN, KB_F1)`；插件 `DoKeyboard()` 响应；收到新的 `PIXEL_DATA`，颜色状态变化 |
| `NoInputNoPixelData` | drain 启动帧后，不发送输入；短 timeout 内不应收到额外 `PIXEL_DATA` |
| `InvalidInputIgnored` | 发送空 Body、未知 eventType、错误长度；AppHost 不 crash、不退出、不产生输入响应帧 |
| `PostWorkThreadSafety` | 快速连续发 100 个 `LEFT_DOWN` 或 `MOVE` 事件；AppHost 无 crash/deadlock；最终仍能响应 `CLOSE_SESSION` 并 exit 0 |
| `GatewayM5InputRoute` | fake Client 经真实 Gateway 启动 demo_app，发送 `INPUT_EVENT(KEY_DOWN, KB_F1)` 或 `LEFT_DOWN`；Gateway 不解析 Body 但正确转发，fake Client 收到输入触发的新 `PIXEL_DATA` |

可选但有价值：

| 测试用例 | 期望结果 |
|----------|----------|
| `InputBeforeWindowReadyIgnored` | 在插件注册 handler 前到达的输入不会 crash（测试实现可能较难稳定，可作为单元测试覆盖） |
| `FrameSeqIncreasesAfterInput` | 输入产生的新 `PIXEL_DATA.frameSeq` 大于启动帧 |
| `MouseOutsideTargetNoFrame` | 点击 demo_app 非目标区域不触发 `Damage()`，不产生新帧 |

### 5.7 验收标准

**功能通过标准**：

```
bazel test //src/apphost:m5_input_test \
           //src/gateway:gateway_m5_input_test
```

全部 PASSED。

**工程标准**：

- AppHost reader/network 线程不直接调用 `window.handler.Process()`。
- 所有插件 `DoMouse()` / `DoKeyboard()` 调用都发生在 `on_host_loop()` drain PostWork 的主线程路径。
- `PostWork` payload 生命周期明确，没有栈指针跨线程、没有泄漏明显路径。
- 无效 `INPUT_EVENT` 不 crash、不关闭 session。
- 输入触发的 `Damage()` 能复用 M4 `dispatch_draw_and_send()` 路径，不新增第二套像素发送逻辑。
- Gateway 不需要改业务逻辑；它继续按 SessionID 原样转发 `INPUT_EVENT`。

### 5.8 M5 不做的事情

| 不做 | 原因 |
|------|------|
| 真实 Windows/Linux GUI Client | M6 范围；M5 用 fake Gateway 或 fake Client 构造 `INPUT_EVENT`；Gateway 联立测试使用真实 Gateway，但 Client 仍是测试 socket |
| 坐标缩放 / DPI 映射 | M6 Client viewer 才知道窗口显示尺寸；M5 坐标直传 |
| 鼠标滚轮 delta 完整语义 | 当前协议没有 delta 字段；M5 测试不依赖 |
| 多窗口输入路由 | M4/M5 只支持单 toplevel |
| 输入 ACK / FRAME_ACK | 当前协议无此要求，后续再评估 |
| Gateway 解析输入 Body | Gateway 是路由层，仍不解析业务 payload |

**通过标准**：M5 AppHost 局部测试、Gateway 联立测试全部 PASSED，并且 M4/M3 Gateway 回归测试仍通过：

```
bazel test //src/apphost:m5_input_test \
           //src/gateway:gateway_m5_input_test \
           //src/apphost:m4_plugin_test \
           //src/apphost:apphost_lifecycle_test \
           //src/apphost:pixel_capture_test \
           //src/gateway:gateway_test \
           //src/gateway:gateway_lifecycle_test \
           //src/gateway:gateway_m4_plugin_test
```

---

## M6 — Windows/GKC Client（真实 GUI 插件加载器 + 端到端联调）

**目标**：把 M5 的 fake Client 替换成真实 GKC GUI Client。用户在 Windows 上运行
`client.exe`，Client 加载自己的 `client_viewer.dll`，连接 Gateway 暴露的 public
port，发送 `SPAWN_APP("demo_app")`，接收 AppHost 返回的 `PIXEL_DATA` 并显示到真实
Win32/GKC 窗口；用户在窗口中的鼠标/键盘输入被打包为 `INPUT_EVENT` 发回 Gateway，
最终驱动 AppHost 插件状态变化并刷新窗口。

> **关键架构约定**：AppHost 端使用我们自己实现的 fake/headless `ui_host_impl`，因为
> AppHost 是 UI 虚拟化宿主，不需要真实屏幕，只需要内存 backing store 和 `rcPaint`
> 捕获。Client 端不再实现 fake host，而是直接使用 GKC 自带的真实 GUI host。GKC 的
> `util/gui/uihost` 已经同时有 Linux Wayland 与 Windows Win32 适配：`uihost/src/Main.cpp`
> 初始化 `g_win_ui_host`，再把 `LcInterface<IUiHost>(&g_win_ui_host, &g_ui_host_interface)`
> 注入插件 `_SA_UIMain`。M6 可以复用这个模式，或把这套真实 GUI host 封装进
> `client.exe`。

M6 建议拆成三个阶段，避免把跨平台构建、真实 GUI、网络闭环、2048 业务逻辑混在一起：

| 阶段 | 目标 | 通过标准 |
|------|------|----------|
| M6a | Windows/GKC GUI 基础与插件加载 | `client.exe` 能在 Windows 上加载 `client_viewer.dll`，创建真实窗口，显示本地测试图案，正常退出 |
| M6b | Client 网络 viewer 闭环 | `client.exe + client_viewer.dll` 能连接 Gateway，显示 `demo_app` 的 PIXEL_DATA，并把鼠标/`KB_F1` 输入发回 AppHost |
| M6c | 2048 AppHost demo | 新增或替换服务端 2048 插件，Client 发送方向键后窗口中的 2048 状态可交互更新 |

M6a/M6b 是本 milestone 的主线；M6c 可以紧随其后做，但不应阻塞 M6 基础闭环验收。

### 6.1 Client 插件接口

Client 插件使用与 AppHost 插件相同的 GKC 标准接口：

```cpp
// plugins/client_viewer/client_viewer.cpp
// Client 插件链接 GKC 的 GkcGui.cpp（gkc_gui_runtime），由它提供
// extern "C" int _SA_UIMain(...) shim；插件作者只需实现：

namespace program_entry_point {
    int GuiMain(const GKC::ConstArray<GKC::ConstStringS>& args);
}
```

`client_viewer.dll` / `.so` 与 AppHost 插件一样链接 GKC 的 plugin-side runtime
（`GkcDef.cpp` / `GkcSAMain.cpp` / `GkcGui.cpp`），由它导出 `_SA_UIMain` 并持有插件本地
`GKC::g_ui_host`。区别在于宿主传入的 `IUiHost`：

| 组件 | 宿主传入的 `IUiHost` | 目的 |
|------|----------------------|------|
| AppHost | 我们的 `ui_host_impl` fake/headless host | 捕获 `DoDraw` 输出，打包 `PIXEL_DATA` |
| Client | GKC 真实 GUI host（Windows Win32 / Linux Wayland） | 创建真实窗口，接收本地鼠标键盘，显示像素 |

Client 可执行程序通过 `LoadLibrary` / `dlopen` 加载 viewer 插件，通过
`GetProcAddress` / `dlsym("_SA_UIMain")` 取入口，构造真实 GKC GUI `LcInterface<IUiHost>`，
调用 `_SA_UIMain(lcHost, args)`。`_SA_UIMain` shim 把 `lcHost` 写入插件本地
`g_ui_host`，再调用 `ProgramEntryPoint::GuiMain(args)`。

`GuiMain(args)` 内建议只做 UI 层编排：
1. 从 `args` 解析 Gateway 地址、端口、要启动的 AppHost appName（默认 `demo_app`）。
2. 创建 `ViewerWindow : ToplevelImpl<ViewerWindow>`。
3. 通过 Client runtime 连接 Gateway 并发送 `SPAWN_APP(appName)`。
4. `ViewerWindow.Show(true)` 后进入 `GuiHelper::Loop()`。
5. `DoDraw` 从 `PixelRenderer` blit 到 `pDraw->pBuffer`。
6. `DoMouse` / `DoKeyboard` 调用 Client runtime 发送 `INPUT_EVENT`。

> 职责边界：`client_viewer` 负责真实窗口与用户输入；`src/client` 负责网络连接、
> `MessageHandler`、`PixelRenderer`、向插件暴露发送输入/触发重绘的 runtime API。

### 6.2 需要实现的文件

| 文件 | 内容 |
|------|------|
| `third_party/BUILD` | 补齐 Windows GKC runtime / GUI host 目标；`GkcSys` 已有 Linux/Windows `select`，M6 还需要真实 GUI host 或等价封装目标 |
| `src/client/plugin_loader.h/.cpp` | `LoadLibrary`（Windows）/ `dlopen`（Linux）加载 `.dll`/`.so`，取 `_SA_UIMain`，调用入口 |
| `src/client/client_runtime.h/.cpp` 或 `client.h/.cpp` | Client 网络 runtime：连接 Gateway，发送 `SPAWN_APP` / `INPUT_EVENT`，接收 `SESSION_ACK` / `PIXEL_DATA` / `ERROR_RESP` |
| `src/client/pixel_renderer.h/.cpp` | 线程安全像素缓冲区；支持 `PIXEL_DATA` dirty rect apply、frameSeq 过滤、`DoDraw` blit |
| `src/client/main.cpp` | Windows `client.exe` 入口；解析 `--plugin/--gateway-host/--gateway-port/--app`；初始化真实 GKC GUI host；加载 viewer 插件 |
| `plugins/client_viewer/client_viewer.cpp` | `ToplevelImpl<ViewerWindow>`：创建窗口、DoDraw 贴像素、DoMouse/DoKeyboard 调用 runtime 发送输入、DoClose 退出 |
| `plugins/client_viewer/BUILD` | 构建 Linux `.so` / Windows `.dll`；链接 plugin-side GKC runtime；输出名便于手动运行 |
| `src/client/pixel_renderer_test.cpp` | headless 单元测试：dirty rect apply、越界/长度校验、frameSeq 过滤、blit |
| `src/client/m6_client_test.cpp` | headless 协议测试：fake Gateway 验证 SPAWN_APP、SESSION_ACK、PIXEL_DATA apply、INPUT_EVENT pack/send |
| `docs/design/M6.md` | M6 完成后记录 Windows 运行方式、GKC GUI host 链接方式、手动联调步骤 |

### 6.3 Client 插件加载流程

```
client.exe main()
  ↓
初始化或封装 GKC real UIHost
  Windows: Win32 host（GKC util/gui/uihost 的 _system_/Windows 实现）
  Linux: Wayland host（可选，用于本地 Linux 手动验证）
  ↓
plugin_loader.load("client_viewer.dll")
  ↓
_SA_UIMain(lcHost{real_gkc_ui_host}, args)
  ↓ （shim 内：g_ui_host = lcHost; program_entry_point::GuiMain(args)）
插件创建 ViewerWindow（ToplevelImpl）
  ↓
Client runtime StartConnect → Gateway public_port
  ↓
发送 SPAWN_APP(appName，默认 "demo_app")
  ↓
收到 SESSION_ACK → 开始接收 PIXEL_DATA
  ↓
ViewerWindow.Show(true) + GuiHelper::Loop()
```

运行示例（Windows）：

```
client.exe ^
  --plugin=.\plugins\client_viewer\client_viewer.dll ^
  --gateway-host=<gateway-ip> ^
  --gateway-port=19000 ^
  --app=demo_app
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
    → 将 GKC 鼠标消息映射为 InputEventType
        MOUSE_EVENT_MOVE                      → MOUSE_MOVE
        MOUSE_EVENT_DOWN + MOUSE_BUTTON_LEFT  → MOUSE_LEFT_DOWN
        MOUSE_EVENT_UP   + MOUSE_BUTTON_LEFT  → MOUSE_LEFT_UP
        MOUSE_EVENT_DOWN + MOUSE_BUTTON_RIGHT → MOUSE_RIGHT_DOWN
        MOUSE_EVENT_UP   + MOUSE_BUTTON_RIGHT → MOUSE_RIGHT_UP
        MOUSE_EVENT_WHEEL                     → MOUSE_SCROLL（M6 仍无 delta）
    → 使用 src/common/input_event.h 的 pack_mouse_input_event()
    → Client runtime 发送 CmdType::INPUT_EVENT 到 Gateway

DoKeyboard(pKb)
    → pKb->btDown ? KEY_DOWN : KEY_UP
    → keyCode = pKb->btKey（仍只传 btKey，不传 chFull）
    → 使用 pack_keyboard_input_event()
    → Client runtime 发送 CmdType::INPUT_EVENT 到 Gateway
```

> M6 仍不实现可打印字符输入协议扩展。2048 和 M5 demo 都只需要 `btKey`
> 方向键 / `KB_F1`，不依赖 Space 或 `chFull`。

### 6.5 测试要求

| 测试类型 | 测试用例 | 期望结果 |
|----------|----------|----------|
| 自动化单元 | `pixel_renderer_test` | dirty rect 正确写入缓冲区；旧 frameSeq 可丢弃；错误长度/越界不 crash |
| 自动化 headless | `m6_client_test` | fake Gateway 下 Client 发送 `SPAWN_APP`，接收 `SESSION_ACK/PIXEL_DATA`，PixelRenderer 更新，输入事件 pack/send 正确 |
| 手动端到端 M6b | Windows `client.exe` + `client_viewer.dll` + Gateway + AppHost(demo_app) | 窗口显示蓝色背景，鼠标点击小矩形切色，键盘 `KB_F1` 切换背景，稳定运行 60 秒无 crash |
| 手动端到端 M6c | Windows `client.exe` + `client_viewer.dll` + Gateway + AppHost(2048) | 方向键能操作 2048，窗口画面随服务端状态更新 |

**通过标准（M6 主线）**：`pixel_renderer_test` + `m6_client_test` PASSED；Windows 手动端到端
demo_app 稳定运行 60 秒；`client_viewer.dll` 可替换，Client 不把 demo_app 业务逻辑写死。

### 6.6 GKC / Bazel 跨平台前置工作

M6 的主要风险不在协议，而在 Windows 构建与 GKC GUI host 接入。开工前需要明确：

| 项目 | 要求 |
|------|------|
| `GkcSys_windows` | 当前 `third_party/BUILD` 已通过 `cc_import` 指向 `GKC_BUILD/release/bin/Release/GkcSys.dll/.lib`；需要确认 Windows 构建产物路径稳定，并放入 Bazel runfiles / 发布目录 |
| real GUI host | GKC `util/gui/uihost` 已有 Windows `_system_/Windows` 实现；M6 需要选择：直接构建/复用 `uihost.exe`，或把 `g_win_ui_host + g_ui_host_interface` 封装到 `client.exe` |
| plugin runtime | `client_viewer.dll` 必须像 `demo_app.so` 一样拥有插件本地 `g_ui_host` 和 `_SA_UIMain`；Client exe 不应错误链接出另一份 plugin-side `g_ui_host` 并让插件拿不到注入 |
| BUILD select | `src/client`、`plugins/client_viewer`、`third_party` 需要 Linux/Windows `select()`：Linux 输出 `.so`，Windows 输出 `.dll/.exe`，并处理 `LoadLibrary` / `dlopen` 差异 |
| 手动运行包 | Windows 运行目录至少包含 `client.exe`、`client_viewer.dll`、`GkcSys.dll`、必要 MSVC runtime，以及可访问的 Gateway IP/port |

### 6.7 2048 与 M6 的关系

2048 不应作为 M6a/M6b 的阻塞项。M6b 跑通 demo_app 后，2048 的网络/显示/输入基础已经具备；
剩余工作主要是 AppHost 业务插件：

- 新增 `plugins/app_2048`（或后续替换 demo_app）。
- 服务端维护 4×4 board、score、随机生成 tile、game over 状态。
- `DoKeyboard` 响应 `KB_Left/KB_Right/KB_Up/KB_Down`，状态变化后 `Damage(full window)`。
- `DoDraw` 绘制棋盘、方块和数字；初版可使用简单 bitmap 数字或色块，先不做复杂字体。
- Gateway allowlist 增加 `app_2048`，Client 用 `--app=app_2048` 启动。

因此 PM 拆分建议是：先完成 M6b 的通用 viewer，再做 M6c/后续 2048。这样如果 2048
画面不对，问题集中在游戏插件；如果 demo_app 都跑不通，问题集中在 Client/GKC/网络链路。

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
- [ ] `bazel test //src/apphost:m4_plugin_test` — PASSED（含 `PluginLoadsAndSeesFakeHost` / `InitialFullFrame` / `DirtyRectIsSubset` / `MultiPixelPacketOrdering`）
- [ ] `bazel test //src/apphost:pixel_capture_test` — PASSED（含 `CaptureRectByteOrder` / `PackPixelDataConvertsBgraToArgb`）
- [ ] `bazel test //src/apphost:apphost_lifecycle_test` — PASSED（连接 / HEARTBEAT / CLOSE_SESSION / 多包顺序）
- [ ] `src/apphost/pixel_buffer.h/.cpp` 已删除；窗口像素由 `ui_host_impl::WindowRuntime` 持有 `std::vector<color_quad>`
- [ ] 旧 `src/apphost/apphost_test.cpp`（M2 静态红帧测试）已替换为上面的 `m4_plugin_test` + `apphost_lifecycle_test`
- [ ] `ui_host_impl` 使用 `pDraw->rcPaint`，不保存上一帧；`Show(true)` 触发首帧整窗 DRAW，`Damage(rect)` 触发同步局部 DRAW
- [ ] `plugin_loader` 通过 `dlsym("_SA_UIMain")` 加载入口；`demo_app.so` 入口处 `g_ui_host.GetFunc() != nullptr`
- [ ] AppHost 二进制不链接 `GkcGui.cpp`；`gkc_gui_runtime` 仅由插件 .so 链接
- [ ] PIXEL_DATA body 中像素已按 `[A,R,G,B]` 大端排列（与 `pBuffer` 内存中的 `B,G,R,A` 完成转换）
- [ ] fake host 的 `Quit` 线程安全，可由 IoPool 线程在收到 `CLOSE_SESSION` 时调用，唤醒主线程 `Loop`

### M5 完成标准
- [ ] `bazel test //src/apphost:m5_input_test` — AppHost 局部输入测试全部 PASSED
- [ ] `bazel test //src/gateway:gateway_m5_input_test` — 真实 Gateway + 真实 AppHost + fake Client 联立输入测试 PASSED
- [ ] MOUSE_EVENT / KEYBOARD_EVENT 经 PostWork 安全注入插件，无数据竞争

### M6 完成标准
- [ ] `bazel test //src/client:m6_client_test` — PASSED
- [ ] 手动测试：GKC 窗口稳定运行 60 秒，鼠标/键盘响应正常，无 crash
- [ ] Client 是插件加载器，client_viewer.so/.dll 独立可替换
- [ ] 键码直接使用 GKC `KB_*`，未引入额外映射层
