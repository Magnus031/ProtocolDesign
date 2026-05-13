# Client 代码概览与 Roadmap

## 一、Client 代码 Summary

### 1.1 整体结构

Client 侧由两层组成：

```
client.exe (src/client/main.cpp)
  └── ClientPluginLoader (src/client/plugin_loader.cpp)
        └── client_viewer.dll (plugins/client_viewer/client_viewer.cpp)
              ├── ViewerWindow (ToplevelImpl) — UI 线程
              ├── ClientRuntime (src/client/client.cpp) — 后台网络线程
              └── PixelRenderer (src/client/pixel_renderer.cpp) — 线程安全像素缓冲
```

Client 与 AppHost **完全对称**：`src/client/` 是通用插件加载器可执行程序，实际的 UI 逻辑在 `plugins/client_viewer/` 插件中，通过 `dlopen`(Linux) / `LoadLibrary`(Windows) 动态加载。

### 1.2 各文件职责

| 文件 | 角色 |
|------|------|
| `src/client/main.cpp` | `client.exe` 入口。初始化 GKC 真实 GUI host (`g_win_ui_host`)，解析 `--plugin=` 等命令行参数，加载并运行 viewer 插件。采用 GKC 的 `ProgramEntryPoint::UIMain` 模式。 |
| `src/client/plugin_loader.h/.cpp` | 跨平台 DLL/.so 加载器。Windows: `LoadLibraryA` + `GetProcAddress("_SA_UIMain")`；Linux: `dlopen` + `dlsym`。将 `std::string` args 转为 GKC 的 `ConstStringS`（Windows 走宽字符），然后调用插件的 `_SA_UIMain` 入口。 |
| `src/client/client.h/.cpp` | **ClientRuntime** — 后台 TCP 协议 runtime。跑在独立 `std::thread` 上，负责：socket connect → SPAWN_APP → recv 循环 → MessageParser 定帧 → 按 CmdType 分发（SESSION_ACK / PIXEL_DATA / ERROR_RESP / CLOSE_SESSION）。同时承载 1Hz HEARTBEAT 发送和 INPUT_EVENT 发送。 |
| `src/client/pixel_renderer.h/.cpp` | **PixelRenderer** — 线程安全的 ARGB backstore。`std::mutex` 保护 `pixels_` / `width_` / `height_` / `last_frame_seq_`。网络线程写脏矩形，UI 线程 DoDraw 时 blit 到 GKC draw buffer。支持：dirty rect apply、PIXEL_DATA body 解析（24 字节头 + ARGB payload）、1:1 blit、nearest-neighbour 缩放 blit、stale frame 过滤、越界/长度校验、offline demo 绘制。 |
| `src/client/pixel_renderer_test.cpp` | PixelRenderer 单元测试（headless，跨平台）。覆盖：apply+blit 正确性、stale frame 拒绝、BAD_RECT/BAD_LENGTH 拒绝、body 解析、缩放 blit。 |
| `src/client/client_log.h` | 追加式诊断日志，写到 `client_m6_pid<PID>.log`。Windows GUI subsystem 会吞掉 stderr，此日志是主要排查手段。 |
| `plugins/client_viewer/client_viewer.cpp` | **ViewerWindow** (ToplevelImpl 插件)。可配置画布尺寸与显示缩放；持有 PixelRenderer / ClientRuntime / RepaintWork；DoDraw 做 `blit_scaled_to_fit`；DoMouse/DoKeyboard 做坐标变换后打包 `INPUT_EVENT` 发网络；手写 Win32 `GetMessageW` 消息泵（替代 GKC `GuiHelper::Loop()`）+ 显式 dispatch `UWM_USER_EVENT` 线程消息实现跨线程 PostWork。 |
| `plugins/client_viewer/BUILD` | 构建为 shared library（`client_viewer.dll` / `libclient_viewer.so`），链接 `ws2_32.lib` + `user32.lib`（Windows）。 |

### 1.3 数据流

#### 连接建立
```
ClientRuntime::run()
  → open_socket() → TCP connect Gateway:19000
  → send_packet(SPAWN_APP, sid=0, body="demo_app")
  → recv 循环 → MessageParser::feed() → next_packet()
  → SESSION_ACK → session_id_.store(N)
```

#### PIXEL_DATA 路径（AppHost → 屏幕）
```
网络线程 recv() → MessageParser → handle_packet(PIXEL_DATA)
  → PixelRenderer::apply_pixel_data_body() [mutex 保护]
  → repaint_callback_() → RepaintWork::PostWork()
  → PostThreadMessageW(UWM_USER_EVENT, ...)
UI 线程（手写泵）→ exec(lParam) → DoWork → DamageFull()
  → WM_PAINT → DoDraw → blit_scaled_to_fit() → GKC BitBlt 上屏
```

