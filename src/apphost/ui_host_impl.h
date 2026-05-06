#pragma once

#include "base/GkcDef.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

// UiHostImpl — headless fake implementation of GKC::IUiHost for AppHost.
//
// Important boundary:
//   - AppHost does NOT link or run GKC's real GUI host implementation
//     (Wayland / Win32 windows, compositor integration, native event loop).
//   - AppHost DOES provide the same IUiHost ABI that a GKC plugin expects.
//     The ABI is a C-style function table plus an opaque context pointer,
//     not a C++ inheritance hierarchy.
//   - The plugin still uses GKC's wrapper classes in GkcGui.h
//     (ToplevelImpl<T>, GuiHelper::Loop(), Show(), Damage(), etc.).  Those
//     wrappers are thin calls through the injected g_ui_host function table.
//
// Plugins compiled against GKC see a normal IUiHost contract: they call
// CreateToplevel / Show / Damage / Loop / Quit / PostWork.  We translate
// those calls into:
//   - in-memory window pixel buffers (no real Wayland/X11 surface)
//   - synchronous UI_MESSAGE_DRAW dispatch back to the plugin
//   - PIXEL_DATA wire packets handed off to a sink supplied by AppHost
//
// Threading model (M4):
//   - Plugin runs on the main thread.  CreateToplevel / Show / Damage /
//     Loop / DoDraw / pixel send all happen on that thread, synchronously.
//   - request_quit() is the only cross-thread entry point: AppHost's IoPool
//     thread calls it from the CLOSE_SESSION handler to wake up Loop().
//   - PostWork() is supported as a synchronous queue drained inside Loop()
//     (so demo plugins or GKC internals that happen to enqueue work do not
//     deadlock).  M5 will drive INPUT_EVENT injection through the same path.
//
// Window model (M4):
//   - Exactly one Toplevel window per AppHost process.  Dialogs and popups
//     return 0 (unsupported).
//   - Window backing store is std::vector<color_quad> sized to width*height.
class UiHostImpl {
public:
    // Sink invoked once per dispatched UI_MESSAGE_DRAW with the finished
    // PIXEL_DATA Body bytes (header is the caller's responsibility).
    using PixelDataSink = std::function<void(std::vector<uint8_t> body)>;

    UiHostImpl();
    ~UiHostImpl();

    UiHostImpl(const UiHostImpl&)            = delete;
    UiHostImpl& operator=(const UiHostImpl&) = delete;

    // Install the pixel-data sink.  Must be called before the plugin
    // invokes Show(true) / Damage() so the first frame can be delivered.
    void set_pixel_data_sink(PixelDataSink sink);

    // Build the bridge passed to the plugin's _SA_UIMain entry point.
    //
    // LcInterface<IUiHost> carries:
    //   - context: this UiHostImpl instance
    //   - function table: kHostTable in ui_host_impl.cpp
    //
    // _SA_UIMain stores that pair into the plugin-local GKC::g_ui_host.
    // After that, plugin-side GKC wrappers such as win.Create() call
    // g_ui_host.GetFunc()->CreateToplevel(g_ui_host.GetContext(), ...),
    // which lands back in our on_host_create_toplevel(this, ...).
    GKC::LcInterface<GKC::IUiHost> make_interface();

    // Wake up Loop() from any thread.  Idempotent.
    void request_quit();

    // Post an input event from any thread (typically the AppHost reader
    // thread) to be dispatched on the plugin main thread.
    //
    // The event is *copied by value* into an internal owns-data queue
    // before this function returns, so callers may pass stack-local
    // GKC::UiMessageMouse / GKC::UiMessageKeyboard objects safely.
    //
    // The first event after a quiet period schedules a single PostWork
    // drain.  Subsequent events that arrive before the drain runs share
    // that same PostWork — we never enqueue a second one while one is
    // pending.  This keeps the WorkProc count bounded under bursts.
    //
    // Plugin DoMouse/DoKeyboard runs on the main thread inside
    // on_host_loop().  If the window has not been created yet, or has
    // already been destroyed, or has no handler installed, the event is
    // silently dropped.  M5 must NOT crash on input that arrives outside
    // a fully-wired plugin window.
    void post_mouse_input(const GKC::UiMessageMouse& msg);
    void post_keyboard_input(const GKC::UiMessageKeyboard& msg);

    // State is the implementation struct.  It is declared public so the
    // file-static IUiHost dispatch callbacks in ui_host_impl.cpp can name
    // its nested types directly.  External code does not depend on it.
    struct State;

private:
    // WorkProc trampoline used by post_mouse_input / post_keyboard_input
    // to schedule drain_input_queue_on_main_thread() onto the main thread
    // through PostWork.  The first argument is the UiHostImpl*.
    static void drain_input_work_static(void* self) noexcept;

    // Main-thread drain of the input queue.  Swaps the pending events into
    // a local container before releasing the state mutex, then dispatches
    // each one — plugin DoDraw triggered by Damage() inside the dispatch
    // path must not see the host mutex held.
    void drain_input_queue_on_main_thread();

    // Synchronous dispatch into the plugin's UiMessageHandler.  Must only
    // be called from the main thread (i.e. from drain_input_queue_on_main_
    // thread()).  Drops events when the window has no handler yet.
    void dispatch_mouse(const GKC::UiMessageMouse& msg) noexcept;
    void dispatch_keyboard(const GKC::UiMessageKeyboard& msg) noexcept;

    std::unique_ptr<State> state_;

    // UiHostImplCallbacks is a "friend bridge": it grants the 15 or so
    // free-standing callback functions in ui_host_impl.cpp (on_window_show,
    // on_host_loop, dispatch_draw_and_send, etc.) indirect access to the
    // otherwise-private state_ member.
    //
    // Why this pattern?
    //   - GKC's IUiHost ABI is a C function table dispatched with a void* ctx.
    //     The callbacks are ordinary file-static functions, not member
    //     functions — they receive ctx (== UiHostImpl*), but cannot touch
    //     state_ because it is private.
    //   - Adding a separate friend declaration for every single callback
    //     would be noisy and fragile.  Instead we friend a single struct
    //     that exposes one static method:
    //
    //         struct UiHostImplCallbacks {
    //             static State* state_of(UiHostImpl* host) {
    //                 return host->state_.get();
    //             }
    //         };
    //
    //   - The anonymous-namespace helpers state_of(void* ctx) and
    //     host_from_ctx(void* ctx) wrap this bridge so every callback can
    //     go from void* → State* in one call.
    //
    // Consequence: UiHostImplCallbacks is the ONE place that needs friend
    // access.  Nothing else outside this class can reach state_.
    friend struct UiHostImplCallbacks;
};
