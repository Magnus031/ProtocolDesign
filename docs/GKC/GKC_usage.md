# GKC 库使用参考

> 本文档面向 ProtocolDesign 项目的 **全部三个组件（Client / Gateway / AppHost）**，
> 重点覆盖线程池、UIHost 接口、事件系统、日志、网络等核心模块。
> Client / Gateway / AppHost 均基于 GKC，无 FLTK 依赖。

> 当前内容已按 submodule `third_party/GKC` 的 **`51049a4`** 快照校对。
> 本次上游更新相对旧快照的直接变化是：
> - 新增 `public/include/base/GkcCga.h` 及 `_cga_/` 下的基础 CGA / raster 头文件
> - 在基础层补充了 multi-dimensional array 相关类型（`MdaInfo`、`MdaView`、`MdaIterator`、`MdaHelper` 等）

---

## 目录

1. [重要头文件路径](#1-重要头文件路径)
2. [GKC 异步回调范式总览](#2-gkc-异步回调范式总览)
3. [线程池](#3-线程池)
   - [WorkPool — 通用任务线程池](#31-workpool--通用任务线程池)
   - [IoPool — I/O 事件驱动线程池](#32-iopool--io-事件驱动线程池)
   - [两者对比](#33-两者对比)
4. [UIHost 接口体系](#4-uihost-接口体系)
   - [几何类型](#41-几何类型)
   - [IUiHost 主接口](#42-iuihost-主接口)
   - [IUiWindow 及其子类](#43-iuiwindow-及其子类)
   - [C++ 便捷包装类](#44-c-便捷包装类)
5. [事件与消息系统](#5-事件与消息系统)
   - [消息类型](#51-消息类型)
   - [绘制消息（Draw）](#52-绘制消息draw)
   - [鼠标消息（Mouse）](#53-鼠标消息mouse)
   - [键盘消息（Keyboard）](#54-键盘消息keyboard)
   - [文本输入消息（Text）](#55-文本输入消息text)
   - [注册消息处理器](#56-注册消息处理器)
   - [自定义事件回调](#57-自定义事件回调)
6. [窗口类型与绘制机制深解](#6-窗口类型与绘制机制深解)
   - [三种窗口类型（Wayland Surface 角色）](#61-三种窗口类型wayland-surface-角色)
   - [像素缓冲区与绘制流水线](#62-像素缓冲区与绘制流水线)
   - [GKC 不提供任何控件](#63-gkc-不提供任何控件)
   - [与 AppHost 脏矩形检测的关系](#64-与-apphost-脏矩形检测的关系)
7. [.so 插件开发规范](#7-so-插件开发规范)
   - [必须导出的符号](#71-必须导出的符号)
   - [AppHost 加载流程](#72-apphost-加载流程)
8. [日志系统](#8-日志系统)
9. [内置 Socket 封装](#9-内置-socket-封装)
10. [内存管理工具](#10-内存管理工具)
11. [完整使用示例](#11-完整使用示例)

---

## 1. 重要头文件路径

| 功能 | 头文件 |
|------|--------|
| 基础定义、智能指针 | `GKC/public/include/base/GkcDef.h` |
| 计算几何 / raster 新入口 | `GKC/public/include/base/GkcCga.h` |
| GUI 接口和包装类 | `GKC/public/include/base/GkcGui.h` |
| UI 几何类型 | `GKC/public/include/base/system/ui_types.h` |
| GkcSys 运行时接口（线程池、日志） | `GKC/RT/GkcSys/public/_GkcSys.h` |
| 系统工具总入口 | `GKC/public/include/sys/GkcSys.h` |
| UIHost 公共接口定义 | `GKC/public/include/base/GkcDef.h` |
| UIHost 平台实现私有入口 | `GKC/util/private/include/ui/UIDef.h` |
| UIHost 实现侧定义 | `GKC/util/gui/uihost/include/base/SysDef.h` |
| Linux Socket 内部工具 | `GKC/RT/GkcSys/include/base/system/Linux/_util_/_x_socket.h` |

---

## 2. GKC 异步回调范式总览

GKC 的全部事件系统均为**异步回调**，没有任何阻塞调用。三个组件（Client / Gateway / AppHost）共享同一套回调体系：

| 回调类型 | 触发线程 | 函数签名 | 典型用途 |
|---------|---------|---------|---------|
| `_IoFunc::Exec` | IoPool 内部线程 | `int(void* pCtx, int iType, uintptr uParam)` | 网络事件：收包、连接建立/断开 |
| `UiMessageHandler::Process` | 主线程 | `void(void* pCtx, uint uMsg, uintptr uParam)` | UI 事件：绘制、鼠标、键盘、关闭 |
| `WorkProc::Exec` | WorkPool 线程 | `void(void* pCtx)` | CPU 任务：差分检测、压缩 |

**跨线程安全投递**：IoPool / WorkPool 线程需要更新 UI 状态时，直接调用 `g_ui_host.GetFunc()->PostWork()`，
或在窗口对象中通过 `WorkImpl<T>::PostWork()` / `TimerImpl<T>` 使用 GKC 已提供的包装层，将任务封送回主线程。

**MessageParser 与回调的关系**：  
`MessageParser` 是我们的 TCP 分帧解析器，属于业务代码，不是 GKC 的一部分。它在 `_IoFunc::Exec` 的 `IO_TYPE_RECEIVED` 分支内使用：

```
IoPool 触发 IO_TYPE_RECEIVED
    → _IoFunc::Exec 回调（IoPool 线程）
        → MessageParser.Feed(info->p, info->len)   // 喂原始字节
        → while (parser.next_packet(pkt) == OK)    // 取完整协议包
            → 按 pkt.header.cmd_type 分发业务逻辑
        → 需要更新 UI 时：PostWork() 回主线程
```

**各组件的线程模型概览：**

```
Client
├── 主线程：GuiHelper::Loop() —— DoDraw / DoMouse / DoKeyboard / DoWork
└── IoPool 线程：收 DIRTY_RECT → MessageParser → 写 PixelRenderer → PostWork()

Gateway
├── 主线程：初始化 + 等待退出信号
└── IoPool 线程：收发所有包 → MessageParser → 按 SessionID 路由转发

AppHost
├── 主线程：加载 .so 插件 + ui_host_impl 事件循环
├── IoPool 线程：收 MOUSE/KEYBOARD_EVENT → PostWork() 注入 ui_host_impl
└── WorkPool 快速池：异步打包并发送 PIXEL_DATA（像素数据压缩等耗时操作走慢速池）
```

---

## 3. 线程池

### 3.1 WorkPool — 通用任务线程池

用于提交任意 CPU 计算任务，分为**快速池**（short task）和**慢速池**（long task）两个实例。

**核心结构：**

```cpp
// 任务回调
struct work_proc {
    void (*Exec)(void* pContext) noexcept;
};
typedef work_proc WorkProc;
```

**提交任务：**

```cpp
GKC::WorkProc proc{
    [](void* p) noexcept {
        // 工作内容
    }
};

// 提交到快速池（bSlow = false）
_WorkPool_Submit(false, proc, pContext);

// 提交到慢速池（bSlow = true，适合耗时操作）
_WorkPool_Submit(true, proc, pContext);
```

**等待完成：**

```cpp
_WorkPool_WaitForZero(false);  // 等待快速池全部完成
_WorkPool_WaitForZero(true);   // 等待慢速池全部完成
```

**适用场景（项目中）：**

| 组件 | 快速池（bSlow=false） | 慢速池（bSlow=true） |
|------|---------------------|---------------------|
| AppHost | PIXEL_DATA 打包发送（pixel_capture 完成后立即投递） | 像素数据压缩（耗时长） |
| Client | 像素解压缩 | 大帧数据处理 |
| Gateway | 一般不使用（I/O 全走 IoPool） | 进程健康检查等管理任务 |

---

### 3.2 IoPool — I/O 事件驱动线程池

专为网络 I/O 设计，基于 epoll（Linux）/ IOCP（Windows），事件驱动模型。

**事件类型枚举：**

```cpp
enum {
    IO_TYPE_NONE = 0,
    IO_TYPE_ACCEPT_ERROR,
    IO_TYPE_ACCEPT_INIT,   // 新连接初始化，uParam: id，return 1 接受 / 0 拒绝
    IO_TYPE_ACCEPTED,      // 连接已接受
    IO_TYPE_CONNECT_ERROR,
    IO_TYPE_CONNECTED,     // 连接成功
    IO_TYPE_RECV_ERROR,
    IO_TYPE_RECV_TIMEOUT,
    IO_TYPE_RECEIVED,      // 收到数据，uParam: _IoRecvInfo*
    IO_TYPE_SEND_ERROR,
    IO_TYPE_SENT,          // 发送完成
    IO_TYPE_BEFORE_CLOSE,  // 即将关闭
    IO_TYPE_MAX
};
```

**核心结构体：**

```cpp
struct _IoRecvInfo {
    byte*   p;      // 接收到的数据指针
    uintptr len;    // 数据字节数
};

struct _IoFunc {
    int (*Exec)(void* pContext, int iType, uintptr uParam) noexcept;
};

struct _IIoPool {
    // 监听指定端口（服务端）
    uintptr (*StartListen)(void* pContext, uint uPort,
                           const _IoFunc& ioFunc, void* pIoContext,
                           bool& bCancelled) noexcept;

    // 发起连接（客户端）
    uintptr (*StartConnect)(void* pContext, const GKC::CharS* szHost, uint uPort,
                            const _IoFunc& ioFunc, void* pIoContext,
                            bool& bCancelled) noexcept;

    // 更换某个连接的事件处理函数
    void (*SetHandleFunc)(void* pContext, uintptr id,
                          const _IoFunc& ioFunc, void* pIoContext,
                          bool& bCancelled) noexcept;

    // 获取发送缓冲区（开始发送）。
    // uLen 必须 > 0 且 <= 4000（内部 BUFFER_SIZE 限制）。
    // 如果上一次发送尚未完成（iSend=1），返回 NULL。
    byte* (*BeginInput)(void* pContext, uintptr id, uint uLen,
                        bool& bCancelled) noexcept;

    // 将缓冲区交给 epoll，触发 EPOLLOUT 后实际发送。
    // 发送完成后触发 IO_TYPE_SENT 回调，iSend 复位为 0。
    bool (*EndInput)(void* pContext, uintptr id) noexcept;

    // 关闭连接
    void (*DisableHandle)(void* pContext, uintptr id) noexcept;
};
```

**获取 IoPool 实例（全局单例，进程内只可初始化一次）：**

```cpp
GKC::LcInterface<_IIoPool> pool = _IoPool_Fetch();
// 调用接口方法的正确写法（LcInterface 无 operator->，需显式获取 func 和 context）：
//   pool.GetFunc()->StartListen(pool.GetContext(), port, ioFunc, ctx, cancelled);
// 使用完毕后释放（停止 epoll 线程，关闭所有连接）
_IoPool_Disable();
```

**per-handle 数据设计原则**

IoPool 本身不管理上下文对象的生命周期。正确做法是：
1. 定义一个 `ConnContext` 结构体，继承 `node_base`，内嵌 `_IoFunc io_func_`，
   把自己的 `this` 作为 `pIoContext` 传入 `SetHandleFunc`。
2. 用 `free_list<ConnContext>` + 原子自旋锁管理池，**不使用** `new`/`delete`。
3. 在 `IO_TYPE_ACCEPT_INIT` 中从池取节点，调用 `SetHandleFunc` 绑定；
   在 `IO_TYPE_BEFORE_CLOSE` 中归还节点到池。

```cpp
// Per-connection context: inherits node_base for free_list<> linkage.
struct ConnContext : public node_base {
    uintptr      id_          = 0;
    ConnContext* next_active_ = nullptr;
    _IoFunc      io_func_;             // Exec set in constructor
    MessageParser parser_;
    Impl*        server_      = nullptr;

    ConnContext() noexcept { io_func_.Exec = on_io_event; }

    static int on_io_event(void* ctx, int type, uintptr uparam) noexcept {
        auto* conn = static_cast<ConnContext*>(ctx);
        switch (type) {
        case IO_TYPE_RECEIVED: {
            auto* info = reinterpret_cast<_IoRecvInfo*>(uparam);
            conn->parser_.feed(
                reinterpret_cast<const uint8_t*>(info->p), info->len);
            break;
        }
        case IO_TYPE_BEFORE_CLOSE:
            conn->server_->release_conn(conn);
            break;
        default:
            break;
        }
        return 0;
    }
};

// Connection pool + spinlock (0=free, 1=held).
free_list<ConnContext> conn_pool_;
ConnContext*           active_head_ = nullptr;
volatile int           conn_lock_   = 0;

// Listener-level callback: only handles IO_TYPE_ACCEPT_INIT.
static int on_listen_event(void* ctx, int type, uintptr uparam) noexcept {
    if (type != IO_TYPE_ACCEPT_INIT) return 0;

    auto* self = static_cast<Impl*>(ctx);
    ConnContext* conn = self->alloc_conn();
    if (!conn) return 0;   // pool exhausted, reject

    conn->id_     = uparam;
    conn->server_ = self;

    bool cancelled = false;
    self->pool_.GetFunc()->SetHandleFunc(
        self->pool_.GetContext(), uparam, conn->io_func_, conn, cancelled);

    if (cancelled) { self->release_conn(conn); return 0; }
    return 1;   // accept
}

// Allocate from pool.
ConnContext* alloc_conn() noexcept {
    ConnContext* conn = nullptr;
    while (atomic_compare_exchange((int&)conn_lock_, 0, 1)) ;
    {
        call_result cr = conn_pool_.FetchFreeNode(conn);
        if (cr.IsSucceeded()) {
            call_constructor(*conn);         // construct BEFORE PickFreeNode
            conn_pool_.PickFreeNode();
            conn->next_active_ = active_head_;
            active_head_       = conn;
        }
    }
    atomic_compare_exchange((int&)conn_lock_, 1, 0);
    return conn;
}

// Return to pool.
void release_conn(ConnContext* conn) noexcept {
    call_destructor(*conn);
    while (atomic_compare_exchange((int&)conn_lock_, 0, 1)) ;
    ConnContext** pp = &active_head_;
    while (*pp && *pp != conn) pp = &(*pp)->next_active_;
    if (*pp == conn) *pp = conn->next_active_;
    conn_pool_.PutFreeNode(conn);
    atomic_compare_exchange((int&)conn_lock_, 1, 0);
}
```

**为什么 `call_constructor` 必须在 `PickFreeNode` 之前：**  
`FetchFreeNode` 只是查看空闲链表的头节点，并不移除它。`PickFreeNode` 才是真正把头节点从链表中摘除。  
如果先 `PickFreeNode` 再构造，一旦构造函数中途失败，节点就永久丢失。  
先构造再 `PickFreeNode` 则保证：构造失败时节点仍留在链表头，不泄漏。

**`StartListen` 调用：**

```cpp
_IoFunc listenFunc{on_listen_event};
bool cancelled = false;
uintptr listenId = pool_.GetFunc()->StartListen(
    pool_.GetContext(), 8080, listenFunc, this, cancelled);
```

**适用场景（项目中）：**
- **Gateway**：`StartListen` 监听客户端连接、`StartConnect` 连接 AppHost，路由转发数据包
- **AppHost**：`StartConnect` 连接 Gateway、接收 MOUSE/KEYBOARD_EVENT、发送 PIXEL_DATA
- **Client**：`StartConnect` 连接 Gateway、接收 DIRTY_RECT、发送 MOUSE/KEYBOARD_EVENT

---

### 3.3 两者对比

| 特性 | WorkPool | IoPool |
|------|---------|--------|
| 用途 | CPU 密集任务 | 网络 I/O 事件 |
| 触发方式 | 主动 Submit | 事件驱动（epoll） |
| 池数量 | 快速池 + 慢速池 | 全局单实例 |
| 回调参数 | `void* pContext` | `iType + uParam` |
| 网络功能 | 无 | 监听、连接、收发 |

---

## 4. UIHost 接口体系

### 4.1 几何类型

```cpp
// 定义于 GKC/public/include/base/system/ui_types.h

class ui_point {
    ui_point(int X, int Y) noexcept;
    int X() const noexcept;
    int Y() const noexcept;
    void Offset(int xOffset, int yOffset) noexcept;
};

class ui_size {
    ui_size(int w, int h) noexcept;
    int W() const noexcept;
    int H() const noexcept;
};

class ui_rect {
    ui_rect(int l, int t, int r, int b) noexcept;
    int L(), T(), R(), B() const noexcept;
    int Width() const noexcept;
    int Height() const noexcept;
    bool IsPointIn(const ui_point& pt) const noexcept;
};

// 别名
typedef ui_size  UiSize;
typedef ui_point UiPoint;
typedef ui_rect  UiRect;
```

---

### 4.2 IUiHost 主接口

```cpp
// 定义于 GKC/public/include/base/GkcGui.h
struct IUiHost {
    // 进入主事件循环（阻塞）
    int  (*Loop)(void* pContext) noexcept;

    // 退出事件循环
    void (*Quit)(void* pContext) noexcept;

    // 向 UI 线程投递任务回调（可从任意线程调用，线程安全）
    void (*PostWork)(void* pContext, const WorkProc& work, void* pData) noexcept;

    // 注册一次性计时器（iPeriod 单位：毫秒），返回计时器 ID
    uintptr (*AddTimer)(void* pContext, int iPeriod,
                        const WorkProc& work, void* pData) noexcept;

    // 取消计时器
    void (*RemoveTimer)(void* pContext, uintptr id) noexcept;

    // 创建顶级窗口
    uintptr (*CreateToplevel)(void* pContext, bool bResizable,
                              int iWidth, int iHeight,
                              IUiToplevel** ppInterface) noexcept;

    // 创建对话框
    uintptr (*CreateDialogBox)(void* pContext, uintptr idOwner,
                               bool bResizable, int iWidth, int iHeight,
                               IUiDialog** ppInterface) noexcept;

    // 创建弹出窗口
    uintptr (*CreatePopup)(void* pContext, uintptr idOwner,
                           int iX, int iY, int iWidth, int iHeight,
                           IUiPopup** ppInterface) noexcept;

    // 销毁窗口
    void (*Destroy)(void* pContext, uintptr idWindow) noexcept;
};

// 全局 UIHost 实例（由 uihost 进程提供）
extern LcInterface<IUiHost> g_ui_host;
```

**PostWork / 计时器用法示例：**

```cpp
// 直接经 IUiHost 向 UI 线程提交任务
GKC::WorkProc proc{ [](void* p) noexcept { /* 在 UI 线程执行 */ } };
GKC::g_ui_host.GetFunc()->PostWork(
    GKC::g_ui_host.GetContext(), proc, pMyData);

// 也可以在窗口/控制器对象内继承 WorkImpl<T> / TimerImpl<T>
class MyWindow
    : public GKC::ToplevelImpl<MyWindow>
    , public GKC::WorkImpl<MyWindow>
    , public GKC::TimerImpl<MyWindow> {
public:
    void KickUiWork() noexcept { PostWork(); }
    uintptr StartHeartbeat() noexcept { return AddTimer(500); }
    void StopHeartbeat(uintptr id) noexcept { RemoveTimer(id); }
};

// GuiHelper 只提供 Loop / Quit 两个最薄的宿主封装
```

**光标类型（完整枚举）：**

```cpp
enum {
    CURSOR_NONE  = 0,   // 隐藏光标
    CURSOR_ARROW,       // 普通箭头
    CURSOR_CROSS,       // 十字准星
    CURSOR_WAIT,        // 等待（沙漏）
    CURSOR_INSERT,      // 文本输入
    CURSOR_HAND,        // 手形（超链接）
    CURSOR_HELP,        // 帮助
    CURSOR_MOVE,        // 移动/平移

    // 双向调整大小
    CURSOR_NS,          // ↕ 垂直调整
    CURSOR_WE,          // ↔ 水平调整
    CURSOR_NWSE,        // ↖↘ 对角线
    CURSOR_NESW,        // ↗↙ 对角线

    // 单边调整大小（用于精确命中测试）
    CURSOR_N,           // 上边
    CURSOR_NE,          // 右上角
    CURSOR_E,           // 右边
    CURSOR_SE,          // 右下角
    CURSOR_S,           // 下边
    CURSOR_SW,          // 左下角
    CURSOR_W,           // 左边
    CURSOR_NW,          // 左上角

    CURSOR_MAX
};
```

> **注意**：`CURSOR_N`～`CURSOR_NW` 为新增，供窗口边缘拖拽调整大小时使用。
> GKC 内部会根据 `MOUSE_EVENT_QUERY_CURSOR` 事件的返回值自动更新光标形状，
> 通常无需业务代码手动调用 `SetCursor`。

---

### 4.3 IUiWindow 及其子类

```cpp
struct IUiWindow {
    // 获取窗口尺寸
    void (*GetSize)(uintptr idWindow, ui_size& size) noexcept;

    // 显示 / 隐藏
    void (*Show)(uintptr idWindow, bool bShow) noexcept;

    // 注册消息处理器（绘制、关闭、鼠标等事件入口）
    void (*SetMessageHandler)(uintptr idWindow,
                              const ui_message_handler& handler,
                              void* pContext) noexcept;

    // 设置退出标志（线程安全，通知主循环该窗口需要关闭）
    void (*SetClose)(uintptr idWindow) noexcept;
};

// Toplevel 和 Dialog 额外支持程序控制大小
struct IUiToplevel {
    IUiWindow base;

    // 设置允许的最小 / 最大尺寸（对应 Wayland xdg_toplevel_set_min/max_size）
    void (*SetMinMax)(uintptr idWindow,
                      const ui_size& sizeMin, const ui_size& sizeMax) noexcept;

    // 程序主动改变窗口尺寸
    void (*SetSize)(uintptr idWindow, const ui_size& size) noexcept;
};

struct IUiDialog {
    IUiWindow base;
    void (*SetMinMax)(uintptr idWindow,
                      const ui_size& sizeMin, const ui_size& sizeMax) noexcept;
    void (*SetSize)(uintptr idWindow, const ui_size& size) noexcept;
};

struct IUiPopup { IUiWindow base; };
```

---

### 4.4 C++ 便捷包装类

```cpp
// 基类（GkcGui.h）
class Window {
    bool    IsValid() const noexcept;
    uintptr GetHandle() const noexcept;
    void    Destroy() noexcept;
    void    GetSize(UiSize& size) noexcept;
    void    Show(bool bShow) noexcept;
    void    SetClose() noexcept;  // 设置退出标志，通知主循环关闭此窗口
};

class Toplevel : public Window {
    bool Create(bool bResizable, int iWidth, int iHeight) noexcept;
    void SetMinMax(const UiSize& sizeMin, const UiSize& sizeMax) noexcept;
    void SetSize(const UiSize& size) noexcept;
};

class Dialog : public Window {
    bool Create(const Toplevel& owner, bool bResizable,
                int iWidth, int iHeight) noexcept;
    bool Create(const Dialog& owner, bool bResizable,
                int iWidth, int iHeight) noexcept;
    void SetMinMax(const UiSize& sizeMin, const UiSize& sizeMax) noexcept;
    void SetSize(const UiSize& size) noexcept;
};

class Popup : public Window {
    bool Create(const Window& owner, int iX, int iY,
                int iWidth, int iHeight) noexcept;
};

// 消息处理模板（业务代码继承此类）
template <class T, class TBase>
class WindowImpl : public TBase {
public:
    // 子类可重写以下虚函数
    void DoDraw(UiMessageDraw* pDraw) noexcept;         // 绘制
    void DoMouse(UiMessageMouse* pMouse) noexcept;      // 鼠标事件
    void DoKeyboard(UiMessageKeyboard* pKb) noexcept;   // 键盘按键事件
    void DoText(UiMessageText* pText) noexcept;         // 文本输入事件
    void DoClose() noexcept;                            // 窗口关闭
};

// 跨线程任务投递模板（业务代码继承此类以获得 PostWork 能力）
template <class T>
class WorkImpl {
public:
    // 向 UI 线程投递任务（可从任意线程调用）
    // 回调在 UI 线程上执行，T* 指针作为 pData 传入
    void PostWork() noexcept;
};

// 计时器模板（业务代码继承此类以获得定时回调能力）
template <class T>
class TimerImpl {
public:
    // 注册计时器，iPeriod 单位毫秒，返回计时器 ID
    uintptr AddTimer(int iPeriod) noexcept;
    // 取消计时器
    void    RemoveTimer(uintptr id) noexcept;
};

// 全局辅助
class GuiHelper {
    static int     Loop() noexcept;
    static void    Quit() noexcept;
    static void    SetCursor(int iType) noexcept;
    static void    PostWork(const WorkProc& work, void* pData) noexcept;
    static uintptr AddTimer(int iPeriod, const WorkProc& work, void* pData) noexcept;
    static void    RemoveTimer(uintptr id) noexcept;
};
```

---

## 5. 事件与消息系统

### 5.1 消息类型

```cpp
#define UI_MESSAGE_CLOSE     (10)   // 窗口关闭请求
#define UI_MESSAGE_DRAW      (11)   // 绘制请求
#define UI_MESSAGE_MOUSE     (12)   // 鼠标事件
#define UI_MESSAGE_KEYBOARD  (13)   // 键盘按键事件
#define UI_MESSAGE_TEXT      (14)   // 文本输入事件（IME / 合成文本）
```

GKC 现在暴露了 CLOSE、DRAW、MOUSE、KEYBOARD、TEXT 五种消息。

---

### 5.2 绘制消息（Draw）

```cpp
struct ui_message_draw {
    color_quad* pBuffer;   // 像素缓冲区（ARGB，每像素 32 位）
    int iWidth, iHeight;   // 缓冲区总尺寸
    ui_rect rcPaint;       // 本次需要重绘的脏矩形区域
};

typedef uint color_quad;

// 像素操作宏
COLOR_QUAD_MAKE(r, g, b, a)   // 构造一个像素值
COLOR_QUAD_GET_R(x)           // 提取 R 分量
COLOR_QUAD_GET_G(x)           // 提取 G 分量
COLOR_QUAD_GET_B(x)           // 提取 B 分量
COLOR_QUAD_GET_A(x)           // 提取 A 分量
```

**在 AppHost 中的意义：**
`pBuffer` 就是整窗口像素缓冲区，`rcPaint` 即 GKC 自维护的本次脏矩形区域。
AppHost 的 `ui_host_impl` 直接用 `rcPaint` 的坐标从 `pBuffer` 裁出像素，
打包为 `PIXEL_DATA` 协议包发给 Gateway，**无需逐帧像素比对**。

---

### 5.3 鼠标消息（Mouse）

GKC 现在通过 `UI_MESSAGE_MOUSE` 将鼠标事件直接推送给窗口消息处理器。

**消息结构体：**

```cpp
struct ui_message_mouse {
    uint  uEvent;    // 事件类型，见 MOUSE_EVENT_* 枚举
    byte  btButton;  // 触发按键，见 MOUSE_BUTTON_* 枚举
    byte  btState;   // 当前所有按键按下状态位掩码，见 MOUSE_STATE_* 枚举
    byte  btDouble;  // 1 = 双击，0 = 单击
    byte  btValue;   // 用于 QUERY_CURSOR：返回光标类型；用于 QUERY_MOVE：返回 1 允许移动
    int   x, y;      // 事件坐标（相对窗口客户区）
};

typedef ui_message_mouse UiMessageMouse;
```

**事件类型（uEvent）：**

```cpp
enum {
    MOUSE_EVENT_MOVE         = 0,  // 鼠标移动
    MOUSE_EVENT_DOWN         = 1,  // 按键按下
    MOUSE_EVENT_UP           = 2,  // 按键释放
    MOUSE_EVENT_WHEEL        = 3,  // 滚轮滚动（btButton 区分水平/垂直，x/y 为滚动量）
    MOUSE_EVENT_QUERY_MOVE   = 4,  // 询问是否允许拖拽移动窗口（返回 btValue=1 表示允许）
    MOUSE_EVENT_QUERY_CURSOR = 5,  // 询问当前坐标应显示何种光标（返回 btValue=CURSOR_*）
};
```

**按键标识（btButton）：**

```cpp
enum {
    MOUSE_BUTTON_NONE   = 0,
    MOUSE_BUTTON_LEFT   = 1,
    MOUSE_BUTTON_RIGHT  = 2,
    MOUSE_BUTTON_MIDDLE = 3,
};
```

**按键状态位掩码（btState）：**

```cpp
enum {
    MOUSE_STATE_LEFT   = 0x01,
    MOUSE_STATE_RIGHT  = 0x02,
    MOUSE_STATE_MIDDLE = 0x04,
};
```

**窗口边框大小（用于命中测试）：**

```cpp
enum {
    WINDOW_BORDER_SIZE = 5,  // 5 像素边缘区域触发拖拽调整大小
};
```

**拖拽移动与边缘调整大小的工作原理：**

GKC 内部通过 `MOUSE_EVENT_QUERY_MOVE` 和 `MOUSE_EVENT_QUERY_CURSOR` 实现窗口拖拽和边缘调整大小：

- **调整大小**：当鼠标进入窗口 5px 边缘区域时，GKC 发出 `MOUSE_EVENT_QUERY_CURSOR` 事件，
  业务代码将 `btValue` 设为对应方向的 `CURSOR_*` 值，GKC 即知道该边缘可调整大小，
  随后在鼠标按下时自动发起 `xdg_toplevel_resize`（Linux）或返回 `HT*` 命中值（Windows）。
- **移动窗口**：GKC 发出 `MOUSE_EVENT_QUERY_MOVE` 事件，业务代码将 `btValue` 设为 1，
  GKC 在鼠标按下时自动发起 `xdg_toplevel_move`（Linux）或返回 `HTCAPTION`（Windows）。

> 对于**有标准标题栏**的 Toplevel 窗口，GKC 已内置处理，通常无需业务代码干预。
> 只有**无边框自绘窗口**才需要在 `DoMouse` 中手动响应这两个查询事件。

---

### 5.4 键盘消息（Keyboard）

GKC 通过 `UI_MESSAGE_KEYBOARD` 推送键盘按键事件。

**消息结构体：**

```cpp
struct ui_message_keyboard {
    byte   btDown;   // 1 = 按下，0 = 释放
    byte   btKey;    // 键码，见 KB_* 枚举（虚拟键码）
    byte   btState;  // modifier 状态位掩码，见 KB_STATE_* 枚举
    byte   btValue;  // 置 1 表示已处理（阻止默认行为）
    char_f chFull;   // 对应的可打印字符（或 KB_CHAR_* 控制字符）
};

typedef ui_message_keyboard UiMessageKeyboard;
```

**键码枚举（btKey）：**

```cpp
enum {
    KB_Clear       = 0x0C,  // 小键盘 5（Num Lock 关闭时）
    KB_Pause       = 0x13,
    KB_Page_Up     = 0x21,
    KB_Page_Down   = 0x22,
    KB_End         = 0x23,
    KB_Home        = 0x24,
    KB_Left        = 0x25,
    KB_Up          = 0x26,
    KB_Right       = 0x27,
    KB_Down        = 0x28,
    KB_Insert      = 0x2D,
    KB_Delete      = 0x2E,
    KB_Meta_L      = 0x5B,  // 左 Win/Meta 键
    KB_Meta_R      = 0x5C,  // 右 Win/Meta 键
    KB_Menu        = 0x5D,
    KB_F1  = 0x70, KB_F2  = 0x71, KB_F3  = 0x72, KB_F4  = 0x73,
    KB_F5  = 0x74, KB_F6  = 0x75, KB_F7  = 0x76, KB_F8  = 0x77,
    KB_F9  = 0x78, KB_F10 = 0x79, KB_F11 = 0x7A, KB_F12 = 0x7B,
    KB_Shift_L     = 0xA0,
    KB_Shift_R     = 0xA1,
    KB_Control_L   = 0xA2,
    KB_Control_R   = 0xA3,
    KB_Alt_L       = 0xA4,
    KB_Alt_R       = 0xA5,
};
```

> 普通可打印字符（A–Z、0–9 等）直接以 ASCII 值作为 `btKey`。

**modifier 状态位掩码（btState）：**

```cpp
enum {
    KB_STATE_SHIFT       = 0x01,  // Shift 键按下
    KB_STATE_CAPS_LOCK   = 0x02,  // Caps Lock 开启
    KB_STATE_CTRL        = 0x04,  // Ctrl 键按下
    KB_STATE_ALT         = 0x08,  // Alt 键按下
    KB_STATE_NUM_LOCK    = 0x10,  // Num Lock 开启
    KB_STATE_META        = 0x20,  // Meta/Win 键按下
    KB_STATE_SCROLL_LOCK = 0x40,  // Scroll Lock 开启
};
```

**控制字符常量（chFull）：**

```cpp
enum {
    KB_CHAR_BACKSPACE = 0x08,
    KB_CHAR_TAB       = 0x09,
    KB_CHAR_LF        = 0x0A,  // Ctrl+Enter
    KB_CHAR_CR        = 0x0D,  // Enter
    KB_CHAR_ESC       = 0x1B,
};
```

---

### 5.5 文本输入消息（Text）

`UI_MESSAGE_TEXT` 用于 IME 合成文本或直接文字输入（非按键级别）。

```cpp
struct ui_message_text {
    const char_s* szText;    // UTF-8 / UTF-16 文本内容
    uintptr       uLength;   // 文本长度；若 == INVALID_ARRAY_INDEX，则查询输入法候选框位置
    int x, y, w, h;          // 输入法候选框建议位置（uLength 为 INVALID_ARRAY_INDEX 时使用）
};

typedef ui_message_text UiMessageText;
```

**典型用途（AppHost 侧）：**

在远程渲染架构中，AppHost 不处理本地键盘，而是将来自 Client 的 `KEYBOARD_EVENT` 协议包
映射为 `UI_MESSAGE_KEYBOARD` / `UI_MESSAGE_TEXT` 投递给业务 `.so`：

```
Client 键盘输入
  → KEYBOARD_EVENT 协议包 → Gateway → AppHost
  → AppHost 调用 ui_host_impl 模拟分发
  → UI_MESSAGE_KEYBOARD → 业务 .so 的 DoKeyboard()
  → UI_MESSAGE_TEXT     → 业务 .so 的 DoText()（IME 合成文本时）
```

---

### 5.6 注册消息处理器

```cpp
ui_message_handler handler{
    [](void* pContext, uint uMessage, uintptr uParam) noexcept {
        switch (uMessage) {
        case UI_MESSAGE_DRAW: {
            auto* pDraw = reinterpret_cast<ui_message_draw*>(uParam);
            // 操作 pDraw->pBuffer 进行绘制
            break;
        }
        case UI_MESSAGE_MOUSE: {
            auto* pMouse = reinterpret_cast<ui_message_mouse*>(uParam);
            // 处理鼠标事件
            break;
        }
        case UI_MESSAGE_KEYBOARD: {
            auto* pKb = reinterpret_cast<ui_message_keyboard*>(uParam);
            // pKb->btDown: 1=按下 0=释放
            // pKb->btKey:  KB_* 键码
            // pKb->btState: KB_STATE_* modifier 位掩码
            // pKb->chFull: 对应字符
            break;
        }
        case UI_MESSAGE_TEXT: {
            auto* pText = reinterpret_cast<ui_message_text*>(uParam);
            // pText->szText / pText->uLength: IME 合成文本
            break;
        }
        case UI_MESSAGE_CLOSE:
            GKC::GuiHelper::Quit();
            break;
        }
    }
};

pWindow->base.SetMessageHandler(idWindow, handler, pContext);
```

---

### 5.7 自定义事件回调

继承 `WindowImpl` 并重写对应方法：

```cpp
class MyWindow : public GKC::WindowImpl<MyWindow, GKC::Toplevel>
{
public:
    // 绘制回调：只处理脏矩形区域，避免全帧重绘
    void DoDraw(GKC::UiMessageDraw* pDraw) noexcept override
    {
        color_quad* pBuf = pDraw->pBuffer;
        int stride = pDraw->iWidth;
        const ui_rect& rc = pDraw->rcPaint;

        for (int y = rc.T(); y < rc.B(); y++) {
            for (int x = rc.L(); x < rc.R(); x++) {
                pBuf[y * stride + x] = COLOR_QUAD_MAKE(255, 0, 0, 255);
            }
        }
    }

    // 鼠标事件回调（新增）
    void DoMouse(GKC::UiMessageMouse* pMouse) noexcept override
    {
        switch (pMouse->uEvent) {
        case MOUSE_EVENT_MOVE:
            // 鼠标移动，坐标：pMouse->x, pMouse->y
            break;
        case MOUSE_EVENT_DOWN:
            if (pMouse->btButton == MOUSE_BUTTON_LEFT) {
                // 左键按下
            }
            break;
        case MOUSE_EVENT_UP:
            // 按键释放
            break;
        case MOUSE_EVENT_WHEEL:
            // 滚轮，滚动量在 x（水平）或 y（垂直）中
            break;
        case MOUSE_EVENT_QUERY_CURSOR:
            // 根据坐标设置光标（用于无边框窗口边缘调整大小）
            // pMouse->btValue = CURSOR_NS;  // 示例：上下调整
            break;
        case MOUSE_EVENT_QUERY_MOVE:
            // 若坐标在标题区返回 1，允许拖拽移动
            // pMouse->btValue = 1;
            break;
        }
    }

    // 关闭回调
    void DoClose() noexcept override
    {
        GKC::GuiHelper::Quit();
    }
};
```

**关于键盘事件（项目架构说明）：**

GKC 现在原生支持 `UI_MESSAGE_KEYBOARD` 和 `UI_MESSAGE_TEXT`。
但在本项目的**远程渲染架构**中，AppHost 运行在无键盘外设的服务端，
实际键盘输入来自远端 Client 通过协议发送的 `KEYBOARD_EVENT` 包。
因此 AppHost 中的 `ui_host_impl` 需要将收到的协议键盘事件
**模拟为** `UI_MESSAGE_KEYBOARD` / `UI_MESSAGE_TEXT` 分发给业务 `.so`，
而不是依赖 GKC 的本地键盘输入。

协议 `KEYBOARD_EVENT` 包中的键码字段直接使用 GKC 定义的 `KB_*` 枚举值——
Client 侧 `DoKeyboard()` 回调的 `pKb->btKey` 已经是 `KB_*`，无需任何映射，直接填入协议包即可。

**关于鼠标事件（架构说明）：**

GKC 现在通过 `UI_MESSAGE_MOUSE` 将本地鼠标事件传递给窗口。
但在本项目的**远程渲染架构**中，AppHost 运行在服务端，
实际鼠标输入来自远端 Client 通过协议发送的 `MOUSE_EVENT` 包。
因此 AppHost 中的 `ui_host_impl` 需要把收到的协议鼠标事件
**模拟为** `UI_MESSAGE_MOUSE` 消息分发给业务 `.so` 插件，
而不是依赖 GKC 的本地鼠标输入。

---

---

## 6. 窗口类型与绘制机制深解

### 6.1 三种窗口类型（Wayland Surface 角色）

GKC 在 Linux 上基于 **Wayland** 实现 UI（底层用 `wl_surface`、`xdg_wm_base`、`wl_shm`），
对外暴露了三种窗口类型，对应 Wayland 的三种 surface 角色：

| GKC 类 | Wayland 角色 | 说明 |
|--------|-------------|------|
| `Toplevel` | `xdg_toplevel` | 独立顶层窗口，有标题栏，可移动/缩放 |
| `Dialog` | `xdg_toplevel`（带 owner） | 对话框，从属于某个 Toplevel，会跟随宿主 |
| `Popup` | `xdg_popup` | 弹出层，挂靠在 owner 的指定坐标位置（菜单、提示框等） |

**创建方式的区别：**

```cpp
// Toplevel：无 owner
GKC::Toplevel win;
win.Create(/*bResizable=*/true, 800, 600);

// Dialog：需要指定 owner（Toplevel 或另一个 Dialog）
GKC::Dialog dlg;
dlg.Create(win, /*bResizable=*/false, 400, 300);

// Popup：需要指定 owner 以及屏幕坐标
GKC::Popup pop;
pop.Create(win, /*iX=*/100, /*iY=*/200, 200, 150);
```

三种窗口的**绘制机制完全相同**，均通过 `UI_MESSAGE_DRAW` 回调写像素。
区别仅在于窗口管理器（Wayland compositor）对它们的布局、焦点、层级处理。

---

### 6.2 像素缓冲区与绘制流水线

GKC 的底层绘制流水线（源码位于
`third_party/GKC/util/gui/uihost/include/base/_system_/Linux/_core_/`）：

```
1. GKC 通过 wl_shm 分配共享内存（surface_data::Create）
   → 得到一块 iWidth × iHeight × 4 字节的像素缓冲区

2. 事件循环检测到 damage 区域（rcDamage 非空时）
   → 构造 ui_message_draw：
       umd.pBuffer  = (color_quad*) 共享内存地址
       umd.iWidth   = 窗口宽度
       umd.iHeight  = 窗口高度
       umd.rcPaint  = 本次需重绘的脏矩形区域

3. 调用 handler.Process(pContext, UI_MESSAGE_DRAW, &umd)
   → 业务代码在 DoDraw() 里写像素

4. GKC 调用 Wayland API 提交：
       wl_surface_attach(surface, buffer, 0, 0)
       wl_surface_damage_buffer(surface, rc.L, rc.T, rc.W, rc.H)
       wl_surface_commit(surface)
```

**像素格式（color_quad）：**

```cpp
typedef uint color_quad;  // 32-bit ARGB，每分量 8 bit

// 构造像素
color_quad px = COLOR_QUAD_MAKE(r, g, b, a);  // r,g,b,a: 0-255

// 读取分量
uint8_t r = COLOR_QUAD_GET_R(px);
uint8_t g = COLOR_QUAD_GET_G(px);
uint8_t b = COLOR_QUAD_GET_B(px);
uint8_t a = COLOR_QUAD_GET_A(px);
```

**像素寻址（行优先，top-down 存储）：**

```cpp
// 第 (x, y) 个像素的地址（y=0 在顶部）
color_quad* pixel = pBuffer + y * iWidth + x;
```

> **注意**：`pBuffer` 中像素按**从上到下**（top-down）顺序存储，
> 与 Windows DIB 默认的 bottom-up 相反。

---

### 6.3 GKC 不提供任何控件

GKC 的 UI 层只提供：
- **空白像素画布**（`pBuffer`）
- **五种事件**：`UI_MESSAGE_DRAW`（重绘）、`UI_MESSAGE_CLOSE`（关闭）、`UI_MESSAGE_MOUSE`（鼠标）、`UI_MESSAGE_KEYBOARD`（键盘）、`UI_MESSAGE_TEXT`（文本输入）

**没有**按钮、文本框、列表、布局管理等控件。

如果业务 `.so` 插件需要复杂 UI，有两个选择：

| 方案 | 说明 |
|------|------|
| 自行绘制 | 在 `DoDraw()` 里用 `pBuffer` 手动绘制矩形、文字等图形 |
| 集成第三方 2D 库 | 如 [nanovg](https://github.com/memononen/nanovg)、[blend2d](https://blend2d.com/) 等只需要一块像素缓冲区的矢量绘图库 |

---

### 6.4 与 AppHost 像素传输的关系

本项目的 **AppHost** 不会真正将像素提交给 Wayland 合成器，
而是通过实现一个"假的" `IUiHost`（即 `ui_host_impl`）拦截 `DoDraw()` 的输出。
脏矩形由 GKC 的 `DoDraw` 回调**天然携带**，无需逐帧像素比对：

```
业务 .so 的 DoDraw(pDraw)
    ↓ 写像素到 pDraw->pBuffer
AppHost 的 ui_host_impl 捕获 pDraw->rcPaint（GKC 自维护的脏矩形）
    ↓
pixel_capture(pDraw->pBuffer, pDraw->iWidth, pDraw->rcPaint)
    裁出 rcPaint 区域的 ARGB 字节块
    ↓
WorkPool 快速池：打包 PIXEL_DATA → IoPool → Gateway → Client
```

因此，`.so` 插件本身的代码**无需感知网络**，与本地运行的写法完全一致，
只是 `pBuffer` 的"消费者"从 Wayland 合成器换成了网络传输层。

---

## 7. .so 插件开发规范

### 7.1 必须导出的符号

```cpp
// 应用入口结构
struct SA_UIMain {
    int (*Exec)(const GKC::LcInterface<GKC::IUiHost>& lcHost,
                const GKC::ConstArray<GKC::ConstStringS>& args) noexcept;
};

// 必须以 C 链接方式导出此函数
extern "C" GKC::SA_UIMain* sa_ui_main()
{
    static GKC::SA_UIMain g_main{
        [](const GKC::LcInterface<GKC::IUiHost>& lcHost,
           const GKC::ConstArray<GKC::ConstStringS>& args) noexcept -> int
        {
            // 应用入口逻辑
            return 0;
        }
    };
    return &g_main;
}
```

### 7.2 AppHost 加载流程

```cpp
// plugin_loader.cpp 中的加载逻辑
void* hLib = dlopen("./myapp.so", RTLD_LAZY | RTLD_LOCAL);

typedef GKC::SA_UIMain* (*FactoryFn)();
FactoryFn factory = (FactoryFn)dlsym(hLib, "sa_ui_main");

GKC::SA_UIMain* pMain = factory();
int ret = pMain->Exec(g_ui_host, args);  // g_ui_host 由 AppHost 实现提供

dlclose(hLib);
```

**编译 .so 插件：**

```bash
g++ -std=c++17 -fPIC -shared -o myapp.so myapp.cpp \
    -I third_party/GKC/public/include \
    -L bazel-bin/third_party -lGkcSys
```

---

## 8. 日志系统

```cpp
// 初始化（指定日志文件路径）
LOGGER_INITIALIZE("/var/log/myapp/app.log");

// 输出日志（自动附带文件名和行号）
LOGGER_OUTPUT("Gateway started");
LOGGER_OUTPUT("Session created");
```

**日志格式：**
```
[source_file.cpp][2026-04-12 16:05:30][123]Gateway started
```

**特性：**
- 线程安全（内部 Mutex 保护）
- 单文件上限 2GB，自动轮转

---

## 9. 内置 Socket 封装

GKC 在 `RT/GkcSys` 内部提供了 Socket 工具类（供 IoPool 内部使用）。
**本项目建议直接使用 IoPool 的事件驱动接口**，而不是直接操作底层 Socket。

若确实需要低层操作，Linux 侧工具类位于：

```cpp
// GKC/RT/GkcSys/include/base/system/Linux/_util_/_x_socket.h

class _x_socket_addr {
    bool Fetch(const char* node, const char* service) noexcept;
};

class _x_nb_socket {  // 非阻塞 socket
    bool Create(int domain, int type, int protocol = 0) noexcept;
    bool Disable(int how) noexcept;  // SHUT_RD / SHUT_WR / SHUT_RDWR
    bool GetSockName(struct sockaddr* addr, socklen_t* addrlen) const noexcept;
};

class _x_socket_helper {
    static bool GetIPName(const struct sockaddr* addr, socklen_t addrlen,
                          char* dest, socklen_t size) noexcept;
    static u_int16_t GetPort(const struct sockaddr* addr, socklen_t addrlen) noexcept;
    static bool FillSockAddr(struct sockaddr_storage& addr, socklen_t& addrlen,
                             int af, u_int16_t port, const char* ip = NULL) noexcept;
};
```

---

## 10. 内存管理工具

```cpp
// 定义于 GKC/public/include/base/GkcDef.h

// 独占指针
GKC::UniquePtr<T>
GKC::UniquePtrHelper::CreateNormal<T>(args...)

// 引用计数指针
GKC::RefPtr<T>

// 示例
auto pPool = GKC::UniquePtrHelper::CreateNormal<WorkPool>();
if (pPool.IsNull()) { /* 分配失败 */ }
```

---

## 11. 完整使用示例

以下示例展示了包含鼠标、键盘处理、计时器、跨线程任务投递的完整插件结构：

```cpp
#include "GkcDef.h"
#include "GkcGui.h"

// 业务窗口：继承 WindowImpl 处理绘制与鼠标事件
//           继承 TimerImpl 获得计时器能力
//           继承 WorkImpl 获得跨线程任务投递能力
class AppWindow
    : public GKC::WindowImpl<AppWindow, GKC::Toplevel>
    , public GKC::TimerImpl<AppWindow>
    , public GKC::WorkImpl<AppWindow>
{
public:
    bool Initialize(int w, int h)
    {
        if (!Create(/*bResizable=*/true, w, h))
            return false;

        // 设置最小 / 最大尺寸
        SetMinMax(GKC::UiSize(320, 240), GKC::UiSize(1920, 1080));

        // 启动 1000ms 周期计时器（演示：每秒触发一次）
        m_timerId = AddTimer(1000);

        return true;
    }

    // 绘制回调：只重绘脏矩形区域
    void DoDraw(GKC::UiMessageDraw* pDraw) noexcept override
    {
        color_quad* pBuf   = pDraw->pBuffer;
        int         stride = pDraw->iWidth;
        const auto& rc     = pDraw->rcPaint;

        // 根据是否有鼠标按下切换颜色（左键=红，无=蓝）
        color_quad col = m_bLeftDown
                         ? COLOR_QUAD_MAKE(255, 0, 0, 255)
                         : COLOR_QUAD_MAKE(0, 0, 255, 255);

        for (int y = rc.T(); y < rc.B(); ++y)
            for (int x = rc.L(); x < rc.R(); ++x)
                pBuf[y * stride + x] = col;
    }

    // 鼠标事件回调
    void DoMouse(GKC::UiMessageMouse* pMouse) noexcept override
    {
        switch (pMouse->uEvent) {
        case MOUSE_EVENT_DOWN:
            if (pMouse->btButton == MOUSE_BUTTON_LEFT) {
                m_bLeftDown = true;
                // 触发全窗口重绘（将整个窗口标记为 dirty）
                // ...
            }
            break;
        case MOUSE_EVENT_UP:
            if (pMouse->btButton == MOUSE_BUTTON_LEFT) {
                m_bLeftDown = false;
            }
            break;
        case MOUSE_EVENT_WHEEL:
            // pMouse->y：垂直滚动量（正=向上，负=向下）
            break;
        case MOUSE_EVENT_QUERY_MOVE:
            // 整个窗口均可拖拽移动（无边框自绘窗口示例）
            pMouse->btValue = 1;
            break;
        default:
            break;
        }
    }

    // 计时器回调（每 1000ms 触发一次）
    void OnTimer(uintptr id) noexcept
    {
        // 例：从非 UI 线程调度任务回到 UI 线程
        PostWork();
    }

    // WorkImpl 回调（在 UI 线程执行）
    void OnWork() noexcept
    {
        // 安全地操作 UI 状态
    }

    // 键盘事件回调
    void DoKeyboard(GKC::UiMessageKeyboard* pKb) noexcept override
    {
        if (!pKb->btDown)
            return;  // 只处理按下事件

        if (pKb->btKey == KB_Escape) {
            SetClose();
            return;
        }

        // Ctrl+C 示例
        if (pKb->chFull == 'c' && (pKb->btState & KB_STATE_CTRL)) {
            // 处理复制
        }

        pKb->btValue = 1;  // 标记为已处理
    }

    // 文本输入回调（IME 合成文本）
    void DoText(GKC::UiMessageText* pText) noexcept override
    {
        if (pText->uLength == INVALID_ARRAY_INDEX) {
            // 查询 IME 候选框位置：将 x,y,w,h 设为光标位置
            return;
        }
        // pText->szText 是输入的文本内容
    }

    void DoClose() noexcept override
    {
        RemoveTimer(m_timerId);
        GKC::GuiHelper::Quit();
    }

private:
    bool    m_bLeftDown = false;
    uintptr m_timerId   = 0;
};

// 插件入口（AppHost 通过 dlsym("sa_ui_main") 加载）
extern "C" GKC::SA_UIMain* sa_ui_main()
{
    static GKC::SA_UIMain g_main{
        [](const GKC::LcInterface<GKC::IUiHost>& lcHost,
           const GKC::ConstArray<GKC::ConstStringS>& args) noexcept -> int
        {
            auto pWin = GKC::UniquePtrHelper::CreateNormal<AppWindow>();
            if (pWin.IsNull() || !pWin->Initialize(800, 600))
                return 1;

            pWin->Show(true);
            return GKC::GuiHelper::Loop();
        }
    };
    return &g_main;
}
```