#### INPUT_EVENT 路径（鼠标/键盘 → AppHost）
```
UI 线程 DoMouse/DoKeyboard
  → 屏幕坐标 → 逻辑坐标变换
  → pack_mouse/keyboard_input_event()
  → runtime_.send_input_event(body)
  → send_packet(INPUT_EVENT, sid, body) [send_mutex_ 下]
  → send() → TCP → Gateway → AppHost
```

### 1.4 线程模型

| 线程 | 持有方 | 做什么 | 不做什么 |
|------|--------|--------|----------|
| 主/UI 线程 | `client.exe` → plugin | wWinMain → GuiMain → 消息泵 → 所有 `Do*` 回调 → `blit*` | recv/send（走 runtime 方法，由 send_mutex_ 或原子量保护） |
| 网络线程 | `ClientRuntime` (std::thread) | recv、MessageParser 解析、PixelRenderer apply、HEARTBEAT 发送、repaint_callback 触发 | GKC widget / GDI / Win32 窗口 API |

跨线程通道：
- **网络线程 → UI 线程**：`PostThreadMessageW(UWM_USER_EVENT, ...)` → 手写泵 inline dispatch → `RepaintWork::DoWork` → `DamageFull()`
- **UI 线程 → 网络线程**：`runtime_.send_input_event()` → `send_mutex_` → `send()`

### 1.5 关键设计决策

1. **Plugin 架构**：Client EXE 是通用加载器，viewer 逻辑在可替换的 DLL 中。与 AppHost 对称——两端都是 "稳定 host EXE + 可替换 plugin DLL"。

2. **Raw socket 而非 GKC IoPool**：ClientRuntime 使用 BSD/Winsock 原生 socket + `std::thread`，而非 GKC IoPool。原因：M6a 期间发现 GKC `m_loop.Run()` 在 plugin DLL 上下文下不工作（消息不再派发），用原生 socket 更可控。代价是与 Gateway/AppHost 的网络模式不统一。

3. **手写 Win32 消息泵**：替代 `GuiHelper::Loop()`，因为 GKC 的 `_w_message_loop_impl::run()` 在 plugin DLL 加载场景下不再 pump 消息。手写泵使用 `GetMessageW` + 显式处理 `UWM_USER_EVENT` 线程消息。详细分析见 `docs/design/M6a_message_pump.md`。

4. **SO_RCVTIMEO = 250ms**：非阻塞式 stop——`stop_requested_` 在 250ms 内必然被读到，`close_socket()` 让 recv 解阻塞，`thread.join()` 不会卡住。

5. **无 GKC TimerImpl 依赖**：心跳在 `ClientRuntime::run()` 内用 `std::chrono::steady_clock` 自计时，不依赖 GKC timer（手写泵下 timer 不 fire）。

---

## 二、Roadmap

### 2.1 当前状态总览

| 子系统 | 状态 | 说明 |
|--------|------|------|
| 连接建立（SPAWN_APP / SESSION_ACK） | ✅ 完成 | raw socket connect + MessageParser |
| PIXEL_DATA 接收与渲染 | ✅ 完成 | PixelRenderer + blit_scaled_to_fit |
| INPUT_EVENT 发送（鼠标/键盘） | ✅ 完成 | DoMouse/DoKeyboard → pack + send |
| HEARTBEAT 发送 | ✅ 完成 | 1Hz，仅在 session_id_ != 0 后 |
| Offline 模式 | ✅ 完成 | 不连 Gateway 也能显示 demo 图案 |
| 跨线程 PostWork（网络→UI） | ✅ 完成 | RepaintWork + 手写泵 |
| 单元测试 (pixel_renderer) | ✅ 完成 | 5 test cases |
| 端到端手动验收 | ✅ 完成 | M6a 60s 稳定性测试通过 |

### 2.2 短期目标（M6b — 2048 应用适配）

当前 `client_viewer.cpp` 的几何参数（32×16 画布、10× 缩放、小矩形切色交互）是 demo_app 专用的。M6b 需要为 2048 应用做适配：

- [ ] **新建 `plugins/client_viewer_2048/` 插件**：针对 4×4 棋盘 + 分数面板的独立 viewer DLL。画布尺寸约 160×200，缩放倍率 4×。方向键映射到 GKC KB_* 枚举发送 INPUT_EVENT。
- [ ] **坐标系统泛化**：当前 `DoMouse` 的屏幕→逻辑坐标反算是针对 demo 32×16 的。2048 viewer 需要自己的坐标映射（可能点击的是 4×4 网格单元格，需要行列转换）。
- [ ] **多 viewer 共存**：确保 `client.exe --plugin=client_viewer_2048.dll` 和 `client.exe --plugin=client_viewer.dll` 都能正常工作，EXE 本体不动。
- [ ] **AppHost 侧 2048 插件**：对应的 `plugins/app_2048/` 插件需要在 AppHost 侧实现游戏逻辑 + IUiHost 绘制（参考已有 `docs/design/2048.md`）。

