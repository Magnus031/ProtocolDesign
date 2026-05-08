# M6a — Windows GUI Client + 端到端联调

## 一句话总结

M6a 把 M5 的 fake-Client 测试 socket 替换成真实的 Windows GKC GUI 程序。`client.exe`
加载 `client_viewer.dll`（一个 GKC `_SA_UIMain` 插件），打开真实 Win32 窗口，连接
Gateway，发送 `SPAWN_APP("demo_app")`，把收到的 `PIXEL_DATA` 解码到线程安全的
`PixelRenderer` 中，在 `WM_PAINT` 时贴到屏幕；用户在窗口里的鼠标和键盘事件被打成
`INPUT_EVENT` 包发回 AppHost。AppHost 那一侧 M5 时建立的反向通路一行不改。

---

## 在整个项目中的位置

```
M1 (协议层)                  ✅
M2 (IoPool + MessageHandler) ✅
M3 (Gateway 路由 + 生命周期) ✅
M4 (AppHost 插件加载器)      ✅
M5 (INPUT_EVENT 闭环)        ✅
        ↓
M6a (Windows client + viewer + 端到端) ← 当前完成
        ↓
M6b (扩展应用，例如 2048)
```

M5 已经证明协议能在 fake socket 上承载输入事件；M6a 进一步证明**同一套协议在
真实 GUI 进程驱动真实键鼠硬件的场景下依然成立**。M6a 不引入任何新的 wire
command —— 只消费 M1 已经定义好的 `SPAWN_APP` / `SESSION_ACK` / `PIXEL_DATA` /
`INPUT_EVENT` / `HEARTBEAT` / `ERROR_RESP` / `CLOSE_SESSION`。

---

## 交付物清单

| 文件 | 角色 |
|------|------|
| `src/client/main.cpp` | `client.exe` 的 `wWinMain → UIMain` 入口；解析 `--plugin/--gateway-host/--gateway-port/--app`；初始化 `g_win_ui_host`；通过 `ClientPluginLoader` 加载 viewer 插件并把参数透传 |
| `src/client/plugin_loader.h/.cpp` | 跨平台 DLL/.so 加载器：Windows 用 `LoadLibraryA + GetProcAddress("_SA_UIMain")`，Linux 用 `dlopen + dlsym`；把 args 转成 GKC 期望的宽字符 |
| `src/client/client.h/.cpp` (`ClientRuntime`) | 后台 TCP runtime：connect、SPAWN_APP、`MessageParser` 头-体两段解析、`PixelRenderer::apply_pixel_data_body`、`INPUT_EVENT` 发送、1Hz `HEARTBEAT`、`SO_RCVTIMEO` 让 stop 优雅 |
| `src/client/pixel_renderer.h/.cpp` | 线程安全 ARGB backstore：`apply_dirty_rect` / `apply_pixel_data_body` / `blit` / `blit_scaled_to_fit` / `paint_demo`；mutex 保护 `pixels_` / `width_` / `height_` / `last_frame_seq_` |
| `src/client/pixel_renderer_test.cpp` | 单元测试：全帧 apply、dirty rect apply、frame_seq 过滤、body 解析、缩放 blit |
| `src/client/client_log.h` | 追加式日志（写到 `client_m6.log`），M6a 联调期间引入，留作 Windows 端长期诊断工具 |
| `plugins/client_viewer/client_viewer.cpp` | `ToplevelImpl<ViewerWindow>` 插件：32×16 逻辑画布 × 10 倍缩放显示，持有 `ClientRuntime`，通过 `RepaintWork::PostWork` 把网络线程的重绘请求 marshal 到 UI 线程，用手写 Win32 消息泵 |
| `plugins/client_viewer/BUILD` | DLL 输出；链接 `ws2_32.lib`（网络）和 `user32.lib`（手写泵需要） |
| `docs/design/M6a_message_pump.md` | 为什么用手写泵替代 `GuiHelper::Loop()`；故障诊断 + 影响清单 |
| `docs/design/M6a.md` | 本文档 |

---

## 架构总览

