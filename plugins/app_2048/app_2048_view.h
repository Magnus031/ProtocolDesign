#pragma once

// View constants and pure-C++ hit-test for the 2048 plugin.
//
// Lives in a header separate from app_2048.cpp so unit tests can link the
// reset-button geometry without pulling in GKC/_SA_UIMain plumbing, and so
// the integration test (plugins/app_2048/app_2048_input_test.cpp) can reuse
// the canvas and button rect constants verbatim.

namespace app_2048 {

// Logical canvas (matches docs/design/2048.md "Window Layout").
constexpr int kCanvasW = 160;
constexpr int kCanvasH = 200;

// Reset button rect (canvas-local pixel coordinates, exclusive-right).  The
// values come from docs/design/2048.md "Reset Button" — keeping them as
// constants here lets tests click a stable point even if the surrounding
// layout shifts.
constexpr int kResetButtonLeft   = 16;
constexpr int kResetButtonTop    = 176;
constexpr int kResetButtonRight  = 72;
constexpr int kResetButtonBottom = 194;

// Stable button-centre coordinates exposed for tests.  Not used by the
// renderer.
constexpr int kResetButtonCenterX =
    (kResetButtonLeft + kResetButtonRight) / 2;
constexpr int kResetButtonCenterY =
    (kResetButtonTop + kResetButtonBottom) / 2;

// Returns true iff (x, y) is inside the reset button rect.  Half-open in
// both axes: top-left is inside, bottom-right is outside.  Mouse coordinates
// produced by the client/AppHost are already canvas-local.
constexpr bool point_in_reset_button(int x, int y) {
    return x >= kResetButtonLeft  && x < kResetButtonRight &&
           y >= kResetButtonTop   && y < kResetButtonBottom;
}

}  // namespace app_2048
