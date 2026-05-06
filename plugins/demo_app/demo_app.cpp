// demo_app — minimal SA UI plugin for M4/M5 verification.
//
// Window content:
//   * 32 x 16 ARGB canvas
//   * Full window background: BLUE  (M5 toggles BLUE <-> CYAN on KB_F1 down)
//   * Small rect at (8, 4) .. (16, 12):
//       - first DoDraw from Show(true): RED   (M4 startup, do not change)
//       - DoDraw from initial Damage(small): GREEN (M4 startup, do not change)
//       - M5 left-click inside the small rect: GREEN <-> YELLOW toggle
//
// Lifecycle:
//   1. GuiMain asserts that g_ui_host has been injected by the host.
//   2. Create + Show(true)              -> initial full-frame DRAW (RED rect).
//   3. Damage(small_rect)               -> dirty-frame DRAW (GREEN rect).
//   4. GuiHelper::Loop()                -> blocks until host calls Quit
//                                          (CLOSE_SESSION) or input arrives.
//   5. M5 inputs arrive via PostWork on the main thread:
//      - left button down inside small rect: toggle small_color_,
//                                            Damage(small) -> dirty PIXEL_DATA
//      - KB_F1 key down: toggle bg_color_, Damage(full) -> full-window
//                                            PIXEL_DATA
//
// Note: the M4 startup sequence is deliberately preserved (RED then GREEN
// rect on Show / first Damage).  M5 only takes over once the plugin enters
// GuiHelper::Loop() and starts servicing input.  M4 regression tests must
// keep passing without modification.

#include <cstdio>

#include "base/GkcDef.h"
#include "base/GkcGui.h"

namespace {

constexpr int kWindowWidth  = 32;
constexpr int kWindowHeight = 16;

constexpr int kSmallLeft   = 8;
constexpr int kSmallTop    = 4;
constexpr int kSmallRight  = 16;
constexpr int kSmallBottom = 12;

class DemoWindow : public GKC::ToplevelImpl<DemoWindow> {
public:
    void DoDraw(GKC::UiMessageDraw* pDraw) noexcept {
        ++draw_count_;

        // Choose colours based on phase:
        //   draw 1: M4 full-frame after Show(true) — small rect RED.
        //   draw 2: M4 dirty-rect from initial Damage(small) — small rect GREEN.
        //   draw 3+: input-driven repaints (M5) — use the latched toggles.
        const color_quad bg = (draw_count_ <= 2) ? COLOR_QUAD_BLUE : bg_color_;
        color_quad small;
        if (draw_count_ == 1) {
            small = COLOR_QUAD_RED;
        } else if (draw_count_ == 2) {
            small = COLOR_QUAD_GREEN;
            // Latch the post-startup small rect colour so the first input
            // toggle moves between GREEN and YELLOW deterministically.
            small_color_ = COLOR_QUAD_GREEN;
        } else {
            small = small_color_;
        }

        for (int y = 0; y < pDraw->iHeight; ++y) {
            for (int x = 0; x < pDraw->iWidth; ++x) {
                color_quad c = bg;
                if (x >= kSmallLeft && x < kSmallRight &&
                    y >= kSmallTop  && y < kSmallBottom) {
                    c = small;
                }
                pDraw->pBuffer[y * pDraw->iWidth + x] = c;
            }
        }
    }

    // Left-button-down inside the small rect toggles its colour between
    // GREEN and YELLOW and submits a dirty-rect Damage.  Other mouse events
    // (move, right button, wheel, clicks outside the small rect) are NOT
    // turned into Damage() calls, so they don't generate spurious frames —
    // the M5 MouseOutsideTargetNoFrame / NoInputNoPixelData expectations
    // depend on this.
    void DoMouse(GKC::UiMessageMouse* pMouse) noexcept {
        if (pMouse == nullptr) return;
        if (pMouse->uEvent != MOUSE_EVENT_DOWN) return;
        if (pMouse->btButton != MOUSE_BUTTON_LEFT) return;
        if (pMouse->x < kSmallLeft || pMouse->x >= kSmallRight) return;
        if (pMouse->y < kSmallTop  || pMouse->y >= kSmallBottom) return;

        small_color_ = (small_color_ == COLOR_QUAD_GREEN) ? COLOR_QUAD_YELLOW
                                                         : COLOR_QUAD_GREEN;
        GKC::UiRect r;
        r.Set(kSmallLeft, kSmallTop, kSmallRight, kSmallBottom);
        Damage(r);
    }

    // KB_F1 key-down toggles the background colour between BLUE and CYAN
    // and Damages the full window.  KB_F1 key-up and any other key are
    // ignored — they don't cause repaints.
    void DoKeyboard(GKC::UiMessageKeyboard* pKb) noexcept {
        if (pKb == nullptr) return;
        if (pKb->btDown != 1) return;
        if (pKb->btKey  != 0x70 /* KB_F1 */) return;

        bg_color_ = (bg_color_ == COLOR_QUAD_BLUE) ? COLOR_QUAD_CYAN
                                                   : COLOR_QUAD_BLUE;
        GKC::UiRect r;
        r.Set(0, 0, kWindowWidth, kWindowHeight);
        Damage(r);
    }

    void DoClose() noexcept { GKC::GuiHelper::Quit(); }

private:
    int        draw_count_  = 0;
    // Latched colours used on draw_count_ >= 3 (M5 input-driven repaints).
    color_quad bg_color_    = COLOR_QUAD_BLUE;
    color_quad small_color_ = COLOR_QUAD_GREEN;
};

}  // namespace

namespace GKC {

class ProgramEntryPoint {
public:
    static int GuiMain(const ConstArray<ConstStringS>& /*args*/) {
        if (g_ui_host.GetFunc().IsNull()) {
            std::fprintf(stderr, "demo_app: g_ui_host not injected (link mismatch?)\n");
            return 99;
        }

        DemoWindow win;
        if (!win.Create(true, kWindowWidth, kWindowHeight))
            return 1;

        win.Show(true);  // initial full-frame DRAW (small rect RED)

        UiRect small;
        small.Set(kSmallLeft, kSmallTop, kSmallRight, kSmallBottom);
        win.Damage(small);  // initial dirty-rect DRAW (small rect GREEN)

        return GuiHelper::Loop();
    }
};

}  // namespace GKC

// Pull GKC's plugin-side translation units into this .so so that:
//   - GKC::g_ui_host has storage local to this plugin
//   - extern "C" _SA_UIMain is exported from this plugin
// Order mirrors third_party/GKC/util/gui/LocalDesk/src/Main.cpp.
#include "base/GkcDef.cpp"
#include "base/GkcSAMain.cpp"
#include "base/GkcGui.cpp"