### 2.3 中期目标（Client 健壮性增强）

- [ ] **重连机制**：当前 `recv()` 返回 0 或连接失败后 runtime 保持断开状态。需要实现自动/手动重连：exponential backoff 重试、re-send SPAWN_APP、重新建立 session。（注意：重连意味着新 session_id，AppHost 侧是全新进程，没有状态恢复，这是设计约束。）
- [ ] **连接状态 UI 反馈**：在 viewer 窗口上显示连接状态指示（已连接/断开/重连中），而非静默失去连接后仍显示最后一帧。
- [ ] **错误信息展示**：`ClientRuntime::last_error()` 目前在 `DoClose` 时未被读取或展示。应在窗口标题栏或 overlay 文字中显示。
- [ ] **输入坐标 clamp**：当前 `(pMouse->x * canvas_width) / display_width` 可能算出越界值，应在 client_viewer 侧 clamp 到 `[0, canvas_width)` 范围。
- [ ] **GKC IoPool 迁移评估**：评估将 ClientRuntime 的 raw socket 迁移到 GKC IoPool 的可行性。好处是与 Gateway/AppHost 网络模式统一；前提是解决 IoPool 在 Windows GUI plugin DLL 下的兼容性问题（或改用 EXE 侧管理 IoPool，通过 interface 暴露给 plugin）。

### 2.4 长期目标（高级特性）

- [ ] **窗口 resize 支持**：当前窗口固定尺寸。需要处理 WM_SIZE → 调整 `blit_scaled_to_fit` 的 dst 尺寸，可能还需要通知 AppHost 改变画布尺寸（需要协议扩展或新增 CmdType）。
- [ ] **多显示器 / DPI 感知**：Windows DPI scaling 下窗口物理像素 ≠ 逻辑像素。需要 `SetProcessDpiAwareness()` 或 manifest 声明 + blit 缩放适配。
- [ ] **像素压缩支持**：当前 PIXEL_DATA 是未压缩 ARGB。如果协议层引入压缩（zlib / JPEG），Client 侧 PixelRenderer 需要解压后再 apply。这也会改变 `apply_pixel_data_body` 的 `dataLen` 语义（当前假设 dataLen == rect 面积 × 4）。
- [ ] **音频流支持**：协议扩展音频 CmdType → ClientRuntime 新增音频处理分支 → viewer 播放。
- [ ] **TLS 加密**：Client ↔ Gateway 之间引入 TLS 隧道（Gateway ↔ AppHost 通常在内网，可暂不加）。raw socket 需要升级为 SSL socket（OpenSSL / SChannel）。
- [ ] **Linux Client**：当前 `main.cpp` 和 `plugin_loader.cpp` 已写好了 Linux 分支（`dlopen` 等），但 `client_viewer.cpp` 内部用了大量 Win32 API（`GetMessageW`、`PostThreadMessageW`、`PostQuitMessage`），且手写泵替代的是 Windows 专有的 GKC `m_loop`。Linux 端需要在 Wayland 下验证 GKC `GuiHelper::Loop()` 是否可用（M6a_message_pump 问题可能仅限 Windows plugin DLL 边界）。如果 Linux 下 GKC Loop 正常工作，则 `client_viewer.cpp` 需要 `#ifdef` 分叉消息泵逻辑。

### 2.5 技术债 / 已知问题

| 问题 | 优先级 | 说明 |
|------|--------|------|
| raw socket 与 IoPool 不统一 | 低 | Client 用 raw socket，Gateway/AppHost 用 IoPool。风格不一致但不是功能性缺陷。 |
| 手写泵丢失 GKC TimerImpl | 低 | 当前未用到，需要时可用 Win32 `SetTimer`/Linux `timerfd` 替代。 |
| GKC 两处本地修复 | 低 | `GkcGui.h` TimerImpl AddTimer 签名修复 + `win_base.h` ShowWindow 后加 UpdateWindow。维护在 GKC fork 上。 |
| `client_log.h` 在每次写时 fopen/fclose | 低 | 低频使用（诊断用途），暂不优化。 |
| viewer demo 几何硬编码 | 中 | 32×16 + 小矩形切色交互写死在 `DoMouse` 里。M6b 新建 2048 viewer 时会自然解决。 |

### 2.6 建议的优先级排序

1. **M6b 2048 viewer 插件**（短期最高优先级）— 证明 plugin 架构的可扩展性
2. **重连机制**（中期）— 提升端到端可用性
3. **连接状态 UI 反馈 + 输入坐标 clamp**（中期）— 打磨用户体验
4. **Linux Client 验证**（中期）— 确认跨平台边界
5. **窗口 resize + 压缩支持**（长期）— 特性完善