```
                    ┌────────────────────────────────────────────────┐
                    │ Windows 主机                                    │
                    │                                                 │
                    │   client.exe (wWinMain → UIMain)                │
                    │      │                                          │
                    │      │ LoadLibrary, GetProcAddress("_SA_UIMain")│
                    │      ▼                                          │
                    │   client_viewer.dll                             │
                    │      │                                          │
                    │      ▼                                          │
                    │   ViewerWindow (Toplevel)                       │
                    │      │   ┌──────────────────────────────┐       │
                    │      │   │ PixelRenderer (32×16 ARGB)   │       │
                    │      │   └──────────────────────────────┘       │
                    │      │   ┌──────────────────────────────┐       │
                    │      └───│ ClientRuntime (std::thread)  │       │
                    │          │   - SPAWN_APP / HEARTBEAT    │       │
                    │          │   - PIXEL_DATA decode        │       │
                    │          │   - INPUT_EVENT pack/send    │       │
                    │          └─────────────┬────────────────┘       │
                    │                        │ TCP                    │
                    └────────────────────────┼────────────────────────┘
                                             │  public port 19000
                    ┌────────────────────────┼────────────────────────┐
                    │ Linux 主机             ▼                        │
                    │                                                 │
                    │   gateway_bin                                   │
                    │      ├─ public listener  :19000  ◄─── client    │
                    │      └─ internal listener :19001 ◄── apphost    │
                    │           │ session-id 路由                     │
                    │           ▼                                     │
                    │   apphost_bin (per session)                     │
                    │      │                                          │
                    │      ▼                                          │
                    │   libdemo_app.so / libapp_2048.so / …          │
                    │      DoDraw → pixel_capture → PIXEL_DATA       │
                    │      DoMouse / DoKeyboard ← INPUT_EVENT        │
                    └─────────────────────────────────────────────────┘
```

Linux 一侧就是 M3 / M4 / M5 的成果，M6a 不动它。所有新东西都在 `client.exe` 和
`client_viewer.dll` 这两个 Windows 二进制里。

---

## 组件设计

### 5.1 `client.exe` —— `src/client/main.cpp`

`client.exe` 是一个 **Windows GUI 子系统**二进制（`/SUBSYSTEM:WINDOWS` +
`/ENTRY:wWinMainCRTStartup`），所以 OS 调的是来自
`third_party/GKC/util/private/include/ui/system/Windows/ui_main.cpp` 的 `wWinMain`。
GKC 的 `wWinMain` 完成标准启动 —— COM STA 初始化、COM security、`time_initialize()`、
命令行 token 化 —— 然后调到我们的 `ProgramEntryPoint::UIMain(args)`。

`UIMain` 的职责（核心 5 步，其余都是日志）：

1. `GKC::g_win_ui_host.Initialize()` —— 初始化 EXE 内的 **Win32 GUI host**
   单例（定义在 `third_party/GKC/util/gui/uihost/include/base/SysDef.cpp`）。
   它就是 Windows 端 IUiHost 的实现；AppHost 在 Linux 那边用的是另一套
   `ui_host_impl` headless host。
2. 遍历 argv，认 `--plugin=<path>`；其余参数原样转发给插件，让插件自己解析
   `--gateway-host` / `--gateway-port` / `--app`。
3. 默认插件路径是 `client_viewer.dll`（Windows）或 `libclient_viewer.so`（Linux），
   不带 `--plugin=` 也能跑标准 demo。
4. `ClientPluginLoader::open(plugin_path)` —— `LoadLibrary` + 解析
   `_SA_UIMain` 入口。
5. `ClientPluginLoader::run(LcInterface<IUiHost>(&g_win_ui_host,
   &g_ui_host_interface), forwarded_args)` —— 把"函数表指针 + 上下文指针"交给
   插件，让插件接管，直到关窗。

**为什么用插件架构？** 与 AppHost 对称。两边都是"稳定的 host EXE + 可替换的
plugin DLL"。M6c 我们会引入一个 2048 专属的 viewer（更大窗口、方向键映射），
只换 DLL，`client.exe` 不动。

---

### 5.2 `ClientPluginLoader` —— `src/client/plugin_loader.h/.cpp`

`LoadLibrary` / `dlopen` 的薄封装：

```cpp
loader.open("client_viewer.dll")
  ↓
HMODULE = LoadLibraryA(path);
FARPROC = GetProcAddress(HMODULE, "_SA_UIMain");

loader.run(lcHost, args)
  ↓
1. 把 std::string args -> std::wstring -> GKC::ConstStringS
   （Windows _SA_UIMain 期望宽字符；Linux 直接窄字符传过去）
2. 包装成 GKC::ConstArray<ConstStringS>
3. 调 entry_(lcHost, arg_array)
```

