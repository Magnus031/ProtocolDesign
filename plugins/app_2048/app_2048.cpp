// app_2048 — server-side 2048 game plugin loaded by AppHost via dlopen.
//
// Canvas:
//   * 160 x 200 ARGB logical canvas (matches docs/design/2048.md).
//   * Layout:
//       y =   0 ..  24  header        (logo "2048" + current score)
//       y =  28 .. 172  board         (4 x 4 tiles, 36 px pitch, origin x=8)
//       y = 176 .. 194  bottom strip  (RESET button + GAME OVER indicator)
//
// Game state lives entirely in app_2048::Board (plugins/app_2048/app_2048_logic.*).
// The plugin only reads the board during DoDraw and forwards input events to
// Board::move / Board::reset in DoKeyboard / DoMouse.  After any state change
// we call Damage(full window) so AppHost re-paints and emits a single
// PIXEL_DATA frame.
//
// Inputs:
//   * KEY_DOWN KB_Left / KB_Right / KB_Up / KB_Down  → Board::move(direction)
//   * KEY_DOWN KB_F2                                 → do_reset()
//   * MOUSE_LEFT_DOWN inside RESET button rect       → do_reset()
//
// F2 and the RESET button click both call the same do_reset() helper; this is
// the "calls the same reset path as F2 at the application boundary" contract
// from docs/design/2048.md.
//
// Merge highlight: Board::merged_this_turn(x,y) is set on the tile produced by
// a merge during the most recent move, and cleared at the start of the next
// move().  We render those tiles using a brighter accent palette for that one
// frame, matching the rule in docs/design/2048.md "Color Rules".

#include <cstdint>
#include <cstdio>
#include <random>

#include "base/GkcDef.h"
#include "base/GkcGui.h"

#include "plugins/app_2048/app_2048_logic.h"
#include "plugins/app_2048/app_2048_view.h"

