// demo_app — minimal SA UI plugin for M4 verification.
//
// Window content:
//   * 32 x 16 ARGB canvas
//   * Full window background: COLOR_QUAD_BLUE
//   * Small rect at (8, 4) .. (16, 12):
//       - first DoDraw from Show(true): COLOR_QUAD_RED
//       - later DoDraw calls from Damage(...): COLOR_QUAD_GREEN
//
// Lifecycle:
//   1. GuiMain asserts that g_ui_host has been injected by the host.
//   2. Create + Show(true)  → triggers the host's initial full-frame DRAW.
//   3. Damage(small_rect)   → triggers exactly one dirty-frame DRAW whose
//                              small rect differs from the first frame.
//   4. GuiHelper::Loop()    → blocks until host calls Quit (e.g. CLOSE_SESSION).
//
// Per the M4 design (docs/plan/process.md §4.2), the plugin links GKC's
// GkcGui.cpp / GkcSAMain.cpp into its own translation unit at the bottom
// of this file.  This gives the plugin .so its own GKC::g_ui_host storage
// and exports the extern "C" _SA_UIMain symbol for the host to dlsym.

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
    // Overrides _WindowImpl<T>::DoDraw.  Paints the full pattern every call
    // so that any rcPaint sub-rect submits the right pixels regardless of
    // whether the host dispatched a full-window or dirty-rect DRAW.
    //
    // Uses a call counter to vary the small-rect colour across frames:
    //   damage_count_ == 1 (first Draw, from Show)           → RED
    //   damage_count_ >= 2 (subsequent Draws, from Damage)   → GREEN
    // This lets the host-side test verify that dirty-rect
    // PIXEL_DATA carries different content from the initial
    // full-frame PIXEL_DATA.
    void DoDraw(GKC::UiMessageDraw* pDraw) noexcept {
        ++damage_count_;
        for (int y = 0; y < pDraw->iHeight; ++y) {
            for (int x = 0; x < pDraw->iWidth; ++x) {
                color_quad c = COLOR_QUAD_BLUE;
                if (x >= kSmallLeft && x < kSmallRight &&
                    y >= kSmallTop  && y < kSmallBottom) {
                    c = (damage_count_ == 1) ? COLOR_QUAD_RED
                                             : COLOR_QUAD_GREEN;
                }
                pDraw->pBuffer[y * pDraw->iWidth + x] = c;
            }
        }
    }

    void DoClose() noexcept { GKC::GuiHelper::Quit(); }

private:
    int damage_count_ = 0;
};

}  // namespace

namespace GKC {

// Plugin entry point invoked by GkcGui.cpp's _SA_UIMain shim after it has
// populated this plugin's local g_ui_host with the host-supplied LcInterface.
class ProgramEntryPoint {
public:
    static int GuiMain(const ConstArray<ConstStringS>& /*args*/) {
        // Link-topology probe: confirms the plugin's own g_ui_host was
        // populated by the host before GuiMain runs.  If this fires, the
        // plugin .so accidentally got its own copy of GkcGui.cpp linked
        // separately from the host or _SA_UIMain failed to assign.
        if (g_ui_host.GetFunc().IsNull()) {
            std::fprintf(stderr, "demo_app: g_ui_host not injected (link mismatch?)\n");
            return 99;
        }

        DemoWindow win;
        if (!win.Create(true, kWindowWidth, kWindowHeight))
            return 1;

        win.Show(true);  // → host dispatches initial full-frame DRAW

        UiRect small;
        small.Set(kSmallLeft, kSmallTop, kSmallRight, kSmallBottom);
        win.Damage(small);  // → host dispatches one dirty-rect DRAW

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