loader **不持有** `lcHost` 的生命周期。`lcHost` 指向 EXE 里的 `g_win_ui_host` 和
`g_ui_host_interface`，插件返回时 `loader.close()` 调 `FreeLibrary` /
`dlclose`，host 单例继续存在（直到 EXE 进程退出才一起销毁）。

---

### 5.3 `ClientRuntime` —— `src/client/client.h/.cpp`

协议 runtime。**完全跑在后台 `std::thread`** 上，UI 线程不会因为 `recv()` 而
阻塞。它和 UI 之间的双向通信很少：

```
UI 线程                                后台网络线程 (ClientRuntime::run)
───────                                ───────────────────────────────
runtime_.start(opts, &renderer,        ┌── open_socket() 到 gateway
               []{repaint_work_       │
                  .PostWork();})  ───►│   设 SO_RCVTIMEO = 250ms
                                      │   send_spawn_app()
                                      │
                                      │   loop until stop_requested_:
                                      │     若 1s 已过: send HEARTBEAT
                                      │     recv() 字节
                                      │     parser_.feed()
                                      │     while next_packet():
                                      │       SESSION_ACK -> session_id_.store
                                      │       PIXEL_DATA  -> renderer.apply_..
                                      │                      repaint_callback_()
                                      │       ERROR_RESP  -> set_error()
                                      │
runtime_.send_input_event(body)  ◄────┤   (在 send_mutex_ 下；
                                      │    若 !connected_ 直接返回)
                                      │
runtime_.stop()  (从 DoClose 调)  ───►│   close_socket() 让 recv() 解阻塞
                                      │   thread.join()
                                      └──
```

三个并发不变量：

1. **`socket_` 单写者**。所有 `send()` 都在 `send_mutex_` 内串行。UI 线程的
   `INPUT_EVENT` 可能和网络线程的 `HEARTBEAT` 在时间上交错，mutex 保证 wire
   字节流不被撕裂。
2. **跨线程状态用原子量**：`connected_`、`session_id_`、`stop_requested_`。
   UI 线程从 `session_id_` 拿当前 sid 给 `INPUT_EVENT` 拼 header；网络线程在
   `SESSION_ACK` 时写它。
3. **像素写不会卡住 UI**。`PixelRenderer::apply_*` 拿 renderer 自己的 mutex；
   UI 的 `DoDraw` 读时也拿同一把锁。GKC 对象不跨线程移动，唯一共享的可变状态
   就是渲染缓冲区。

两个值得拿出来讲的设计选择：

- **`SO_RCVTIMEO = 250 ms`**，而不是无超时阻塞 `recv()`。这样 `runtime_.stop()`
  之后 `stop_requested_.load()` 在 250ms 内必然被读到，关窗不会卡住。误唤醒
  靠 `is_recv_timeout()`（`WSAETIMEDOUT` / `EAGAIN|EWOULDBLOCK`）过滤掉。
- **1Hz HEARTBEAT**，`cmd=HEARTBEAT, body=empty`。仅在 `session_id_ != 0`
  之后才开始发。和 M3 在 Gateway↔AppHost 之间走的内部心跳一样，只是这条心跳
  跑在 Client↔Gateway 之间。

---

### 5.4 `PixelRenderer` —— `src/client/pixel_renderer.h/.cpp`

线程安全的 ARGB backstore。**网络线程**写脏矩形，**UI 线程**在 `DoDraw` 里通过
`blit*` 读，两者通过它解耦。

状态：

```cpp
mutable std::mutex mutex_;
int width_, height_;        // 逻辑画布尺寸；M6a 是 32×16
uint32_t last_frame_seq_;   // 单调递增，保留最新一帧的 seq
std::vector<color_quad> pixels_;
```

公共接口：

| 方法 | 调用方 | 作用 |
|------|-------|------|
| `reset(w,h,fill)` | UI 线程 / ctor | 重置画布尺寸和填充色 |
| `apply_pixel_data_body(body, len)` | 网络线程 | 解析 24 字节协议 body 头（frame_seq / rect / data_len），转交给 `apply_dirty_rect` |
| `apply_dirty_rect(seq, rect, pixels, len)` | 网络线程 | 把 `len` 字节 ARGB 数据写到 `pixels_[rect]`。stale seq、越界、长度错都拒绝 |
| `blit(dst, dw, dh, paint)` | UI 线程（`DoDraw`）| 1:1 把裁剪后的 `paint` 拷到 `dst` |
| `blit_scaled_to_fit(dst, dw, dh, paint)` | UI 线程（`DoDraw`）| 最近邻把 `pixels_` 放大到 `dst[paint]`。M6a 用得到，因为窗口是 320×160 但画布 32×16 |
| `paint_demo(yellow, cyan)` | UI 线程（offline / 启动）| 填一张已知的测试图案，让窗口在 runtime 还没起来时也有内容 |

