# ProtocolDesign — 协议数据流与 Client 实现指南

> 本文档覆盖：Client ↔ Server 传输内容、IoPool 上下文关系、
> 完整数据流举例、Windows / Linux Client 实现思路。

> 本文档中的 GKC 路径、接口名和线程模型描述已按 submodule
> `third_party/GKC` 的 **`51049a4`** 快照重新校对。
> 更完整的上游结构说明见 [GKC_usage.md](/home/magnus/dev/ProtocolDesign/docs/GKC/GKC_usage.md)。

---

## 目录

1. [Client 发给服务端的内容](#1-client-发给服务端的内容)
2. [IoPool 上下文关系](#2-iopool-上下文关系)
3. [完整数据流举例](#3-完整数据流举例)
4. [Windows Client 实现思路](#4-windows-client-实现思路)
5. [Linux 原生 Client](#5-linux-原生-client)

---

## 1. Client 发给服务端的内容

### Client → Gateway → AppHost

| 包类型 | CmdType | 说明 |
|--------|---------|------|
| `HEARTBEAT` | `0x01` | 心跳，维持会话，Body 为空 |
| `SPAWN_APP` | `0x02` | 建立会话，告知要启动哪个 .so |
| `INPUT_EVENT` | `0x20` | 鼠标或键盘事件，Body[0] 区分子类型 |
| `CLOSE_SESSION` | `0xFE` | 关闭会话 |

### AppHost → Gateway → Client

| 包类型 | CmdType | 说明 |
|--------|---------|------|
| `SESSION_ACK` | `0x03` | 分配 Session ID |
| `PIXEL_DATA` | `0x10` | 脏矩形像素数据（只传变化区域） |
| `ERROR_RESP` | `0xFF` | 错误通知 |

---

## 2. IoPool 上下文关系

IoPool 里**每一个 TCP 连接**对应一个 `pIoContext`，这是你自定义的结构体指针。
IoPool 只负责保存和回传它，不关心内容。

### 正确的 per-handle 数据设计（使用 free_list + 原子自旋锁）

IoPool **不管理**上下文对象的生命周期，使用者必须自行分配和释放。
正确做法是把连接上下文组织成 `free_list<ConnContext>`，用一个原子整型作自旋锁，
每次接受新连接时从池中取一个节点，断开时归还，**绝不使用** `new`/`delete`。

```cpp
// Gateway 侧：每个客户端连接一个 Session
// 必须继承 node_base，使 free_list<> 可以通过 m_pNext 链接空闲节点。
struct ClientSession : public node_base {
    uintptr        id_           = 0;      // 本连接的 IoPool 句柄
    ClientSession* next_active_  = nullptr; // 活跃连接的侵入式单链表
    _IoFunc        io_func_;               // 每连接回调，Exec 在构造时设置
    uint32_t       session_id_   = 0;
    uintptr        apphost_conn_ = 0;      // 对应的 AppHost 连接句柄
    MessageParser  parser_;

    ClientSession() noexcept { io_func_.Exec = on_io_event; }
    static int on_io_event(void* ctx, int type, uintptr uparam) noexcept;
};

// AppHost 侧：与 Gateway 的连接上下文（结构相同，字段不同）
struct AppHostConn : public node_base {
    uintptr       id_          = 0;
    AppHostConn*  next_active_ = nullptr;
    _IoFunc       io_func_;
    uint32_t      session_id_  = 0;
    PixelBuffer   pixel_buf_;

    AppHostConn() noexcept { io_func_.Exec = on_io_event; }
    static int on_io_event(void* ctx, int type, uintptr uparam) noexcept;
};

// 连接池（两种类型各一个），加上自旋锁（0=未持有，1=持有）
free_list<ClientSession> session_pool_;
ClientSession*           active_head_ = nullptr;
volatile int             lock_        = 0;
```

### alloc / release 与 SetHandleFunc 模式

```cpp
// 从池中分配一个节点（自旋锁保护）
ClientSession* alloc_session() noexcept {
    ClientSession* s = nullptr;
    while (atomic_compare_exchange((int&)lock_, 0, 1)) ;
    {
        call_result cr = session_pool_.FetchFreeNode(s);
        if (cr.IsSucceeded()) {
            call_constructor(*s);           // 先构造，再 Pick（constructor 失败则节点不丢失）
            session_pool_.PickFreeNode();
            s->next_active_ = active_head_;
            active_head_    = s;
        }
    }
    atomic_compare_exchange((int&)lock_, 1, 0);
    return s;
}

// 归还节点
void release_session(ClientSession* s) noexcept {
    call_destructor(*s);
    while (atomic_compare_exchange((int&)lock_, 0, 1)) ;
    ClientSession** pp = &active_head_;
    while (*pp && *pp != s) pp = &(*pp)->next_active_;
    if (*pp == s) *pp = s->next_active_;
    session_pool_.PutFreeNode(s);
    atomic_compare_exchange((int&)lock_, 1, 0);
}

// 监听级回调（仅处理 IO_TYPE_ACCEPT_INIT）
static int on_listen_event(void* ctx, int type, uintptr uparam) noexcept {
    if (type != IO_TYPE_ACCEPT_INIT) return 0;

    auto* self = static_cast<GatewayImpl*>(ctx);
    ClientSession* s = self->alloc_session();
    if (!s) return 0;   // 连接池已满，拒绝此连接

    s->id_ = uparam;    // uparam 即新连接的 id

    bool cancelled = false;
    self->pool_.GetFunc()->SetHandleFunc(
        self->pool_.GetContext(), uparam, s->io_func_, s, cancelled);

    if (cancelled) { self->release_session(s); return 0; }
    return 1;   // 接受连接
}
```

**关键点**：
- `ConnContext` 把自己（`s`）作为 `pIoContext` 传给 `SetHandleFunc`，此后所有事件都直接回调到 `s->io_func_.Exec(s, ...)`
- `free_list<>` 是预分配的内存池，取节点顺序严格是 `FetchFreeNode → call_constructor → PickFreeNode`
- 自旋锁保护 `FetchFreeNode`/`PickFreeNode`/`PutFreeNode` 与活跃链表操作
- 关闭时遍历 `active_head_` 主动调用 `call_destructor + PutFreeNode`，防止 `BEFORE_CLOSE` 未触发导致泄漏

### 上下文关系图

```
IoPool
├── 连接 fd=5  →  pIoContext = ClientSession { id_=5, session_id_=0x01, apphost_conn_=9, parser_ }
│                  └── IO_TYPE_RECEIVED → on_io_event(s, ...) → s->parser_.feed()
├── 连接 fd=7  →  pIoContext = ClientSession { id_=7, session_id_=0x02, apphost_conn_=11, parser_ }
└── 连接 fd=9  →  pIoContext = AppHostConn  { id_=9, session_id_=0x01, pixel_buf_ }
                   └── IO_TYPE_RECEIVED → 捕获 pDraw->rcPaint 像素后把 PIXEL_DATA 包发回 fd=5
```

---

## 3. 完整数据流举例

以"鼠标点击触发界面重绘"为例：

```
1. Client 发送 INPUT_EVENT 包
   Header: [Magic(4)][SessionId(4)][0x20(1)][bodyLen(4)]
   Body:   [eventType=LEFT_DOWN(1)][x=100(4)][y=200(4)][timestamp(8)]

2. Gateway 收到
   → MessageParser 解包
   → 读取 SessionId，查路由表找到对应 AppHost 连接句柄
   → 原包转发给 AppHost（不修改内容）

3. AppHost 收到
   → MessageParser 解包，识别 INPUT_EVENT，读 Body[0] 确认为鼠标事件
   → 经 PostWork() 投递到主线程，注入 ui_host_impl 模拟 UI_MESSAGE_MOUSE
   → 业务 .so 的 DoMouse() 修改像素，GKC 触发 DoDraw(pDraw)

4. ui_host_impl 拦截 DoDraw
   → pDraw->rcPaint = 本次脏矩形（GKC 自维护，无需逐帧比对）
   → pixel_capture(pDraw->pBuffer, pDraw->iWidth, pDraw->rcPaint) 裁出像素块

5. AppHost 打 PIXEL_DATA 包
   Header: [Magic(4)][SessionId(4)][0x10(1)][bodyLen(4)]
   Body:   [frameSeq(4)][rectLeft(4)][rectTop(4)][rectRight(4)][rectBottom(4)][dataLen(4)][pixelData...]

6. Gateway 查路由表 → 转发给对应 Client

7. Client 解包
   → 把像素写入本地 frameBuf_ 对应区域
   → 触发窗口重绘（BitBlt / XPutImage）
```

---

## 4. GKC Client 实现思路

Client 与 Gateway / AppHost 一样，全程使用 GKC（无 FLTK 依赖）。
- **GUI 层**：`GkcGui.h` 的 `ToplevelImpl<ClientWindow>`，支持 Linux（Wayland）和 Windows（Win32）
- **网络层**：GKC `IoPool`（Linux epoll / Windows IOCP）

### 线程模型

```
主线程（GKC GuiHelper::Loop()）
├── DoDraw()     ← GKC 触发重绘，把 PixelRenderer 缓冲区 memcpy 到 pDraw->pBuffer
├── DoMouse()    ← GKC 推送鼠标事件，打包为 INPUT_EVENT（eventType=鼠标子类型），经 IoPool 发出
├── DoKeyboard() ← GKC 推送键盘事件，打包为 INPUT_EVENT（eventType=KEY_DOWN/UP），经 IoPool 发出
└── DoWork()     ← PostWork 回调（由 IoPool 线程投递），触发脏矩形局部重绘
IoPool 线程（GKC 内部管理）
└── _IoFunc::Exec → MessageParser.Feed() → 解析 PIXEL_DATA → 写 PixelRenderer → PostWork()
TimerImpl（在主线程触发）
└── 定时发送 HEARTBEAT 协议包
```

### 核心代码骨架

```cpp
// ClientWindow 继承 ToplevelImpl（GUI 回调）和 WorkImpl（跨线程调度）
class ClientWindow
    : public GKC::ToplevelImpl<ClientWindow>
    , public GKC::WorkImpl<ClientWindow>
    , public GKC::TimerImpl<ClientWindow>
{
public:
    bool Initialize(int w, int h) {
        if (!Create(true, w, h)) return false;
        // 启动 IoPool 连接 Gateway
        StartNetworkConnection("127.0.0.1", 19000);
        // 启动心跳计时器（每 30s）
        m_heartbeatTimer = AddTimer(30000);
        return true;
    }

    // GKC 重绘回调：把 PixelRenderer 数据贴到窗口
    void DoDraw(GKC::UiMessageDraw* pDraw) noexcept {
        renderer_.Blit(pDraw->pBuffer, pDraw->iWidth,
                       pDraw->rcPaint);
    }

    // GKC 鼠标回调：直接取字段，打包发送
    void DoMouse(GKC::UiMessageMouse* pMouse) noexcept {
        if (pMouse->uEvent == MOUSE_EVENT_MOVE ||
            pMouse->uEvent == MOUSE_EVENT_DOWN ||
            pMouse->uEvent == MOUSE_EVENT_UP) {
            SendMouseEvent(pMouse->uEvent, pMouse->btButton,
                           pMouse->x, pMouse->y);
        }
    }

    // GKC 键盘回调：KB_* 和 KB_STATE_* 直接写入协议包，无需映射
    void DoKeyboard(GKC::UiMessageKeyboard* pKb) noexcept {
        SendKeyboardEvent(pKb->btKey, pKb->btState, pKb->btDown);
    }

    // WorkImpl 回调（主线程执行）：处理来自 IoPool 线程的像素更新
    void DoWork() noexcept {
        // PixelRenderer 已更新，通知 GKC 重绘脏矩形
        // （通过 SetClose 或其他机制触发 DoDraw）
    }

    // 计时器回调：发送心跳
    void DoWork() noexcept { /* 心跳由 TimerImpl 触发 */ }
    void DoClose() noexcept { GuiHelper::Quit(); }

private:
    PixelRenderer renderer_;
    MessageParser parser_;
    uintptr       m_heartbeatTimer = 0;
};
```

### IoPool 接收 PIXEL_DATA

```cpp
// IoPool 回调（在 IoPool 内部线程执行）
_IoFunc ioFunc{
    [](void* pCtx, int iType, uintptr uParam) noexcept -> int {
        auto* win = static_cast<ClientWindow*>(pCtx);
        if (iType == IO_TYPE_RECEIVED) {
            auto* info = reinterpret_cast<_IoRecvInfo*>(uParam);
            win->parser_.Feed(info->p, info->len);
            Packet pkt;
            while (win->parser_.next_packet(pkt) == ParseResult::OK) {
                if (pkt.header.cmd_type == CmdType::PIXEL_DATA) {
                    // 解析脏矩形，写入 PixelRenderer（mutex 保护）
                    win->renderer_.ApplyPixelData(pkt.body.data(), pkt.body.size());
                    // 投递到主线程触发重绘
                    win->PostWork();
                }
            }
        }
        return 0;
    }
};
```

---

## 5. 跨平台说明

GKC 统一了 Linux 和 Windows 的 Client 实现，无需平台特化分支：

| 功能 | GKC 抽象 | Linux 底层 | Windows 底层 |
|------|---------|-----------|------------|
| 网络 I/O | `IoPool` | epoll | IOCP |
| 窗口 / 绘制 | `ToplevelImpl` + `DoDraw` | Wayland `wl_shm` | Win32 GDI / DWM |
| 鼠标/键盘 | `DoMouse` / `DoKeyboard` | Wayland input | Win32 `WM_*` |
| 定时器 | `TimerImpl` | `timerfd` / `wl_callback` | `SetTimer` |
| 跨线程 | `PostWork()` | eventfd | PostMessage |

同一套 `src/client/` 源码在 Windows 和 Linux 上均可编译运行，Bazel BUILD 文件按平台链接对应的 GKC 运行时，业务代码无需任何 `#ifdef`。