namespace {

// ── Layout constants ──────────────────────────────────────────────────────
//
// Canvas size and reset-button rect come from plugins/app_2048/app_2048_view.h
// so the integration test can hit the same coordinates the plugin draws.

using app_2048::kCanvasW;
using app_2048::kCanvasH;
using app_2048::kResetButtonLeft;
using app_2048::kResetButtonTop;
using app_2048::kResetButtonRight;
using app_2048::kResetButtonBottom;

constexpr int kHeaderH = 24;

// Board: 4 tiles of 36 px → 144 px wide. Centred horizontally with x=8.
constexpr int kBoardOriginX = 8;
constexpr int kBoardOriginY = 28;
constexpr int kTilePitch    = 36;
constexpr int kTileInset    = 2;   // gap from cell edge to tile colour fill

// Game-over indicator on the right side of the bottom strip, separate from
// the reset button on the left.  Same vertical band so the two share the
// "controls strip" visually.
constexpr int kGameOverLeft   = 80;
constexpr int kGameOverTop    = kResetButtonTop;
constexpr int kGameOverRight  = kCanvasW - 4;
constexpr int kGameOverBottom = kResetButtonBottom;

// 5x7 bitmap font for the digits 0..9.  One bit per pixel, MSB on the left
// of each row, low 5 bits used.  Designed for readability at display-scale 4
// where the digit becomes 20x28 on screen.
constexpr int kGlyphW = 5;
constexpr int kGlyphH = 7;
constexpr int kGlyphSpacing = 1;

constexpr uint8_t kDigitGlyphs[10][kGlyphH] = {
    // 0
    {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110},
    // 1
    {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110},
    // 2
    {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111},
    // 3
    {0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110},
    // 4
    {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010},
    // 5
    {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110},
    // 6
    {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110},
    // 7
    {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000},
    // 8
    {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110},
    // 9
    {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100},
};

// 5x7 bitmaps for the letters used by static labels in the UI: "RESET" on
// the button and "GAME OVER!" in the game-over indicator.  Kept as a small
// hand-rolled set instead of pulling in a font system; new labels need new
// glyphs added here.
constexpr uint8_t kLetterR[kGlyphH] = {
    0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001,
};
constexpr uint8_t kLetterE[kGlyphH] = {
    0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111,
};
constexpr uint8_t kLetterS[kGlyphH] = {
    0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110,
};
constexpr uint8_t kLetterT[kGlyphH] = {
    0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100,
};
constexpr uint8_t kLetterG[kGlyphH] = {
    0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01110,
};
constexpr uint8_t kLetterA[kGlyphH] = {
    0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001,
};
constexpr uint8_t kLetterM[kGlyphH] = {
    0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001,
};
constexpr uint8_t kLetterO[kGlyphH] = {
    0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110,
};
constexpr uint8_t kLetterV[kGlyphH] = {
    0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100,
};
constexpr uint8_t kBangGlyph[kGlyphH] = {
    0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100,
};
// All-zero glyph used to render an inter-word space.  Drawing it is a no-op
// but it advances the cursor by one glyph width plus the usual spacing.
constexpr uint8_t kSpaceGlyph[kGlyphH] = {
    0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000,
};

// ── Colour palette ─────────────────────────────────────────────────────────

constexpr color_quad kColorBackground   = COLOR_QUAD_MAKE( 28,  36,  44, 255);
constexpr color_quad kColorBoardBorder  = COLOR_QUAD_MAKE( 60,  68,  78, 255);
constexpr color_quad kColorEmptyCell    = COLOR_QUAD_MAKE( 80,  90, 100, 255);
constexpr color_quad kColorTextDark     = COLOR_QUAD_MAKE( 30,  30,  30, 255);
constexpr color_quad kColorTextLight    = COLOR_QUAD_MAKE(245, 245, 245, 255);
constexpr color_quad kColorScore        = COLOR_QUAD_MAKE(245, 245, 245, 255);
constexpr color_quad kColorLogo         = COLOR_QUAD_MAKE(255, 200,  80, 255);
constexpr color_quad kColorGameOver     = COLOR_QUAD_MAKE(200,  40,  40, 255);
constexpr color_quad kColorButton       = COLOR_QUAD_MAKE( 80, 130, 180, 255);
constexpr color_quad kColorButtonBorder = COLOR_QUAD_MAKE(140, 180, 220, 255);
constexpr color_quad kColorButtonText   = COLOR_QUAD_MAKE(245, 245, 245, 255);

// Tile background colours by value.  Anything above 4096 reuses the highest
// entry; the spec doesn't require unique colours past that.
struct TilePalette {
    color_quad fill;
    color_quad merge_fill;
    color_quad text;
};

TilePalette palette_for(uint32_t value) {
    switch (value) {
    case 2:    return { COLOR_QUAD_MAKE(238, 228, 218, 255),
                        COLOR_QUAD_MAKE(255, 255, 130, 255),
                        kColorTextDark };
    case 4:    return { COLOR_QUAD_MAKE(237, 224, 200, 255),
                        COLOR_QUAD_MAKE(255, 240, 100, 255),
                        kColorTextDark };
    case 8:    return { COLOR_QUAD_MAKE(242, 177, 121, 255),
                        COLOR_QUAD_ORANGE,
                        kColorTextLight };
    case 16:   return { COLOR_QUAD_MAKE(245, 149,  99, 255),
                        COLOR_QUAD_ORANGE,
                        kColorTextLight };
    case 32:   return { COLOR_QUAD_MAKE(246, 124,  95, 255),
                        COLOR_QUAD_ORANGE,
                        kColorTextLight };
    case 64:   return { COLOR_QUAD_MAKE(246,  94,  59, 255),
                        COLOR_QUAD_HOT_PINK,
                        kColorTextLight };
    case 128:  return { COLOR_QUAD_MAKE(237, 207, 114, 255),
                        COLOR_QUAD_HOT_PINK,
                        kColorTextLight };
    case 256:  return { COLOR_QUAD_MAKE(237, 204,  97, 255),
                        COLOR_QUAD_HOT_PINK,
                        kColorTextLight };
    case 512:  return { COLOR_QUAD_MAKE(237, 200,  80, 255),
                        COLOR_QUAD_PURPLE,
                        kColorTextLight };
    case 1024: return { COLOR_QUAD_MAKE(237, 197,  63, 255),
                        COLOR_QUAD_PURPLE,
                        kColorTextLight };
    default:   return { COLOR_QUAD_MAKE(237, 194,  46, 255),
                        COLOR_QUAD_PURPLE,
                        kColorTextLight };
    }
}

// ── Pixel helpers ──────────────────────────────────────────────────────────

inline void put_pixel(color_quad* buf, int stride, int x, int y, color_quad c) {
    if (x < 0 || y < 0 || x >= stride || y >= kCanvasH) return;
    buf[y * stride + x] = c;
}

void fill_rect(color_quad* buf, int stride,
               int left, int top, int right, int bottom, color_quad c) {
    if (left   < 0)        left   = 0;
    if (top    < 0)        top    = 0;
    if (right  > stride)   right  = stride;
    if (bottom > kCanvasH) bottom = kCanvasH;
    for (int y = top; y < bottom; ++y) {
        color_quad* row = buf + y * stride;
        for (int x = left; x < right; ++x)
            row[x] = c;
    }
}

// Draw a single digit glyph at (x, y).  Pixels of the glyph use `fg`; the
// background is left untouched so the glyph composes over an arbitrary tile
// fill.
void draw_digit(color_quad* buf, int stride,
                int x, int y, int digit, color_quad fg) {
    if (digit < 0 || digit > 9) return;
    for (int row = 0; row < kGlyphH; ++row) {
        const uint8_t bits = kDigitGlyphs[digit][row];
        for (int col = 0; col < kGlyphW; ++col) {
            if (bits & (1u << (kGlyphW - 1 - col)))
                put_pixel(buf, stride, x + col, y + row, fg);
        }
    }
}

// Render `value` at the given top-left position.  Returns the width in pixels
// actually consumed (digits-only, no trailing space).  If `value == 0` we
// still draw a single "0" so callers can pass arbitrary numbers.
int draw_number(color_quad* buf, int stride,
                int x, int y, uint32_t value, color_quad fg) {
    // Decompose into digits (most significant first).
    int digits[10];
    int n = 0;
    if (value == 0) {
        digits[n++] = 0;
    } else {
        while (value > 0 && n < 10) {
            digits[n++] = static_cast<int>(value % 10);
            value /= 10;
        }
    }
    int w = 0;
    for (int i = n - 1; i >= 0; --i) {
        draw_digit(buf, stride, x + w, y, digits[i], fg);
        w += kGlyphW;
        if (i > 0) w += kGlyphSpacing;
    }
    return w;
}

int number_width(uint32_t value) {
    int n = 0;
    if (value == 0) {
        n = 1;
    } else {
        while (value > 0) { ++n; value /= 10; }
    }
    return n * kGlyphW + (n - 1) * kGlyphSpacing;
}

// Draw an arbitrary 5x7 glyph (digit or letter) at (x, y).  Used for the
// "RESET" button label so we don't need a generic font system.
void draw_glyph(color_quad* buf, int stride,
                int x, int y, const uint8_t glyph[kGlyphH], color_quad fg) {
    for (int row = 0; row < kGlyphH; ++row) {
        const uint8_t bits = glyph[row];
        for (int col = 0; col < kGlyphW; ++col) {
            if (bits & (1u << (kGlyphW - 1 - col)))
                put_pixel(buf, stride, x + col, y + row, fg);
        }
    }
}

// Draw the literal string "RESET" left-to-right starting at (x, y).  Returns
// the width consumed in pixels.  No general-purpose text engine — this is
// the only string the plugin renders.
int draw_reset_label(color_quad* buf, int stride, int x, int y, color_quad fg) {
    const uint8_t* glyphs[5] = {
        kLetterR, kLetterE, kLetterS, kLetterE, kLetterT,
    };
    int w = 0;
    for (int i = 0; i < 5; ++i) {
        draw_glyph(buf, stride, x + w, y, glyphs[i], fg);
        w += kGlyphW;
        if (i + 1 < 5) w += kGlyphSpacing;
    }
    return w;
}

constexpr int kResetLabelWidth = 5 * kGlyphW + 4 * kGlyphSpacing;

// Draw the literal string "GAME OVER!" left-to-right starting at (x, y).
// Returns the width consumed in pixels.  The space between "GAME" and "OVER!"
// is rendered as a blank glyph so the inter-letter cadence stays uniform.
int draw_game_over_label(color_quad* buf, int stride,
                         int x, int y, color_quad fg) {
    const uint8_t* glyphs[10] = {
        kLetterG, kLetterA, kLetterM, kLetterE,
        kSpaceGlyph,
        kLetterO, kLetterV, kLetterE, kLetterR, kBangGlyph,
    };
    int w = 0;
    for (int i = 0; i < 10; ++i) {
        draw_glyph(buf, stride, x + w, y, glyphs[i], fg);
        w += kGlyphW;
        if (i + 1 < 10) w += kGlyphSpacing;
    }
    return w;
}

constexpr int kGameOverLabelWidth = 10 * kGlyphW + 9 * kGlyphSpacing;

// ── Window ─────────────────────────────────────────────────────────────────

class App2048Window : public GKC::ToplevelImpl<App2048Window> {
public:
    void init(uint64_t initial_seed) {
        board_.reset(initial_seed);
        // Use random_device (not the game seed) for subsequent F2 resets so
        // the player gets a fresh layout each restart.  rng_ only feeds
        // reset() seeds; the actual gameplay rng lives inside Board.
        std::random_device rd;
        rng_.seed((static_cast<uint64_t>(rd()) << 32) | rd());
    }