`apply_pixel_data_body` 处理的 wire 格式：

```
[frame_seq:BE32][rect_left:BE32][rect_top:BE32][rect_right:BE32]
[rect_bottom:BE32][data_len:BE32][pixels...]   (header = 24 字节)

pixels 行优先排列，(right - left) * (bottom - top) 个像素，
每像素 4 字节按 [A, R, G, B] 顺序（wire 字节序）。
```

像素字节序的转换发生在 `argb_to_color_quad`：读 `p[0]=A, p[1]=R, p[2]=G,
p[3]=B`，再 `COLOR_QUAD_MAKE(r,g,b,a)`。这正好对应 AppHost 端
`pack_pixel_data_rect` 把 GKC 内存里的 BGRA 转成线上 `[A,R,G,B]` 的逻辑——
参见 `src/apphost/pixel_capture.h`。

---

### 5.5 `client_viewer.dll` —— `plugins/client_viewer/client_viewer.cpp`

插件本体。三个类：

```cpp
class RepaintWork : public GKC::WorkImpl<RepaintWork> { … };
class ViewerWindow : public GKC::ToplevelImpl<ViewerWindow> { … };
class ProgramEntryPoint { static int GuiMain(args); };
```

#### ViewerWindow

逻辑 32×16 的窗口，屏幕上放大到 320×160（`kDisplayScale = 10`）。持有
`PixelRenderer`、`ClientRuntime`、`RepaintWork` 各一份。

| 方法 | 作用 |
|------|------|
| ctor | `renderer_.reset(32,16,BLUE)` + `paint_demo(false,false)`，把 `repaint_work_` 绑到 `this`。**网络还没连上之前**窗口就有内容（offline 模式也能看见画面） |
| `Start(opts)` | `Create(true, 320, 160) → Show(true) → DamageFull()`；非 offline 模式调 `runtime_.start(opts, &renderer_, []{repaint_work_.PostWork();})` |
| `DoDraw(pDraw)` | `renderer_.blit_scaled_to_fit(pDraw->pBuffer, pDraw->iWidth, pDraw->iHeight, pDraw->rcPaint)` |
| `DoMouse(pMouse)` | 把屏幕坐标按 `kDisplayScale` 反算到逻辑坐标；用 `mouse_event_type()` 分类；`runtime_.send_input_event(pack_mouse_input_event(...))`。**额外**：左键按下落在 `{8,4,16,12}` 内时，本地切一下黄色矩形（不需要走网络） |
| `DoKeyboard(pKb)` | `runtime_.send_input_event(pack_keyboard_input_event(KEY_DOWN/UP, btKey, ts))`。**额外**：`KB_F1` 按下时本地切青色/蓝色背景 |
| `DoClose()` | `runtime_.stop()` 然后 `::PostQuitMessage(0)`（见 §6） |

#### RepaintWork

`WorkImpl<RepaintWork>` 只为一件事而存在：**让网络线程能请求 UI 线程重绘，
但不直接跨线程调 GKC 接口**。

```cpp
// 网络线程，在 PIXEL_DATA 应用成功之后：
repaint_work_.PostWork();
   ↓
g_ui_host.GetFunc()->PostWork(g_ui_host.GetContext(),
                              WorkProc{&Exec},
                              static_cast<RepaintWork*>(this))
   ↓ (host 那边，_UiHost::PostWork)
g_win_ui_host.PostWork(...)
   ↓
PostThreadMessageW(main_thread_id,
                   UWM_USER_EVENT (= WM_USER + 100),
                   wParam = &Exec,
                   lParam = &repaint_work_)
   ↓ (UI 线程，在我们的手写泵里——见 §6)
((void(*)(void*))wParam)((void*)lParam)
   ↓
RepaintWork::Exec → DoWork → window_->DamageFull()
   ↓
::InvalidateRect → 队列里下一条 WM_PAINT → DoDraw → blit_scaled_to_fit
```

之所以走 `PostThreadMessageW`、而不是从网络线程直接调
`InvalidateRect`：跨线程调 `PostMessage` 在 Windows 上**理论上**线程安全，但
某些环境下会延迟或阻塞；线程消息这条路是 GKC 的标准 marshal 机制，和 AppHost
端把 `INPUT_EVENT` marshal 到主线程是一样的模式。

