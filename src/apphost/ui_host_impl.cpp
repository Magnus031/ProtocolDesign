// Standard library headers MUST be included before GKC headers.
// GKC opens `#pragma pack(push, 1)` in several segments and sloppy use can
// leak the packing state into STL containers (deque, condition_variable),
// producing baffling _Alloc_traits / _Deque_base template errors.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "src/apphost/pixel_capture.h"
#include "src/apphost/ui_host_impl.h"

#include "base/GkcDef.h"

using GKC::IUiDialog;
using GKC::IUiHost;
using GKC::IUiPopup;
using GKC::IUiToplevel;
using GKC::UiMessageDraw;
using GKC::UiMessageHandler;
using GKC::UiRect;
using GKC::UiSize;
using GKC::WorkProc;

// ── Internal state ──────────────────────────────────────────────────────────

struct UiHostImpl::State {
    // The single window we support in M4.  nullptr until CreateToplevel.
    struct WindowRuntime {
        UiHostImpl*           host        = nullptr;
        int                   width       = 0;
        int                   height      = 0;
        std::vector<uint32_t> pixels;          // GKC color_quad backing buffer
        UiMessageHandler      handler{nullptr};
        void*                 handler_ctx = nullptr;
        bool                  destroyed   = false;
    };

    std::unique_ptr<WindowRuntime> window;

    // Cross-thread coordination for Loop / Quit / PostWork.
    struct PostedWork {
        WorkProc work;
        void*    data;
    };
    std::mutex                    mtx;
    std::condition_variable       cv;
    bool                          quit_flag = false;
    std::deque<PostedWork>        post_queue;

    // Owns-data input queue (M5).  AppHost reader thread copies parsed
    // GKC::UiMessageMouse / UiMessageKeyboard values into this queue, then
    // schedules a single drain_input_work_static onto post_queue.  Sharing
    // one pending WorkProc across a burst keeps post_queue from growing
    // 1:1 with input events.
    struct InputEvent {
        enum class Kind : uint8_t { Mouse, Keyboard };
        Kind                       kind = Kind::Mouse;
        GKC::UiMessageMouse        mouse{};
        GKC::UiMessageKeyboard     keyboard{};
    };
    std::deque<InputEvent>        input_queue;
    bool                          input_work_pending = false;

    // Pixel sink: receives finished PIXEL_DATA Body bytes per frame.
    PixelDataSink                 sink;

    // Frame sequence counter, monotonic over the AppHost's lifetime.
    std::atomic<uint32_t>         frame_seq{0};
};

// The GKC ABI stores callbacks as plain C function pointers, not C++ member
// functions.  Those callbacks cannot directly access UiHostImpl::state_, so
// this small friend bridge lets the file-local callback functions recover the
// AppHost object state from the opaque context pointer.
struct UiHostImplCallbacks {
    static UiHostImpl::State* state_of(UiHostImpl* host) {
        return host->state_.get();
    }
};

