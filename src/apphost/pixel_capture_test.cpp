#include "src/apphost/pixel_capture.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

// Build a single GKC-style color_quad value with bit31..0 = a r g b.
constexpr uint32_t make_color_quad(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) <<  8) |
            static_cast<uint32_t>(b);
}

uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) <<  8) |
            static_cast<uint32_t>(p[3]);
}

}  // namespace

// capture_rect: byte-level cropping, no format conversion. Same contract
// as M2: source is a tight ARGB-wire buffer and the output is the same.
TEST(CaptureRect, ByteOrderPreservedAndRectExtractedCorrectly) {
    // 3x2 buffer with each pixel encoded as [A, R, G, B] wire bytes.
    // Pixel (x,y) has A=0x80, R=x, G=y, B=0x10.
    const int W = 3, H = 2;
    std::vector<uint8_t> buf(W * H * 4);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint8_t* p = buf.data() + (y * W + x) * 4;
            p[0] = 0x80;                          // A
            p[1] = static_cast<uint8_t>(x);       // R
            p[2] = static_cast<uint8_t>(y);       // G
            p[3] = 0x10;                          // B
        }
    }
    // Crop x in [1,3) y in [0,2): two columns wide, two rows tall.
    auto out = capture_rect(buf.data(), W, 1, 0, 3, 2);
    ASSERT_EQ(out.size(), 2u * 2u * 4u);

    // Expected pixels in row-major order: (1,0), (2,0), (1,1), (2,1).
    const uint8_t expected[] = {
        0x80, 1, 0, 0x10,
        0x80, 2, 0, 0x10,
        0x80, 1, 1, 0x10,
        0x80, 2, 1, 0x10,
    };
    EXPECT_EQ(0, std::memcmp(out.data(), expected, sizeof(expected)));
}

// pack_pixel_data_rect: builds the full PIXEL_DATA Body, with header fields
// in big-endian and pixel bytes in protocol wire order [A,R,G,B].
TEST(PackPixelDataRect, ConvertsBgraToArgbAndWritesBody) {
    // 4x3 color_quad buffer.  Fill the entire buffer with a known palette so
    // we can prove cropping picked the right cells.
    const int W = 4, H = 3;
    std::vector<uint32_t> buf(W * H);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            // R = 10*x, G = 20*y, B = 0xCC, A = 0xFF
            buf[y * W + x] = make_color_quad(
                static_cast<uint8_t>(10 * x),
                static_cast<uint8_t>(20 * y),
                0xCC, 0xFF);
        }
    }

    // Crop a 2x2 sub-rect: x in [1,3), y in [1,3).
    const int       left = 1, top = 1, right = 3, bottom = 3;
    const uint32_t  frame_seq = 0x01020304;
    auto body = pack_pixel_data_rect(buf.data(), W, left, top, right, bottom, frame_seq);

    // Body layout:
    //   24 bytes of metadata (6 BE uint32 fields)
    //   then 2*2*4 = 16 bytes of pixel data.
    ASSERT_EQ(body.size(), 24u + 16u);

    EXPECT_EQ(read_be32(body.data() +  0), frame_seq);
    EXPECT_EQ(read_be32(body.data() +  4), static_cast<uint32_t>(left));
    EXPECT_EQ(read_be32(body.data() +  8), static_cast<uint32_t>(top));
    EXPECT_EQ(read_be32(body.data() + 12), static_cast<uint32_t>(right));
    EXPECT_EQ(read_be32(body.data() + 16), static_cast<uint32_t>(bottom));
    EXPECT_EQ(read_be32(body.data() + 20), 16u);

    // Expected pixels in row-major: (1,1) (2,1) (1,2) (2,2).
    // make_color_quad(R=10x, G=20y, B=0xCC, A=0xFF) → on the wire [A,R,G,B].
    struct Px { uint8_t a, r, g, b; };
    const Px expected[] = {
        {0xFF, 10, 20, 0xCC},   // (1,1)
        {0xFF, 20, 20, 0xCC},   // (2,1)
        {0xFF, 10, 40, 0xCC},   // (1,2)
        {0xFF, 20, 40, 0xCC},   // (2,2)
    };
    const uint8_t* px = body.data() + 24;
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(px[i * 4 + 0], expected[i].a) << "pixel " << i << " A";
        EXPECT_EQ(px[i * 4 + 1], expected[i].r) << "pixel " << i << " R";
        EXPECT_EQ(px[i * 4 + 2], expected[i].g) << "pixel " << i << " G";
        EXPECT_EQ(px[i * 4 + 3], expected[i].b) << "pixel " << i << " B";
    }
}

// Sanity check: a rect that covers the full buffer and a single solid color
// produces a body whose pixel section is uniform.
TEST(PackPixelDataRect, FullRectSolidColorIsUniform) {
    const int W = 5, H = 4;
    const uint32_t blue = make_color_quad(0, 0, 0xFF, 0xFF);
    std::vector<uint32_t> buf(W * H, blue);

    auto body = pack_pixel_data_rect(buf.data(), W, 0, 0, W, H, /*frame_seq=*/1);
    ASSERT_EQ(body.size(), 24u + static_cast<size_t>(W * H) * 4u);

    EXPECT_EQ(read_be32(body.data() + 20), static_cast<uint32_t>(W * H * 4));

    const uint8_t* px = body.data() + 24;
    for (int i = 0; i < W * H; ++i) {
        EXPECT_EQ(px[i * 4 + 0], 0xFF);  // A
        EXPECT_EQ(px[i * 4 + 1], 0x00);  // R
        EXPECT_EQ(px[i * 4 + 2], 0x00);  // G
        EXPECT_EQ(px[i * 4 + 3], 0xFF);  // B
    }
}