#### ProgramEntryPoint::GuiMain

`GuiMain(args)` 做三件事：

```cpp
ViewerWindow win;
win.Start(parse_options(args));   // Create + Show + (可能的) runtime.start
                                  // 只有 connect() 这一步会阻塞，但它跑在
                                  // 后台网络线程上

// 手写 Win32 消息泵 (§6 解释为什么)
MSG msg{};
for (;;) {
    BOOL got = ::GetMessageW(&msg, NULL, 0, 0);
    if (got == 0 || got == -1) break;          // WM_QUIT / 错误
    if (msg.hwnd == NULL && msg.message == kUserEvent) {
        // UWM_USER_EVENT 线程消息：跨线程 PostWork 的载体。
        // DispatchMessage 默认丢弃线程消息，所以我们 inline 执行。
        using ExecFn = void (*)(void*) noexcept;
        ExecFn exec = reinterpret_cast<ExecFn>(msg.wParam);
        if (exec) exec(reinterpret_cast<void*>(msg.lParam));
        continue;
    }
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
}
return 0;
```

---

## 为什么手写消息泵

原计划用 `GuiHelper::Loop()`（即 GKC 的 `_w_message_loop_impl::run()`）。
实际跑起来发现：在 plugin DLL 这种调用上下文下它**不再派发任何消息** ——
`InvalidateRect` 排进队列的 `WM_PAINT` 永远不会到 WindowProc，GKC 自己的
`TimerImpl` 回调也一次都不 fire，跨线程 `PostWork` 也到不了 UI 线程。

我们没有继续往 GKC 内部追 —— 替换泵是 30 行的小改动，而且 `LocalDesk`（GKC
官方示例）能跑，说明问题特定于 plugin DLL 这种调用边界。完整故障诊断、以及
替换之后哪些 GKC 子系统还能用、哪些丢掉，见
[M6a_message_pump.md](M6a_message_pump.md)。简短总结：

- **照常工作**：窗口创建/销毁、绘制派发、鼠标键盘、跨线程 `WorkImpl::PostWork`
  （我们在泵里 inline 处理 `UWM_USER_EVENT`）
- **失效**：`TimerImpl::AddTimer`（GKC 的 `process_timers` 在我们绕过的 `Run()`
  里 —— 当前未使用所以无影响）
- **替换**：`GuiHelper::Quit()` 也失效；`DoClose` 改用 `::PostQuitMessage(0)`
  来退出泵

---

## 端到端数据流

### 7.1 连接建立

```
Client                              Gateway                              AppHost
──────                              ───────                              ───────
runtime_.start()
  socket() / connect(:19000)  ───►  在 public 端口 accept
                                    [client connected id=...]

send SPAWN_APP                ───►  recv SPAWN_APP
  (sid=0, body="demo_app")          解析 app 名
                                    fork apphost_bin --apphost-internal-port=19001
                                    apphost 连接 internal 端口
                                                                  ◄──── apphost connect
                                    [apphost connected id=...]
                                    apphost 发 APPHOST_READY (sid=N)
                                                                  ◄──── APPHOST_READY
                                    SessionTable[N]={client_id, apphost_id}
                                    转发给 client
recv SESSION_ACK              ◄──── SESSION_ACK (sid=N, body=empty)
  session_id_.store(N)

                                                                        plugin _SA_UIMain
                                                                        ViewerWindow.Show
                                                                        DoDraw #1 (RED)
                                                                        Damage(small)
                                                                        DoDraw #2 (GREEN)
                                                                        capture+pack
                                    ◄──── PIXEL_DATA (sid=N, 全帧)
recv PIXEL_DATA               ◄──── 按 sid 转发
  apply_pixel_data_body
  repaint_callback → PostWork
DoDraw → blit                       ◄──── PIXEL_DATA (sid=N, 脏矩形)
recv PIXEL_DATA
  apply_pixel_data_body
  repaint_callback → PostWork
DoDraw → blit
```

到用户在窗口里看到 apphost 内容时，已经有 3 段 TCP 通信发生过：

1. Client → Gateway（`SPAWN_APP`）
2. Gateway → AppHost spawn + AppHost → Gateway（`APPHOST_READY`）
3. Gateway → Client（`SESSION_ACK`）以及 AppHost → Gateway → Client
   （M4 启动两帧 `PIXEL_DATA`）

---

### 7.2 PIXEL_DATA 路径（apphost → 屏幕）