namespace {

inline UiHostImpl* host_from_ctx(void* ctx) noexcept {
    // This cast is the inverse of UiHostImpl::make_interface(), which stores
    // `this` as the LcInterface context pointer.  Every IUiHost callback
    // receives that pointer as its first argument, so plain C callbacks can
    // regain the AppHost-side UiHostImpl instance.
    return static_cast<UiHostImpl*>(ctx);
}
inline UiHostImpl::State* state_of(void* ctx) noexcept {
    return UiHostImplCallbacks::state_of(host_from_ctx(ctx));
}
inline UiHostImpl::State::WindowRuntime* window_from_id(uintptr id) noexcept {
    return reinterpret_cast<UiHostImpl::State::WindowRuntime*>(id);
}

UiRect make_rect(int l, int t, int r, int b) noexcept {
    UiRect rc;
    rc.Set(l, t, r, b);
    return rc;
}

// Synchronous DRAW dispatch + PIXEL_DATA emission.
//
// Call direction for Show(true) / Damage(rect):
//   plugin GKC wrapper
//     -> g_ui_host.GetFunc()->Show/Damage(...)
//     -> on_window_show/on_window_damage(...)
//     -> dispatch_draw_and_send(...)
//     -> plugin message handler Process(UI_MESSAGE_DRAW)
//     -> plugin DemoWindow::DoDraw(pDraw)
//     -> pack pDraw->pBuffer/pDraw->rcPaint into PIXEL_DATA
//
// The draw callback intentionally runs before packing.  The plugin owns the
// business rendering logic; AppHost owns the backing buffer and the protocol
// conversion.
void dispatch_draw_and_send(UiHostImpl::State::WindowRuntime* w,
                            int left, int top, int right, int bottom) noexcept {
    if (w->destroyed) return;
    if (w->handler.Process == nullptr) return;  // no SetMessageHandler yet

    UiMessageDraw msg;
    // color_quad lives in the global namespace (typedef in ui_types.h);
    // GKC::ColorQuad is the in-namespace alias.  Use the unqualified name.
    msg.pBuffer = reinterpret_cast<color_quad*>(w->pixels.data());
    msg.iWidth  = w->width;
    msg.iHeight = w->height;
    msg.rcPaint = make_rect(left, top, right, bottom);

    w->handler.Process(w->handler_ctx, UI_MESSAGE_DRAW,
                       reinterpret_cast<uintptr>(&msg));

    auto* s = UiHostImplCallbacks::state_of(w->host);
    if (!s->sink) return;

    const uint32_t seq = s->frame_seq.fetch_add(1) + 1u;
    auto body = pack_pixel_data_rect(w->pixels.data(), w->width,
                                     left, top, right, bottom, seq);
    s->sink(std::move(body));
}

// ── IUiWindow callbacks ────────────────────────────────────────────────────

void on_window_get_size(uintptr id, UiSize& size) noexcept {
    auto* w = window_from_id(id);
    size.Set(w->width, w->height);
}

void on_window_show(uintptr id, bool bShow) noexcept {
    auto* w = window_from_id(id);
    if (!bShow) return;
    // First-frame paint: full window rect.  GKC's _WindowImpl::Show calls
    // SetMessageHandler before this, so the plugin handler is already wired.
    dispatch_draw_and_send(w, 0, 0, w->width, w->height);
}

void on_window_set_message_handler(uintptr id, const UiMessageHandler& handler,
                                   void* pContext) noexcept {
    auto* w = window_from_id(id);
    w->handler     = handler;
    w->handler_ctx = pContext;
}

void on_window_set_close(uintptr id) noexcept {
    auto* w = window_from_id(id);
    if (w->handler.Process != nullptr)
        w->handler.Process(w->handler_ctx, UI_MESSAGE_CLOSE, 0);
}

void on_window_damage(uintptr id, const UiRect& rc) noexcept {
    auto* w = window_from_id(id);
    int l = rc.L(), t = rc.T(), r = rc.R(), b = rc.B();
    if (l < 0)         l = 0;
    if (t < 0)         t = 0;
    if (r > w->width)  r = w->width;
    if (b > w->height) b = w->height;
    if (l >= r || t >= b) return;
    dispatch_draw_and_send(w, l, t, r, b);
}

// ── IUiToplevel callbacks (size hints — stubs in M4) ───────────────────────

void on_toplevel_set_min_max(uintptr, const UiSize&, const UiSize&) noexcept {}
void on_toplevel_set_size(uintptr, const UiSize&) noexcept {}

// ── IUiHost callbacks ──────────────────────────────────────────────────────

int on_host_loop(void* ctx) noexcept {
    auto* s = state_of(ctx);
    while (true) {
        std::unique_lock<std::mutex> lk(s->mtx);
        s->cv.wait(lk, [&]{ return s->quit_flag || !s->post_queue.empty(); });

        // Drain posted work, releasing the lock while running each item.
        while (!s->post_queue.empty()) {
            auto item = s->post_queue.front();
            s->post_queue.pop_front();
            lk.unlock();
            if (item.work.Exec != nullptr) item.work.Exec(item.data);
            lk.lock();
        }
        if (s->quit_flag) break;
    }
    return 0;
}

void on_host_quit(void* ctx) noexcept {
    auto* s = state_of(ctx);
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        s->quit_flag = true;
    }
    s->cv.notify_all();
}

