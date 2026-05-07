# M6a — Windows 客户端手写消息泵的引入

## 一句话概要

M6a 联调阶段发现 Windows 端的 client plugin DLL 调用 GKC 提供的
`GuiHelper::Loop()`（即 `_w_message_loop_impl::run()`）后，主线程消息循环不再
派发任何消息，导致 PIXEL_DATA 应用后窗口无法重绘、跨线程 `PostWork` 无法回
到 UI 线程。我们用一个手写的 Win32 `GetMessageW` 消息泵替换了 GKC 的实现，
仅替换"消息泵"这一个最小职责，保留了 GKC 的窗口、绘制、跨线程 marshal、
Quit 等其他全部子系统的语义。

## 背景

按 M6a 的设计，客户端的运行模型是：

```
client.exe (wWinMain → UIMain)
  └── 加载 plugin DLL (libclient_viewer.dll)
        └── _SA_UIMain → GuiMain → ViewerWindow::Start
              ├── ClientRuntime::start()  // 起 std::thread 跑网络 I/O
              └── GuiHelper::Loop()        // UI 线程进入消息循环
```

UI 线程负责绘制和接受用户输入；网络线程从 gateway 接 PIXEL_DATA，应用到
`PixelRenderer` 的 backstore 后，需要让 UI 线程刷新窗口。跨线程通知靠 GKC
的 `WorkImpl::PostWork()` 完成 —— 它内部走 `PostThreadMessageW` 把
`UWM_USER_EVENT (= WM_USER + 100)` 投递到 UI 线程消息队列，由
`m_loop.Run()` 的主循环识别并执行 `work_proc`。

## 故障现象

集成 M6a 时观察到三个关联现象：

1. **窗口初始绘制成功，但 PIXEL_DATA 到来之后窗口不更新**。日志里
   `runtime: PIXEL_DATA len=… result=0` 表示 backstore 已被正确写入，但接
   下来没有任何 `viewer: DoDraw …` 日志。
2. **鼠标悬在窗口上几秒后变成"应用未响应"的转圈光标**，几秒后窗口被
   操作系统当作 Not Responding 而关闭，但进程本身仍在跑（仍然能看到
   `runtime: sent HEARTBEAT …` 持续输出）。
3. **GKC 提供的 `TimerImpl::AddTimer` 也完全不 fire** —— 注册一个 250 ms
   的诊断 timer，几秒内一次回调都没收到。

加诊断日志确认 `GuiHelper::Loop()` 已经被调到 (`viewer: entering
GuiHelper::Loop`) 但从未返回 (`viewer: GuiHelper::Loop returned rc=…`
始终未出现)，说明 `m_loop.Run()` 进入后既不派发消息、也不退出，处于
某种"运行但失能"的状态。

## 排查过程

按"二分定位 + 替换验证"的思路一步步缩小范围：

### 第一步：确认 backstore 数据正确

在 `PixelRenderer::apply_pixel_data_body` 后立刻打印 `result`，结果对所有帧
都是 `0 (OK)`。说明：
- 协议解析没问题（24 字节 body header 正确）
- 字节序转换没问题（`argb_to_color_quad` 与 apphost 端
  `pack_pixel_data_rect` 的 `[A,R,G,B]` 约定对齐）
- 帧序号校验没问题（`last_frame_seq_` 单调递增 OK）

### 第二步：确认跨线程通知链的前半段

在网络线程的 `repaint_callback_()` 调用前后埋日志：

```
runtime: invoking repaint_callback   ← 调到了
viewer: repaint_callback PostWork    ← 进入 lambda 调到了 PostWork
（之后再无任何 RepaintWork::DoWork 输出）
```

说明 `WorkImpl::PostWork()` 调到了，调用链一路进到 GKC 的
`PostThreadMessageW`，**但 UI 线程没接到**。

### 第三步：诊断 UI 线程是否还活着

在 `Start()` 末尾注册一个 250 ms 的 `TimerImpl` ticker，预期 UI 线程的
消息循环每次迭代都会调 `process_timers()` 把它 fire 出来：

```cpp
ui_tick_id_ = ui_tick_.AddTimer(250);
```

但日志里 `viewer: ui-tick N` 一次都没出现。同时也没有
`viewer: GuiHelper::Loop returned rc=…`，证明：

- `m_loop.Run()` 没退出（不是 `WAIT_FAILED` 或 `WM_QUIT`）
- 也没在 pump 任何消息（连自己内部 timer 都不 fire）

### 第四步：在 offline 模式下复现

去掉网络线程，只跑窗口 + 空消息泵：

```powershell
client.exe --plugin=...client_viewer.dll --offline
```