```
apphost 插件 DoDraw / Damage
      │
      ▼
ui_host_impl pixel sink
      │  pack_pixel_data_rect (BGRA → ARGB；构造 24 字节 body 头)
      ▼
apphost socket send (Header + body)
      │ Header: magic|sid|cmd=PIXEL_DATA|body_len|0
      │ Body:   [frame_seq][L][T][R][B][data_len][pixels…]
      ▼
Gateway internal 监听线程
      │ MessageHandler.feed → packet
      │ SessionTable 按 sid 查找
      ▼
原样转发到 public 侧的 client 连接 (Header + Body 不动)
      │
      ▼  TCP
Client recv()
      │ ClientRuntime::run → MessageParser::feed → next_packet
      ▼
handle_packet(PIXEL_DATA)
      │ renderer_->apply_pixel_data_body(body, body_len)
      │   - 从前 24 字节读 frame_seq、rect、data_len
      │   - frame_seq < last_frame_seq_  → 丢 (stale)
      │   - rect 越界                    → 丢 (BAD_RECT)
      │   - data_len != width*height*4   → 丢 (BAD_LENGTH)
      │   - mutex 下逐行把 ARGB 转成 color_quad 写入 pixels_
      ▼
repaint_callback_()
      └─► repaint_work_.PostWork()
              └─► PostThreadMessageW(UWM_USER_EVENT, &Exec, &repaint_work_)

UI 线程（手写泵 GetMessageW 返回）
      │ msg.hwnd==NULL && msg.message==UWM_USER_EVENT
      │   → exec(lParam) → RepaintWork::DoWork
      │     → window_->DamageFull() → ::InvalidateRect
      ▼
下一次 GetMessageW 返回 WM_PAINT
      │ DispatchMessageW → GKC WindowProc → Process(UI_MESSAGE_DRAW)
      ▼
ViewerWindow::DoDraw(pDraw)
      │ renderer_->blit_scaled_to_fit(pDraw->pBuffer,
      │                                pDraw->iWidth,
      │                                pDraw->iHeight,
      │                                pDraw->rcPaint)
      ▼
GKC BitBlt 上屏 (在 win_base_inc.cpp 的 WM_PAINT 处理里)
```

---

### 7.3 INPUT_EVENT 路径（鼠标 / 键盘 → apphost）

```
用户移动鼠标 / 点击 / 按键
      │
      ▼  Win32 消息
WindowProc 收到 WM_MOUSEMOVE / WM_LBUTTONDOWN / WM_KEYDOWN…
      │
      ▼  GKC 翻译并调到我们注册的 message handler
ViewerWindow::DoMouse(pMouse) 或 DoKeyboard(pKb)
      │ (UI 线程)
      │ 屏幕坐标 → 逻辑坐标：(pMouse->x * 32) / 320
      │ 分类 (mouse_event_type / btDown)
      │ pack_mouse_input_event() / pack_keyboard_input_event()
      ▼
runtime_.send_input_event(body)
      │ 若 session_id_==0 || !connected_，直接返回
      │ send_packet(INPUT_EVENT, sid, body)  [在 send_mutex_ 下]
      ▼  TCP
Gateway public 监听线程
      │ Header.sid 查表
      │ 转发到 apphost 连接 (Header + Body 原样)
      ▼
AppHost reader 线程
      │ MessageHandler → INPUT_EVENT lambda
      │ parse_input_event_body() → ParsedInputEvent
      │ ui_host_.post_*_input(msg)   (M5 契约)
      ▼  PostWork → 主线程 (M5 契约)
drain_input_queue_on_main_thread()
      │ swap 队列；逐条事件：
      │   dispatch_mouse / dispatch_keyboard
      │     → window.handler.Process(UI_MESSAGE_MOUSE / UI_MESSAGE_KEYBOARD)
      │       → DemoWindow::DoMouse / DoKeyboard
      │         → 可能调 Damage(rect)
      │           → dispatch_draw_and_send → PIXEL_DATA → 上线 ────►
                                                      (回到 7.2)
```

Client 只负责发原始键码（`pKb->btKey`）和 32×16 逻辑坐标。坐标缩放和事件分类
全在客户端做；AppHost 那边和 M5 完全一致。

---

### 7.4 心跳

