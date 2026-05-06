#include "src/client/pixel_renderer.h"

#include <algorithm>
#include <cstring>

namespace {

constexpr size_t kPixelDataHeaderSize = 24;

uint32_t read_be32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
            static_cast<uint32_t>(p[3]);
}

color_quad argb_to_color_quad(const uint8_t* p) noexcept {
    const uint8_t a = p[0];
    const uint8_t r = p[1];
    const uint8_t g = p[2];
    const uint8_t b = p[3];
    return COLOR_QUAD_MAKE(r, g, b, a);
}

}  // namespace

PixelRenderer::PixelRenderer() {
    reset(32, 16, COLOR_QUAD_BLUE);
}

PixelRenderer::PixelRenderer(int width, int height) {
    reset(width, height, COLOR_QUAD_BLACK);
}

void PixelRenderer::reset(int width, int height, color_quad fill) {
    if (width <= 0) width = 1;
    if (height <= 0) height = 1;

    std::lock_guard<std::mutex> lock(mutex_);
    width_ = width;
    height_ = height;
    last_frame_seq_ = 0;
    pixels_.assign(static_cast<size_t>(width_) * static_cast<size_t>(height_),
                   fill);
}

PixelRenderer::ApplyResult PixelRenderer::apply_dirty_rect(
    uint32_t frame_seq, const GKC::UiRect& rect, const uint8_t* pixels,
    size_t len) {
    if (pixels == nullptr) return ApplyResult::BAD_LENGTH;

    std::lock_guard<std::mutex> lock(mutex_);

    if (frame_seq < last_frame_seq_) return ApplyResult::STALE_FRAME;
    if (rect.L() < 0 || rect.T() < 0 || rect.R() > width_ ||
        rect.B() > height_ || rect.IsEmpty()) {
        return ApplyResult::BAD_RECT;
    }

    const int rect_width = rect.Width();
    const int rect_height = rect.Height();
    const size_t expected =
        static_cast<size_t>(rect_width) * static_cast<size_t>(rect_height) * 4u;
    if (len != expected) return ApplyResult::BAD_LENGTH;

    const uint8_t* src = pixels;
    for (int y = rect.T(); y < rect.B(); ++y) {
        color_quad* dst =
            pixels_.data() + static_cast<size_t>(y) * width_ + rect.L();
        for (int x = 0; x < rect_width; ++x) {
            dst[x] = argb_to_color_quad(src);
            src += 4;
        }
    }

    last_frame_seq_ = frame_seq;
    return ApplyResult::OK;
}

PixelRenderer::ApplyResult PixelRenderer::apply_pixel_data_body(
    const uint8_t* body, size_t len) {
    if (body == nullptr || len < kPixelDataHeaderSize)
        return ApplyResult::BAD_LENGTH;

    const uint32_t frame_seq = read_be32(body);
    GKC::UiRect rect;
    rect.Set(static_cast<int>(read_be32(body + 4)),
             static_cast<int>(read_be32(body + 8)),
             static_cast<int>(read_be32(body + 12)),
             static_cast<int>(read_be32(body + 16)));
    const uint32_t data_len = read_be32(body + 20);
    if (static_cast<size_t>(data_len) != len - kPixelDataHeaderSize)
        return ApplyResult::BAD_LENGTH;

    return apply_dirty_rect(frame_seq, rect, body + kPixelDataHeaderSize,
                            data_len);
}

void PixelRenderer::blit(color_quad* dst, int dst_width, int dst_height,
                         const GKC::UiRect& paint) const {
    if (dst == nullptr || dst_width <= 0 || dst_height <= 0) return;

    std::lock_guard<std::mutex> lock(mutex_);
    GKC::UiRect clipped;
    if (!clip_rect(paint, std::min(width_, dst_width),
                   std::min(height_, dst_height), clipped)) {
        return;
    }

    const int copy_width = clipped.Width();
    for (int y = clipped.T(); y < clipped.B(); ++y) {
        const color_quad* src =
            pixels_.data() + static_cast<size_t>(y) * width_ + clipped.L();
        color_quad* row =
            dst + static_cast<size_t>(y) * dst_width + clipped.L();
        std::memcpy(row, src, static_cast<size_t>(copy_width) * sizeof(color_quad));
    }
}

void PixelRenderer::paint_demo(bool yellow_rect, bool cyan_background) {
    std::lock_guard<std::mutex> lock(mutex_);
    const color_quad bg = cyan_background ? COLOR_QUAD_CYAN : COLOR_QUAD_BLUE;
    std::fill(pixels_.begin(), pixels_.end(), bg);

    const int left = std::min(8, width_);
    const int top = std::min(4, height_);
    const int right = std::min(16, width_);
    const int bottom = std::min(12, height_);
    const color_quad rect_color = yellow_rect ? COLOR_QUAD_YELLOW
                                              : COLOR_QUAD_GREEN;
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            pixels_[static_cast<size_t>(y) * width_ + x] = rect_color;
        }
    }
    ++last_frame_seq_;
}

int PixelRenderer::width() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return width_;
}

int PixelRenderer::height() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return height_;
}

uint32_t PixelRenderer::last_frame_seq() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_frame_seq_;
}

bool PixelRenderer::clip_rect(const GKC::UiRect& in, int width, int height,
                              GKC::UiRect& out) {
    out.Set(std::max(0, in.L()), std::max(0, in.T()),
            std::min(width, in.R()), std::min(height, in.B()));
    return !out.IsEmpty();
}
