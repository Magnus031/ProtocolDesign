#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "base/GkcDef.h"
#include "base/GkcGui.h"
#include "src/client/client.h"
#include "src/client/pixel_renderer.h"
#include "src/common/input_event.h"

namespace {

constexpr int kWindowWidth = 32;
constexpr int kWindowHeight = 16;
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
    for (uintptr i = 0; i < args.GetCount(); ++i) {
        const std::string arg = to_std_string(args[i]);
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
        if (!Create(true, kWindowWidth, kWindowHeight)) return false;
        Show(true);

        if (!options_.offline) {
            ClientRuntime::Options runtime_options;
            runtime_options.gateway_host = options_.gateway_host;
            runtime_options.gateway_port = options_.gateway_port;
            runtime_options.app_name = options_.app_name;
            runtime_options.connect_gateway = true;
            runtime_.start(runtime_options, &renderer_,
                           [this]() { repaint_work_.PostWork(); });
        }
        return true;
    }

    void DoDraw(GKC::UiMessageDraw* pDraw) noexcept {
        if (pDraw == nullptr) return;
        renderer_.blit(pDraw->pBuffer, pDraw->iWidth, pDraw->iHeight,
                       pDraw->rcPaint);
    }

    void DoMouse(GKC::UiMessageMouse* pMouse) noexcept {
        if (pMouse == nullptr) return;
        const InputEventType type = mouse_event_type(*pMouse);
        runtime_.send_input_event(
            pack_mouse_input_event(type, pMouse->x, pMouse->y, timestamp_us()));

        if (type == InputEventType::MOUSE_LEFT_DOWN &&
            pMouse->x >= kSmallLeft && pMouse->x < kSmallRight &&
            pMouse->y >= kSmallTop && pMouse->y < kSmallBottom) {
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
        GKC::GuiHelper::Quit();
    }

    void DamageFull() noexcept {
        GKC::UiRect r;
        r.Set(0, 0, kWindowWidth, kWindowHeight);
        Damage(r);
    }

private:
    ViewerOptions options_;
    PixelRenderer renderer_;
    ClientRuntime runtime_;
    RepaintWork repaint_work_;
    bool yellow_rect_ = false;
    bool cyan_background_ = false;
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
        return GuiHelper::Loop();
    }
};

}  // namespace GKC

#include "base/GkcDef.cpp"
#include "base/GkcSAMain.cpp"
#include "base/GkcGui.cpp"