`session_id_` 一旦非零，`ClientRuntime::run` 每隔 1 秒发一条
`Header{cmd=HEARTBEAT, sid=N, body_len=0}`。Gateway 把它转发给对应的 AppHost；
AppHost 可以丢掉也可以原样回。M6a 不解析回包 —— 心跳的意义是让 OS 维持 TCP
通路，并且让 Gateway 通过 socket EOF 而不是超时来探测掉线的 client。

---

### 7.5 退出

```
用户点关闭按钮
      │
      ▼  WM_CLOSE → DispatchMessage → WindowProc → Process(UI_MESSAGE_CLOSE)
ViewerWindow::DoClose()
      │ runtime_.stop()
      │   stop_requested_.store(true)
      │   close_socket() → recv() 返回 0 / WSAESHUTDOWN
      │   thread.join()
      │ ::PostQuitMessage(0)
      ▼
GetMessageW 返回 0 → 泵退出
      ▼
GuiMain 返回 → _SA_UIMain 返回 → loader.run() 返回 → UIMain 返回
      ▼
wWinMain 返回 → 进程退出
```

Gateway 看到 public 侧 socket 关闭，按 M3 的逻辑销毁 session：发 `CLOSE_SESSION`
给 AppHost，AppHost 插件 `DoClose` 触发，AppHost 进程退出。

---

## 线程模型 —— 总结

| 线程 | 持有方 | 做什么 | 不做什么 |
|------|-------|--------|----------|
| 主 / UI 线程 | `client.exe` | wWinMain、插件 `GuiMain`、消息泵、所有 `Do*` 回调、`blit*` | `recv` / `send`（这些走 runtime 的方法，由 `send_mutex_` 或原子量保护） |
| 网络线程 | `ClientRuntime` | `recv`、解析、`apply_*`、`repaint_callback_`；发出去的 `HEARTBEAT` 和 `INPUT_EVENT` | GKC widget、GDI / Win32 窗口 API（只通过 `PostThreadMessageW` 线程消息间接交互） |

跨线程通道：

- **网络线程 → UI 线程**：`PostThreadMessageW(UWM_USER_EVENT, …)` 携带
  `RepaintWork` 指针；在我们泵里手动 dispatch。
- **UI 线程 → 网络线程**：`runtime_.send_input_event(body)` → `send_mutex_` →
  `send()`。

没有条件变量，没有共享队列 —— `PixelRenderer` 是唯一的共享可变状态，它的
mutex 私有自己持有。

---

## 编译与运行

```powershell
# 1. 构建 (Windows host)
$env:PATH = "D:\Tools\bazel;" + $env:PATH
bazel build //src/client:client //plugins/client_viewer:client_viewer

# 2. 连接远端 gateway 跑
.\bazel-bin\src\client\client.exe `
    --plugin=.\bazel-bin\plugins\client_viewer\client_viewer.dll `
    --gateway-host=<gateway-ip> `
    --gateway-port=19000 `
    --app=demo_app
```

```bash
# Linux gateway 一侧（M3/M4/M5 不变）
bazel-bin/src/gateway/gateway_bin \
    --public-port=19000 \
    --apphost-internal-port=19001 \
    --apphost-bin=$PWD/bazel-bin/src/apphost/apphost_bin \
    --app=demo_app=$PWD/bazel-bin/plugins/demo_app/libdemo_app.so
```

Offline 模式（不连网关，只验证窗口本身）：

```powershell
.\bazel-bin\src\client\client.exe `
    --plugin=.\bazel-bin\plugins\client_viewer\client_viewer.dll `
    --offline
```

诊断日志：每条关键事件会写一行到 `client_m6.log`（在 cwd 下）。Windows
GUI 子系统会吞掉 stderr，所以这个日志在排查问题时很有用。

---

## 测试覆盖

### `pixel_renderer_test`（单元测试，headless）

| 用例 | 验证内容 |
|------|---------|
| `ResetSetsBackground` | `reset(w,h,fill)` 创建出正确大小和填色 |
| `AppliesFullFrame` | 全画布 dirty rect + 合法 ARGB → 整个 buffer 被写入；`last_frame_seq` 推进 |
| `AppliesDirtyRect` | 子区域 apply 只写 rect 内，外部不动 |
| `RejectsStaleFrameSeq` | `seq < last_frame_seq` 返回 `STALE_FRAME` |
| `RejectsBadRect` | 越界 rect 返回 `BAD_RECT` |
| `RejectsBadLength` | `len != width*height*4` 返回 `BAD_LENGTH` |
| `ParsesPixelDataBody` | 完整 body 解析能正确读 24 字节头 + payload |
| `BlitsScaledFrame` | `blit_scaled_to_fit` 把 2×2 ARGB 放大到 4×4，四角颜色正确（M6a 新增） |

