#include "src/client/pixel_renderer.h"

#include <vector>

#include "gtest/gtest.h"

namespace {

void append_be32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

std::vector<uint8_t> pixel_data_body(uint32_t frame_seq, int l, int t, int r,
                                     int b,
                                     const std::vector<uint8_t>& pixels) {
    std::vector<uint8_t> body;
    append_be32(body, frame_seq);
    append_be32(body, static_cast<uint32_t>(l));
    append_be32(body, static_cast<uint32_t>(t));
    append_be32(body, static_cast<uint32_t>(r));
    append_be32(body, static_cast<uint32_t>(b));
    append_be32(body, static_cast<uint32_t>(pixels.size()));
    body.insert(body.end(), pixels.begin(), pixels.end());
    return body;
}

}  // namespace

TEST(PixelRendererTest, AppliesDirtyRectAndBlits) {
    PixelRenderer renderer(4, 4);
    GKC::UiRect rect;
    rect.Set(1, 1, 3, 3);
    const std::vector<uint8_t> argb = {
        255, 255, 0, 0, 255, 0, 255, 0,
        255, 0, 0, 255, 255, 255, 255, 0,
    };

    EXPECT_EQ(PixelRenderer::ApplyResult::OK,
              renderer.apply_dirty_rect(1, rect, argb.data(), argb.size()));

    std::vector<color_quad> out(16, COLOR_QUAD_BLACK);
    GKC::UiRect full;
    full.Set(0, 0, 4, 4);
    renderer.blit(out.data(), 4, 4, full);

    EXPECT_EQ(COLOR_QUAD_RED, out[5]);
    EXPECT_EQ(COLOR_QUAD_GREEN, out[6]);
    EXPECT_EQ(COLOR_QUAD_BLUE, out[9]);
    EXPECT_EQ(COLOR_QUAD_YELLOW, out[10]);
}

TEST(PixelRendererTest, RejectsStaleFrame) {
    PixelRenderer renderer(2, 2);
    GKC::UiRect rect;
    rect.Set(0, 0, 1, 1);
    const std::vector<uint8_t> argb = {255, 255, 0, 0};

    EXPECT_EQ(PixelRenderer::ApplyResult::OK,
              renderer.apply_dirty_rect(2, rect, argb.data(), argb.size()));
    EXPECT_EQ(PixelRenderer::ApplyResult::STALE_FRAME,
              renderer.apply_dirty_rect(1, rect, argb.data(), argb.size()));
}

TEST(PixelRendererTest, RejectsBadLengthAndRect) {
    PixelRenderer renderer(2, 2);
    GKC::UiRect rect;
    rect.Set(0, 0, 2, 2);
    const std::vector<uint8_t> short_argb = {255, 255, 0, 0};

    EXPECT_EQ(PixelRenderer::ApplyResult::BAD_LENGTH,
              renderer.apply_dirty_rect(1, rect, short_argb.data(),
                                        short_argb.size()));

    rect.Set(1, 1, 3, 3);
    const std::vector<uint8_t> enough(16, 0xFF);
    EXPECT_EQ(PixelRenderer::ApplyResult::BAD_RECT,
              renderer.apply_dirty_rect(1, rect, enough.data(), enough.size()));
}

TEST(PixelRendererTest, ParsesPixelDataBody) {
    PixelRenderer renderer(2, 2);
    const std::vector<uint8_t> argb = {255, 0, 255, 0};
    const std::vector<uint8_t> body = pixel_data_body(7, 1, 1, 2, 2, argb);

    EXPECT_EQ(PixelRenderer::ApplyResult::OK,
              renderer.apply_pixel_data_body(body.data(), body.size()));
    EXPECT_EQ(7u, renderer.last_frame_seq());
}

TEST(PixelRendererTest, BlitsScaledFrame) {
    PixelRenderer renderer(2, 2);
    GKC::UiRect rect;
    rect.Set(0, 0, 2, 2);
    const std::vector<uint8_t> argb = {
        255, 255, 0, 0, 255, 0, 255, 0,
        255, 0, 0, 255, 255, 255, 255, 0,
    };

    EXPECT_EQ(PixelRenderer::ApplyResult::OK,
              renderer.apply_dirty_rect(1, rect, argb.data(), argb.size()));

    std::vector<color_quad> out(16, COLOR_QUAD_BLACK);
    GKC::UiRect full;
    full.Set(0, 0, 4, 4);
    renderer.blit_scaled_to_fit(out.data(), 4, 4, full);

    EXPECT_EQ(COLOR_QUAD_RED, out[0]);
    EXPECT_EQ(COLOR_QUAD_RED, out[5]);
    EXPECT_EQ(COLOR_QUAD_GREEN, out[2]);
    EXPECT_EQ(COLOR_QUAD_BLUE, out[8]);
    EXPECT_EQ(COLOR_QUAD_YELLOW, out[15]);
}