现象一致 —— 同样进 `Loop()` 后不再 pump。**确认问题与网络线程无关，与
GKC `m_loop.Run()` 本身有关**。

### 第五步：手写 Win32 消息泵替换

为了把 GKC 自家的实现摘出去做对照，我们在 plugin 的 `GuiMain` 里直接调
`PeekMessageW` / `DispatchMessageW`：

```cpp
MSG msg{};
while (true) {
    if (::PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) break;
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    } else {
        ::Sleep(10);
    }
}
```

**这个泵立刻就工作了**。日志里同时出现：

- `viewer: msg=15 hwnd=…`（msg = `0x000F` = `WM_PAINT`）跟着
  `viewer: DoDraw dst=320x160 paint=0,0,320,160`
- 鼠标移动产生 `WM_MOUSEMOVE`（msg = `0x0200`）派发到 `DoMouse`
- 鼠标点击触发 `DamageFull` → 队列里出现新的 `WM_PAINT` → `DoDraw`

也就是说：**Win32 消息派发本身完全正常，是 GKC 的 `m_loop.Run()` 这个
具体实现在我们的 build 环境里没有 pump**。

## 根因猜测

`_w_message_loop_impl::run()` 的核心是：

```cpp
DWORD dwTimeout = process_timers();
DWORD dwRet = ::MsgWaitForMultipleObjects(0, NULL, FALSE, dwTimeout,
                                           QS_ALLINPUT);
if (dwRet == WAIT_TIMEOUT) continue;
if (dwRet == WAIT_FAILED) break;
while (::PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { … }
```

可能的诱因（本次没花时间彻底证伪，因为替换方案已经验证 OK 且改动范围足够小）：

1. **`MsgWaitForMultipleObjects(0, NULL, …)` 在 plugin DLL 加载场景下
   行为异常**。该 API 的语义在 `nCount=0` 时是"只等输入事件"，但部分
   Windows 版本/COM apartment 配置下，对 `lpHandles=NULL` 的处理不一致
   或被 STA 拦截。
2. **`m_dwMainThreadId` 在 EXE 静态初始化时记录**，但客户端 EXE 同时
   走了 `_os_initialize_com_STA()`/`_os_initialize_com_security()`，COM
   初始化可能改变了线程的某些消息派发属性，导致后续 `PostThreadMessageW`
   投到一个 GKC 拿不到的子系统队列。
3. **GKC 的 `ProgramEntryPoint::GuiMain` 是从 plugin DLL 内调出的
   `m_loop.Run()`**，而 LocalDesk 那种"原生 EXE + GKC 静态链入"的形态
   没有这一层 DLL 边界 —— 我们这个差异可能触发了 GKC 内部假设。

由于 LocalDesk 用同一份 GKC 代码在它自己的 `cmake` 二进制里能跑，且我们
的修复方案在替换面足够小（仅消息泵），**没有继续往 GKC 内部追，把工程
精力留给了 M6a 的下游工作（2048 应用层）**。

## 解决方案

只替换"消息泵"这一个最小职责，其他 GKC 子系统全部保留：

### 改动 1：手写消息泵

`plugins/client_viewer/client_viewer.cpp` 的 `GuiMain` 用 `GetMessageW`
直接 pump，并**显式处理 `UWM_USER_EVENT` 线程消息**（因为 `DispatchMessage`
会丢弃 `hwnd == NULL` 的线程消息，这是 GKC `PostWork` 的载体）：

```cpp
constexpr UINT kUserEvent = WM_USER + 100;  // 与 GKC _w_message_loop_impl 一致

MSG msg{};
for (;;) {
    BOOL got = ::GetMessageW(&msg, NULL, 0, 0);
    if (got == 0) break;          // WM_QUIT
    if (got == -1) break;         // GetMessage 内部错误
    if (msg.hwnd == NULL && msg.message == kUserEvent) {
        // 跨线程 PostWork 的载体，手动调 work_proc
        using ExecFn = void (*)(void*) noexcept;
        ExecFn exec = reinterpret_cast<ExecFn>(msg.wParam);
        if (exec != nullptr) exec(reinterpret_cast<void*>(msg.lParam));
        continue;
    }
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
}
```

`GetMessageW` 比 `PeekMessage` + `Sleep` 更优 —— 它会阻塞到有消息为止，
不浪费 CPU，对应行为完全等价于 GKC 原本想做的事。

### 改动 2：用 `PostQuitMessage` 替代 `GuiHelper::Quit()`

GKC 的 `Quit()` 走的是它自己的 `m_lExit` 标志位 + `UWM_USER_EVENT`
路径，依赖 `m_loop.Run()` 内部检查 —— 我们绕过了那层，只能用 Win32
原生方式让 `GetMessageW` 返回 0：