    void DoDraw(GKC::UiMessageDraw* pDraw) noexcept {
        if (pDraw == nullptr || pDraw->pBuffer == nullptr) return;
        const int stride = pDraw->iWidth;

        // Clear the full canvas to the background colour.  The dirty rect
        // (rcPaint) may be smaller, but on every state change we Damage the
        // full window so painting outside rcPaint is wasted work, not wrong.
        fill_rect(pDraw->pBuffer, stride, 0, 0, kCanvasW, kCanvasH,
                  kColorBackground);

        draw_header(pDraw->pBuffer, stride);
        draw_board(pDraw->pBuffer, stride);
        draw_bottom_strip(pDraw->pBuffer, stride);
    }

    void DoKeyboard(GKC::UiMessageKeyboard* pKb) noexcept {
        if (pKb == nullptr) return;
        if (pKb->btDown != 1) return;  // KEY_DOWN only

        bool changed = false;
        switch (pKb->btKey) {
        case 0x25 /* KB_Left  */: changed = board_.move(app_2048::Direction::Left ).changed; break;
        case 0x26 /* KB_Up    */: changed = board_.move(app_2048::Direction::Up   ).changed; break;
        case 0x27 /* KB_Right */: changed = board_.move(app_2048::Direction::Right).changed; break;
        case 0x28 /* KB_Down  */: changed = board_.move(app_2048::Direction::Down ).changed; break;
        case 0x71 /* KB_F2    */:
            do_reset();
            return;  // do_reset already issues Damage(full).
        default:
            return;
        }

        if (changed) damage_full();
    }

