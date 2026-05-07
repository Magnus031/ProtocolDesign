#include "plugins/app_2048/app_2048_view.h"

#include <gtest/gtest.h>

namespace {

using app_2048::kResetButtonLeft;
using app_2048::kResetButtonTop;
using app_2048::kResetButtonRight;
using app_2048::kResetButtonBottom;
using app_2048::kResetButtonCenterX;
using app_2048::kResetButtonCenterY;
using app_2048::point_in_reset_button;

}  // namespace

// The reset-button hit-test must accept the centre point.  This is the
// coordinate the integration test (app_2048_input_test) clicks at, so a
// regression here would silently break the end-to-end mouse path.
TEST(ResetButtonHitTest, AcceptsCenter) {
    EXPECT_TRUE(point_in_reset_button(kResetButtonCenterX, kResetButtonCenterY));
}

// Top-left is inside (half-open in both axes).
TEST(ResetButtonHitTest, AcceptsTopLeftCorner) {
    EXPECT_TRUE(point_in_reset_button(kResetButtonLeft, kResetButtonTop));
}

// One-past-right and one-past-bottom are outside; this is the half-open
// rect contract.  A bug here would make right/bottom edges count as clicks
// while the rendered border is one pixel inside.
TEST(ResetButtonHitTest, ExcludesRightAndBottomEdges) {
    EXPECT_FALSE(point_in_reset_button(kResetButtonRight,  kResetButtonTop));
    EXPECT_FALSE(point_in_reset_button(kResetButtonLeft,   kResetButtonBottom));
    EXPECT_FALSE(point_in_reset_button(kResetButtonRight,  kResetButtonBottom));
}

// The interior just before the right/bottom edges is inside.
TEST(ResetButtonHitTest, AcceptsInteriorJustBeforeFarEdges) {
    EXPECT_TRUE(point_in_reset_button(kResetButtonRight  - 1,
                                      kResetButtonBottom - 1));
}

// Points far outside the button — including the board area, the header, and
// negative coordinates — must be rejected.  These guard against accidentally
// shifting the rect or dropping a coordinate-axis check.
TEST(ResetButtonHitTest, RejectsCanvasInteriorAwayFromButton) {
    EXPECT_FALSE(point_in_reset_button(0, 0));            // header / corner
    EXPECT_FALSE(point_in_reset_button(80, 100));         // mid-board
    EXPECT_FALSE(point_in_reset_button(140, 185));        // bottom-right strip
    EXPECT_FALSE(point_in_reset_button(-1, kResetButtonTop));
    EXPECT_FALSE(point_in_reset_button(kResetButtonLeft, -1));
}
