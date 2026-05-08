#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "base/GkcDef.h"

// PixelRenderer owns the Client-side backing store shown by client_viewer.
// It accepts protocol PIXEL_DATA dirty rectangles in ARGB wire order and blits
// the current frame into GKC's color_quad draw buffer.
class PixelRenderer {
public:
    enum class ApplyResult {
        OK,
        STALE_FRAME,
        BAD_RECT,
        BAD_LENGTH,
    };

    PixelRenderer();
    PixelRenderer(int width, int height);

    // Resizes the backing store and fills it with `fill`.
    void reset(int width, int height, color_quad fill = COLOR_QUAD_BLACK);

    // Applies one dirty rectangle.  `pixels` must contain width * height * 4
    // bytes in ARGB order.  Older frame sequence numbers are rejected.
    ApplyResult apply_dirty_rect(uint32_t frame_seq, const GKC::UiRect& rect,
                                 const uint8_t* pixels, size_t len);

    // Parses and applies a PIXEL_DATA body.
    ApplyResult apply_pixel_data_body(const uint8_t* body, size_t len);

    // Copies the requested paint rectangle into GKC's draw buffer.
    void blit(color_quad* dst, int dst_width, int dst_height,
              const GKC::UiRect& paint) const;

    // Copies the current frame into a larger or smaller destination while
    // preserving nearest-neighbour pixel edges.
    void blit_scaled_to_fit(color_quad* dst, int dst_width, int dst_height,
                            const GKC::UiRect& paint) const;

    // Paints the local M6a demonstration frame used before M6b networking is
    // connected.
    void paint_demo(bool yellow_rect, bool cyan_background);

    int width() const;
    int height() const;
    uint32_t last_frame_seq() const;

private:
    static bool clip_rect(const GKC::UiRect& in, int width, int height,
                          GKC::UiRect& out);

    mutable std::mutex mutex_;
    int width_ = 0;
    int height_ = 0;
    uint32_t last_frame_seq_ = 0;
    std::vector<color_quad> pixels_;
};