    // RESET button handling.  We trigger on MOUSE_LEFT_DOWN inside the button
    // rect (per docs/design/2048.md "Reset Button" — preferred trigger).
    // Other mouse events (move, up, right-click, clicks outside the button)
    // are intentionally dropped without Damage so they don't generate
    // spurious frames; tile clicks are explicitly out-of-scope for MVP.
    void DoMouse(GKC::UiMessageMouse* pMouse) noexcept {
        if (pMouse == nullptr) return;
        if (pMouse->uEvent  != MOUSE_EVENT_DOWN)  return;
        if (pMouse->btButton != MOUSE_BUTTON_LEFT) return;
        if (!app_2048::point_in_reset_button(pMouse->x, pMouse->y)) return;
        do_reset();
    }

    void DoClose() noexcept { GKC::GuiHelper::Quit(); }

private:
    void draw_header(color_quad* buf, int stride) {
        // "2048" logo in the top-left corner.
        const int logo_x = 4;
        const int logo_y = (kHeaderH - kGlyphH) / 2;
        draw_number(buf, stride, logo_x, logo_y, 2048, kColorLogo);

        // Score, right-aligned at x = kCanvasW - 4.
        const uint32_t score = board_.score();
        const int score_w = number_width(score);
        const int score_x = kCanvasW - 4 - score_w;
        const int score_y = (kHeaderH - kGlyphH) / 2;
        draw_number(buf, stride, score_x, score_y, score, kColorScore);
    }

    void draw_board(color_quad* buf, int stride) {
        const int board_size = kTilePitch * app_2048::kBoardSize;

        // Outer board frame: a 1-pixel-thick border that visually separates
        // the playfield from the background.  The interior is left as the
        // background colour and is then over-painted by the cell rectangles.
        fill_rect(buf, stride,
                  kBoardOriginX - 1,
                  kBoardOriginY - 1,
                  kBoardOriginX + board_size + 1,
                  kBoardOriginY + board_size + 1,
                  kColorBoardBorder);

        for (int by = 0; by < app_2048::kBoardSize; ++by) {
            for (int bx = 0; bx < app_2048::kBoardSize; ++bx) {
                draw_cell(buf, stride, bx, by);
            }
        }
    }

