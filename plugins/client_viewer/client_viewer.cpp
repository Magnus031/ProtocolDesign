#include <chrono>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "base/GkcDef.h"
#include "base/GkcGui.h"
#include "src/client/client.h"
#include "src/client/client_log.h"
#include "src/client/pixel_renderer.h"
#include "src/common/input_event.h"

namespace {

constexpr int kWindowWidth = 32;
constexpr int kWindowHeight = 16;
constexpr int kDisplayScale = 10;
constexpr int kDisplayWidth = kWindowWidth * kDisplayScale;
constexpr int kDisplayHeight = kWindowHeight * kDisplayScale;
constexpr int kSmallLeft = 8;
constexpr int kSmallTop = 4;
constexpr int kSmallRight = 16;
constexpr int kSmallBottom = 12;

struct ViewerOptions {
    std::string gateway_host = "127.0.0.1";
    uint16_t gateway_port = 19000;
    std::string app_name = "demo_app";
    bool offline = true;
};

std::string to_std_string(const GKC::ConstStringS& s) {
    std::string out;
    out.reserve(static_cast<size_t>(s.GetLength()));
    for (uintptr i = 0; i < s.GetLength(); ++i) {
        out.push_back(static_cast<char>(s.GetAddress()[i]));
    }
    return out;
}

bool starts_with(const std::string& s, const char* prefix) {
    const std::string p(prefix);
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

uint64_t timestamp_us() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

ViewerOptions parse_options(const GKC::ConstArray<GKC::ConstStringS>& args) {
    ViewerOptions out;
    std::vector<std::string> tokens;
    for (uintptr i = 0; i < args.GetCount(); ++i) {
        std::istringstream parts(to_std_string(args[i]));
        std::string token;
        while (parts >> token) tokens.push_back(std::move(token));
    }

    for (const std::string& arg : tokens) {
        client_log_line("viewer: arg " + arg);
        if (starts_with(arg, "--gateway-host=")) {
            out.gateway_host = arg.substr(std::string("--gateway-host=").size());
            out.offline = false;
        } else if (starts_with(arg, "--gateway-port=")) {
            const int port =
                std::stoi(arg.substr(std::string("--gateway-port=").size()));
            if (port > 0 && port <= 65535)
                out.gateway_port = static_cast<uint16_t>(port);
            out.offline = false;
        } else if (starts_with(arg, "--app=")) {
            out.app_name = arg.substr(std::string("--app=").size());
        } else if (arg == "--offline") {
            out.offline = true;
        }
    }
    client_log_line("viewer: options offline=" +
                    std::to_string(out.offline ? 1 : 0) +
                    " host=" + out.gateway_host +
                    " port=" + std::to_string(out.gateway_port) +
                    " app=" + out.app_name);
    return out;
}

InputEventType mouse_event_type(const GKC::UiMessageMouse& mouse) {
    if (mouse.uEvent == MOUSE_EVENT_DOWN &&
        mouse.btButton == MOUSE_BUTTON_LEFT) {
        return InputEventType::MOUSE_LEFT_DOWN;
    }
    if (mouse.uEvent == MOUSE_EVENT_UP &&
        mouse.btButton == MOUSE_BUTTON_LEFT) {
        return InputEventType::MOUSE_LEFT_UP;
    }
    if (mouse.uEvent == MOUSE_EVENT_DOWN &&
        mouse.btButton == MOUSE_BUTTON_RIGHT) {
        return InputEventType::MOUSE_RIGHT_DOWN;
    }
    if (mouse.uEvent == MOUSE_EVENT_UP &&
        mouse.btButton == MOUSE_BUTTON_RIGHT) {
        return InputEventType::MOUSE_RIGHT_UP;
    }
    if (mouse.uEvent == MOUSE_EVENT_WHEEL) {
        return InputEventType::MOUSE_SCROLL;
    }
    return InputEventType::MOUSE_MOVE;
}

class ViewerWindow;

class RepaintWork : public GKC::WorkImpl<RepaintWork> {
public:
    void Bind(ViewerWindow* window) noexcept {
        window_ = window;
    }

    void DoWork() noexcept;

private:
    ViewerWindow* window_ = nullptr;
};

class ViewerWindow : public GKC::ToplevelImpl<ViewerWindow> {
public:
    ViewerWindow() {
        renderer_.reset(kWindowWidth, kWindowHeight, COLOR_QUAD_BLUE);
        renderer_.paint_demo(false, false);
        repaint_work_.Bind(this);
    }

    bool Start(const ViewerOptions& options) {
        options_ = options;
        if (!Create(true, kDisplayWidth, kDisplayHeight)) {
            client_log_line("viewer: Create failed");
            return false;
        }
        client_log_line("viewer: window created");
        Show(true);
        client_log_line("viewer: window shown");
        DamageFull();

        if (!options_.offline) {
            ClientRuntime::Options runtime_options;
            runtime_options.gateway_host = options_.gateway_host;
            runtime_options.gateway_port = options_.gateway_port;
            runtime_options.app_name = options_.app_name;
            runtime_options.connect_gateway = true;
            runtime_.start(runtime_options, &renderer_,
                           [this]() { repaint_work_.PostWork(); });
            client_log_line("viewer: runtime started");
        } else {
            client_log_line("viewer: offline mode");
        }
        return true;
    }

    void DoDraw(GKC::UiMessageDraw* pDraw) noexcept {
        if (pDraw == nullptr) return;
        if (draw_count_ < 8) {
            client_log_line("viewer: DoDraw dst=" +
                            std::to_string(pDraw->iWidth) + "x" +
                            std::to_string(pDraw->iHeight) + " paint=" +
                            std::to_string(pDraw->rcPaint.L()) + "," +
                            std::to_string(pDraw->rcPaint.T()) + "," +
                            std::to_string(pDraw->rcPaint.R()) + "," +
                            std::to_string(pDraw->rcPaint.B()));
        }
        ++draw_count_;
        renderer_.blit_scaled_to_fit(pDraw->pBuffer, pDraw->iWidth,
                                     pDraw->iHeight, pDraw->rcPaint);
    }

    void DoMouse(GKC::UiMessageMouse* pMouse) noexcept {
        if (pMouse == nullptr) return;
        const int logical_x = (pMouse->x * kWindowWidth) / kDisplayWidth;
        const int logical_y = (pMouse->y * kWindowHeight) / kDisplayHeight;
        const InputEventType type = mouse_event_type(*pMouse);
        runtime_.send_input_event(
            pack_mouse_input_event(type, logical_x, logical_y, timestamp_us()));

        if (type == InputEventType::MOUSE_LEFT_DOWN &&
            logical_x >= kSmallLeft && logical_x < kSmallRight &&
            logical_y >= kSmallTop && logical_y < kSmallBottom) {
            yellow_rect_ = !yellow_rect_;
            renderer_.paint_demo(yellow_rect_, cyan_background_);
            DamageFull();
        }
    }

    void DoKeyboard(GKC::UiMessageKeyboard* pKb) noexcept {
        if (pKb == nullptr) return;
        const InputEventType type = pKb->btDown ? InputEventType::KEY_DOWN
                                                : InputEventType::KEY_UP;
        runtime_.send_input_event(
            pack_keyboard_input_event(type, pKb->btKey, timestamp_us()));

        if (pKb->btDown == 1 && pKb->btKey == KB_F1) {
            cyan_background_ = !cyan_background_;
            renderer_.paint_demo(yellow_rect_, cyan_background_);
            DamageFull();
        }
    }

    void DoClose() noexcept {
        runtime_.stop();
        ::PostQuitMessage(0);
    }

    void DamageFull() noexcept {
        GKC::UiRect r;
        r.Set(0, 0, kDisplayWidth, kDisplayHeight);
        Damage(r);
    }

private:
    ViewerOptions options_;
    PixelRenderer renderer_;
    ClientRuntime runtime_;
    RepaintWork repaint_work_;
    bool yellow_rect_ = false;
    bool cyan_background_ = false;
    int draw_count_ = 0;
};

void RepaintWork::DoWork() noexcept {
    if (window_ != nullptr) window_->DamageFull();
}

}  // namespace

namespace GKC {

class ProgramEntryPoint {
public:
    static int GuiMain(const ConstArray<ConstStringS>& args) {
        if (g_ui_host.GetFunc().IsNull()) {
            std::fprintf(stderr, "client_viewer: g_ui_host not injected\n");
            return 99;
        }

        ViewerWindow win;
        if (!win.Start(parse_options(args))) return 1;
        client_log_line("viewer: entering message pump");

        // UWM_USER_EVENT mirrors GKC's _w_message_loop_impl: WM_USER + 100.
        // Cross-thread PostWork() arrives as a thread message (msg.hwnd=NULL)
        // with msg.message == UWM_USER_EVENT, msg.wParam == work_proc::Exec
        // (function pointer) and msg.lParam == work_proc context. We must
        // dispatch this inline because DispatchMessageW skips thread messages.
        constexpr UINT kUserEvent = WM_USER + 100;

        MSG msg{};
        for (;;) {
            BOOL got = ::GetMessageW(&msg, NULL, 0, 0);
            if (got == 0) break;          // WM_QUIT
            if (got == -1) break;         // GetMessage failed
            if (msg.hwnd == NULL && msg.message == kUserEvent) {
                using ExecFn = void (*)(void*) noexcept;
                ExecFn exec = reinterpret_cast<ExecFn>(msg.wParam);
                if (exec != nullptr) {
                    exec(reinterpret_cast<void*>(msg.lParam));
                }
                continue;
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        client_log_line("viewer: pump exited");
        return 0;
    }
};

}  // namespace GKC

#include "base/GkcDef.cpp"
#include "base/GkcSAMain.cpp"
#include "base/GkcGui.cpp"