```cpp
void DoClose() noexcept {
    runtime_.stop();
    ::PostQuitMessage(0);  // GetMessageW 收到 WM_QUIT 返回 0，泵退出
}
```

### 改动 3：BUILD 文件加 `user32.lib`

Plugin DLL 之前没显式链 `user32.lib`，因为它通过 GKC 的 IUiHost 函数
表间接调 Win32。现在我们在 plugin 里直接调 `GetMessageW` 等，需要加：

```python
linkopts = select({
    "@platforms//os:windows": [
        "ws2_32.lib",
        "user32.lib",
    ],
    ...
})
```

## 影响评估

| 子系统 | 在替换后是否仍可用 | 说明 |
|--------|--------------------|------|
| 窗口创建（`Create`）/销毁（`Destroy`） | ✅ | 走 `IUiHost` 函数表，未触动 |
| 绘制派发（`DoDraw`） | ✅ | `WM_PAINT` 由 `DispatchMessageW` 路由到 GKC 注册的 WindowProc，再到 `Process` → `DoDraw`，链路与 GKC 原生一致 |
| 鼠标 / 键盘（`DoMouse` / `DoKeyboard`） | ✅ | 同 `WM_PAINT` |
| 失效区（`Damage`） | ✅ | 直接调 `IUiWindow::Damage` → `InvalidateRect` |
| 跨线程 `WorkImpl::PostWork` | ✅ | 手写泵显式 dispatch `UWM_USER_EVENT (WM_USER + 100)` |
| `GuiHelper::Quit()` | ⚠️ | 不再生效；用 `PostQuitMessage(0)` 等价替代 |
| `TimerImpl::AddTimer` | ❌ | GKC 内部 timer 走 `process_timers()`，依赖 `m_loop.Run()` 的迭代；当前未使用，将来如需周期回调可改用 Win32 `SetTimer` |
| `IoPool` (gateway / apphost 用的 epoll/IOCP) | ✅ | 完全独立的子系统，与 `m_loop` 不相关 |

唯一被实际放弃的能力是 GKC 的 `TimerImpl`，且当前代码库未使用 ——
客户端的心跳是在 `ClientRuntime::run()` 里用 `std::chrono::steady_clock`
自己计时的，不依赖 GKC timer（参见 `src/client/client.cpp:169-179`）。

## 验证

集成测试 (gateway 117.72.215.251:19000 + demo_app)：

```
viewer: window created
viewer: DoDraw dst=320x160 paint=0,0,320,160         ← 初始绘制
viewer: window shown
viewer: DoDraw dst=320x160 paint=0,0,320,160
viewer: runtime started
viewer: entering message pump
runtime: connected
runtime: SESSION_ACK session_id=1
runtime: PIXEL_DATA len=2072 result=0                ← 全帧
runtime: invoking repaint_callback
runtime: PIXEL_DATA len=280 result=0                 ← 8x8 dirty
runtime: invoking repaint_callback
viewer: DoDraw dst=320x160 paint=0,0,320,160         ← UI 线程被唤醒，应用新 backstore
viewer: DoDraw dst=320x160 paint=0,0,320,160
runtime: sent packet cmd=INPUT_EVENT … bytes=26      ← 鼠标输入正向流动
…
```

具体观察：

1. PIXEL_DATA 到来后 `viewer: DoDraw` 紧跟 `invoking repaint_callback` 出现，
   证明 **`PostThreadMessageW` → 手写泵 → `RepaintWork::DoWork` →
   `DamageFull` → `WM_PAINT` → `DoDraw`** 全链路通畅。
2. 鼠标移动持续触发 `INPUT_EVENT` 发送，gateway 端日志确认收到。
3. 窗口可以稳定运行（不再有"几秒后被 OS 当未响应而关闭"的现象）。
4. 窗口正确显示 apphost 推过来的画面（蓝底 + 中央 8×8 矩形）。

## 后续建议

1. 如果将来确实需要 UI 线程上的周期回调，建议直接用 Win32
   `::SetTimer(hwnd, id, ms, NULL)` —— 触发 `WM_TIMER`，会自动经我们
   的泵派发到 WindowProc。
2. 如果 GKC 后续版本修复了 `m_loop.Run()` 的 plugin DLL 兼容性问题，
   切回 `GuiHelper::Loop()` 只需删掉这段手写泵 + 把 `DoClose` 里的
   `PostQuitMessage` 换回 `GuiHelper::Quit()` 即可，**改动是反向可逆的**。
3. 此问题仅影响 Windows 端 client plugin DLL，**不波及 Linux 上的
   gateway / apphost** —— 那两端各自走 GKC 的 `IoPool`（epoll 实现），
   跟 Windows GUI 子系统是平行的两套机制。