    void draw_cell(color_quad* buf, int stride, int bx, int by) {
        const int cell_left = kBoardOriginX + bx * kTilePitch;
        const int cell_top  = kBoardOriginY + by * kTilePitch;
        const int tile_left   = cell_left + kTileInset;
        const int tile_top    = cell_top  + kTileInset;
        const int tile_right  = cell_left + kTilePitch - kTileInset;
        const int tile_bottom = cell_top  + kTilePitch - kTileInset;

        const uint32_t value = board_.at(bx, by);
        if (value == 0) {
            fill_rect(buf, stride, tile_left, tile_top, tile_right, tile_bottom,
                      kColorEmptyCell);
            return;
        }

        const TilePalette pal = palette_for(value);
        const color_quad fill = board_.merged_this_turn(bx, by) ? pal.merge_fill
                                                                 : pal.fill;
        fill_rect(buf, stride, tile_left, tile_top, tile_right, tile_bottom, fill);

        // Centre the value's bitmap inside the tile.
        const int text_w = number_width(value);
        const int text_x = tile_left + ((tile_right - tile_left) - text_w) / 2;
        const int text_y = tile_top  + ((tile_bottom - tile_top) - kGlyphH) / 2;
        draw_number(buf, stride, text_x, text_y, value, pal.text);
    }

    void draw_bottom_strip(color_quad* buf, int stride) {
        // RESET button on the left.  Drawn unconditionally so the user can
        // see and click it from the very first frame.
        const int btn_w = kResetButtonRight  - kResetButtonLeft;
        const int btn_h = kResetButtonBottom - kResetButtonTop;

        // 1-pixel button border so the click target is visually distinct.
        fill_rect(buf, stride,
                  kResetButtonLeft  - 1, kResetButtonTop    - 1,
                  kResetButtonRight + 1, kResetButtonBottom + 1,
                  kColorButtonBorder);
        fill_rect(buf, stride,
                  kResetButtonLeft, kResetButtonTop,
                  kResetButtonRight, kResetButtonBottom,
                  kColorButton);

        // Centre the "RESET" label inside the button.
        const int label_x = kResetButtonLeft + (btn_w - kResetLabelWidth) / 2;
        const int label_y = kResetButtonTop  + (btn_h - kGlyphH) / 2;
        draw_reset_label(buf, stride, label_x, label_y, kColorButtonText);

        // Game-over indicator on the right side of the strip.  When the
        // board reaches GameOver we render the literal text "GAME OVER!"
        // in the accent colour, centred inside the indicator rect.  The
        // rect itself is left transparent (background colour) so the text
        // reads as a status message rather than a coloured banner —
        // matches the spec line "GAME OVER 或等价的简单状态提示".
        if (board_.state() == app_2048::GameState::GameOver) {
            const int rect_w = kGameOverRight  - kGameOverLeft;
            const int rect_h = kGameOverBottom - kGameOverTop;
            const int label_x = kGameOverLeft + (rect_w - kGameOverLabelWidth) / 2;
            const int label_y = kGameOverTop  + (rect_h - kGlyphH) / 2;
            draw_game_over_label(buf, stride, label_x, label_y, kColorGameOver);
        }
    }

    // Reset path shared by KB_F2 and RESET-button click.  Draws a fresh seed
    // from rng_ so successive resets diverge, then issues Damage(full) so the
    // next frame reflects the cleared board and zeroed score.
    void do_reset() {
        std::uniform_int_distribution<uint64_t> pick;
        board_.reset(pick(rng_));
        damage_full();
    }

    void damage_full() {
        GKC::UiRect r;
        r.Set(0, 0, kCanvasW, kCanvasH);
        Damage(r);
    }

    app_2048::Board   board_{0};
    std::mt19937_64   rng_;
};

}  // namespace

namespace GKC {

class ProgramEntryPoint {
public:
    static int GuiMain(const ConstArray<ConstStringS>& /*args*/) {
        if (g_ui_host.GetFunc().IsNull()) {
            std::fprintf(stderr,
                         "app_2048: g_ui_host not injected (link mismatch?)\n");
            return 99;
        }

        App2048Window win;
        // Seed once for the whole session.  Mixing random_device twice into a
        // 64-bit value gives Board's mt19937_64 a full-width seed instead of
        // the 32 bits std::random_device returns directly.
        std::random_device rd;
        const uint64_t seed = (static_cast<uint64_t>(rd()) << 32) | rd();
        win.init(seed);

        if (!win.Create(true, kCanvasW, kCanvasH))
            return 1;

        // Show(true) triggers the initial full-frame DRAW; the freshly-reset
        // board has its two starting tiles already, so the first PIXEL_DATA
        // frame already contains a playable layout.
        win.Show(true);

        return GuiHelper::Loop();
    }
};

}  // namespace GKC

// Pull GKC plugin-side translation units into this .so so the plugin gets
// its own GKC::g_ui_host storage and exports an extern "C" _SA_UIMain symbol.
// Mirrors plugins/demo_app/demo_app.cpp.  AppHost itself MUST NOT link these.
#include "base/GkcDef.cpp"
#include "base/GkcSAMain.cpp"
#include "base/GkcGui.cpp"