void on_host_post_work(void* ctx, const WorkProc& work, void* pData) noexcept {
    auto* s = state_of(ctx);
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        s->post_queue.push_back({work, pData});
    }
    s->cv.notify_all();
}

// AddTimer is a stub: demo plugin doesn't use timers in M4.
uintptr on_host_add_timer(void*, int, const WorkProc&, void*) noexcept { return 0; }
void    on_host_remove_timer(void*, uintptr) noexcept {}

uintptr on_host_create_toplevel(void* ctx, bool /*bResizable*/,
                                int iWidth, int iHeight,
                                IUiToplevel** ppInterface) noexcept;
uintptr on_host_create_dialog(void*, uintptr, bool, int, int, IUiDialog** pp) noexcept {
    if (pp != nullptr) *pp = nullptr;
    return 0;
}
uintptr on_host_create_popup(void*, uintptr, int, int, int, int, IUiPopup** pp) noexcept {
    if (pp != nullptr) *pp = nullptr;
    return 0;
}

void on_host_destroy(void* ctx, uintptr idWindow) noexcept {
    auto* s = state_of(ctx);
    if (s->window && reinterpret_cast<uintptr>(s->window.get()) == idWindow) {
        s->window->destroyed = true;
        s->window.reset();
    }
}

// ── Static dispatch tables ─────────────────────────────────────────────────
//
// These tables are the AppHost implementation of the GKC UI ABI.  They are
// what the plugin receives through GKC::g_ui_host after _SA_UIMain injection.
// No real GKC GUI host is involved on the AppHost side.
//
// Example:
//   plugin win.Create(...)
//     -> GKC::_Toplevel::Create()
//     -> g_ui_host.GetFunc()->CreateToplevel(g_ui_host.GetContext(), ...)
//     -> kHostTable.CreateToplevel(context, ...)
//     -> on_host_create_toplevel(static_cast<UiHostImpl*>(context), ...)

const IUiToplevel kToplevelTable = {
    /* base */ {
        &on_window_get_size,
        &on_window_show,
        &on_window_set_message_handler,
        &on_window_set_close,
        &on_window_damage,
    },
    &on_toplevel_set_min_max,
    &on_toplevel_set_size,
};

// Top-level host function table.  The first argument to every callback is the
// opaque context pointer stored by make_interface(); in this file that pointer
// is always the AppHost-owned UiHostImpl instance.
const IUiHost kHostTable = {
    &on_host_loop,
    &on_host_quit,
    &on_host_post_work,
    &on_host_add_timer,
    &on_host_remove_timer,
    &on_host_create_toplevel,
    &on_host_create_dialog,
    &on_host_create_popup,
    &on_host_destroy,
};

uintptr on_host_create_toplevel(void* ctx, bool /*bResizable*/,
                                int iWidth, int iHeight,
                                IUiToplevel** ppInterface) noexcept {
    auto* host = host_from_ctx(ctx);
    auto* s    = UiHostImplCallbacks::state_of(host);
    if (s->window != nullptr) {
        // M4 supports a single toplevel per AppHost.
        if (ppInterface != nullptr) *ppInterface = nullptr;
        return 0;
    }
    auto w = std::make_unique<UiHostImpl::State::WindowRuntime>();
    w->host   = host;
    w->width  = iWidth;
    w->height = iHeight;
    w->pixels.assign(static_cast<size_t>(iWidth) * static_cast<size_t>(iHeight), 0u);

    if (ppInterface != nullptr)
        *ppInterface = const_cast<IUiToplevel*>(&kToplevelTable);
    const uintptr id = reinterpret_cast<uintptr>(w.get());
    s->window = std::move(w);
    return id;
}

}  // namespace

// ── UiHostImpl public API ──────────────────────────────────────────────────