### 手动端到端（M6a 验收）

按 `docs/plan/process.md` §6.5：

| 场景 | 步骤 | 通过标准 |
|------|------|---------|
| 连接 + 接收 | 启动 gateway+demo_app；启动 `client.exe`；观察窗口 | 启动两帧能看见：先全蓝，再蓝底 + 绿色小矩形 |
| 本地交互 | 点小矩形，按 F1 | 本地切色立刻可见（GREEN↔YELLOW / BLUE↔CYAN） |
| 网络交互 | 同上点击/按键，看 gateway 日志 | gateway 日志里看到 `INPUT_EVENT`；AppHost 发新 `PIXEL_DATA`；窗口随即更新 |
| 稳定性 | 持续运行 60 秒 | 没有"无响应"的转圈光标；`client_m6.log` 里每秒一条心跳；窗口不掉 |
| 干净退出 | 点 X 关闭 | 日志有 `runtime: stopped` 和 `viewer: pump exited`；进程返回 |

---

## M6a 边界声明（**没做**什么）

- **没有 2048 / 没有 app 专属 viewer**。M6a 的 `client_viewer.dll` 把 32×16
  / 320×160 的 demo 几何和 M4 demo 交互（小矩形切色、F1 切背景）写死了。M6c
  会引入第二个 viewer 插件，针对 4×4 棋盘做适配。
- **没有重连 / 没有 failover**。`recv()` 返回 0 或 `getaddrinfo` 失败时，
  runtime 设个 error 后保持断开状态。窗口里继续显示最后一次解码的像素。
- **没有 DPI / 没有 resize**。窗口固定创建为 320×160，缩放器假设这个目标
  尺寸。resize 消息会让 `blit_scaled_to_fit` 缩到新尺寸，但画布仍是 32×16。
- **输入坐标不做 clamp**。`(pMouse->x * 32) / 320` 在鼠标飞到 non-client
  区时可能算出 `[0,32)` 之外的值；AppHost 自己会忽略越界输入（M5 已经默默
  丢掉小矩形外的 `MOUSE_EVENT_DOWN`）。
- **没有基于 timer 的重绘**。GKC 的 `TimerImpl` 在我们的泵下不工作；当前
  也用不到。如果后续插件需要周期性 UI 任务，用 Win32 `::SetTimer(hwnd, ..)`
  即可，`WM_TIMER` 会经手写泵正常派发。

---

## GKC 本地修复

GKC submodule 里保留了两处与 upstream 不一致的修复（提交记录见
`third_party/GKC` 历史）：

1. `public/include/base/GkcGui.h` —— `TimerImpl::AddTimer` 转发到
   `IUiHost::AddTimer` 时漏了 `iPeriod` 参数。upstream 这个签名不匹配让任何
   `AddTimer` 调用都链接不过。M6a 生产代码不调 `AddTimer`，但联调期间用过
   一个诊断用的 `HeartbeatTimer`，加上修复让 `bazel build //...` 能通过。
2. `util/gui/uihost/include/base/_system_/Windows/_core_/win_base.h` ——
   在 `::ShowWindow()` 和 `::InvalidateRect()` 之后加 `::UpdateWindow()`。
   这让首次 `Show(true)` 同步触发绘制（在进 `GuiHelper::Loop()` 之前就有第
   一次 `DoDraw`），并让 `Damage()` 在我们的手写泵里下一次 `GetMessage`
   时立刻重绘 —— 原本的异步 `InvalidateRect` 在我们这条路径下并不总是这样。

这两处修复维护在 GKC 的 fork 上，submodule URL 见 `.gitmodules`。

---

## 验收

Headless 单元测试：

```
bazel test //src/client:pixel_renderer_test
```

外加 M3 / M4 / M5 的服务端套件保持通过：

```
bazel test //src/apphost:m4_plugin_test \
           //src/apphost:m5_input_test \
           //src/apphost:apphost_lifecycle_test \
           //src/gateway:gateway_test \
           //src/gateway:gateway_lifecycle_test \
           //src/gateway:gateway_m4_plugin_test \
           //src/gateway:gateway_m5_input_test \
           //src/common:input_event_test
```

手动端到端（按 §9）：demo_app 在 `client.exe` 上连续运行至少 60 秒，鼠标 + F1
触发的切色都正确，X 关闭时窗口干净退出。