UiHostImpl::UiHostImpl() : state_(std::make_unique<State>()) {}
UiHostImpl::~UiHostImpl() = default;

void UiHostImpl::set_pixel_data_sink(PixelDataSink sink) {
    state_->sink = std::move(sink);
}

GKC::LcInterface<IUiHost> UiHostImpl::make_interface() {
    // This is the only object passed from AppHost into the plugin before
    // plugin code starts running:
    //
    //   context = this
    //   funcs   = &kHostTable
    //
    // GKC's _SA_UIMain stores this pair in the plugin-local GKC::g_ui_host.
    // Later, plugin-side wrappers call through that stored pair.  For example:
    //
    //   GuiHelper::Loop()
    //     -> g_ui_host.GetFunc()->Loop(g_ui_host.GetContext())
    //     -> on_host_loop(this)
    //
    // This is why the AppHost can provide a fake/headless UI host while the
    // plugin code still looks like normal GKC GUI code.
    return GKC::LcInterface<IUiHost>(this, GKC::RefPtr<IUiHost>(&kHostTable));
}

void UiHostImpl::request_quit() {
    on_host_quit(this);
}

void UiHostImpl::post_mouse_input(const GKC::UiMessageMouse& msg) {
    bool should_post = false;
    {
        std::lock_guard<std::mutex> lk(state_->mtx);
        State::InputEvent ev{};
        ev.kind  = State::InputEvent::Kind::Mouse;
        ev.mouse = msg;  // value copy; caller's stack object may disappear.
        state_->input_queue.push_back(ev);
        if (!state_->input_work_pending) {
            state_->input_work_pending = true;
            should_post = true;
        }
    }
    if (should_post)
        on_host_post_work(this, WorkProc{&UiHostImpl::drain_input_work_static}, this);
}

void UiHostImpl::post_keyboard_input(const GKC::UiMessageKeyboard& msg) {
    bool should_post = false;
    {
        std::lock_guard<std::mutex> lk(state_->mtx);
        State::InputEvent ev{};
        ev.kind     = State::InputEvent::Kind::Keyboard;
        ev.keyboard = msg;  // value copy.
        state_->input_queue.push_back(ev);
        if (!state_->input_work_pending) {
            state_->input_work_pending = true;
            should_post = true;
        }
    }
    if (should_post)
        on_host_post_work(this, WorkProc{&UiHostImpl::drain_input_work_static}, this);
}

void UiHostImpl::drain_input_work_static(void* self) noexcept {
    static_cast<UiHostImpl*>(self)->drain_input_queue_on_main_thread();
}

void UiHostImpl::drain_input_queue_on_main_thread() {
    // Snapshot under the lock so plugin DoMouse/DoKeyboard (which may call
    // Damage and trigger synchronous DoDraw + PIXEL_DATA send) runs without
    // any host-side mutex held.
    std::deque<State::InputEvent> local;
    {
        std::lock_guard<std::mutex> lk(state_->mtx);
        std::swap(local, state_->input_queue);
        state_->input_work_pending = false;
    }

    while (!local.empty()) {
        const State::InputEvent ev = local.front();
        local.pop_front();
        if (ev.kind == State::InputEvent::Kind::Mouse)
            dispatch_mouse(ev.mouse);
        else
            dispatch_keyboard(ev.keyboard);
    }
}

void UiHostImpl::dispatch_mouse(const GKC::UiMessageMouse& msg) noexcept {
    auto* w = state_->window.get();
    if (w == nullptr || w->destroyed) return;
    if (w->handler.Process == nullptr) return;
    w->handler.Process(w->handler_ctx, UI_MESSAGE_MOUSE,
                       reinterpret_cast<uintptr>(&msg));
}

void UiHostImpl::dispatch_keyboard(const GKC::UiMessageKeyboard& msg) noexcept {
    auto* w = state_->window.get();
    if (w == nullptr || w->destroyed) return;
    if (w->handler.Process == nullptr) return;
    w->handler.Process(w->handler_ctx, UI_MESSAGE_KEYBOARD,
                       reinterpret_cast<uintptr>(&msg));
}
