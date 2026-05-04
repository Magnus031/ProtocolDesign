#include "src/apphost/pixel_capture.h"

#include "src/common/byte_order.h"

#include <cstring>

std::vector<uint8_t> capture_rect(const uint8_t* pixels, int stride_pixels,
                                   int left, int top, int right, int bottom) {
    // left <= x < right
    // top  <= y < bottom
    const int w = right - left;
    const int h = bottom - top;
    std::vector<uint8_t> result(static_cast<size_t>(w * h * 4));
    for (int row = 0; row < h; ++row) {
        const uint8_t* src = pixels + static_cast<size_t>((top + row) * stride_pixels + left) * 4;
        uint8_t*       dst = result.data() + static_cast<size_t>(row * w) * 4;
        std::memcpy(dst, src, static_cast<size_t>(w) * 4);
    }
    return result;
}

namespace {

inline void write_be32(uint8_t* dst, uint32_t v) noexcept {
    const uint32_t be = BE32(v);
    std::memcpy(dst, &be, sizeof(be));
}

}  // namespace

std::vector<uint8_t> pack_pixel_data_rect(const uint32_t* pBuffer,
                                          int stride_pixels,
                                          int left, int top,
                                          int right, int bottom,
                                          uint32_t frame_seq) {
    const int      w        = right - left;
    const int      h        = bottom - top;
    const uint32_t data_len = static_cast<uint32_t>(w) * static_cast<uint32_t>(h) * 4u;
    const size_t   header_fields_bytes = 6u * sizeof(uint32_t);

    std::vector<uint8_t> body(header_fields_bytes + data_len);

    write_be32(body.data() +  0, frame_seq);
    write_be32(body.data() +  4, static_cast<uint32_t>(left));
    write_be32(body.data() +  8, static_cast<uint32_t>(top));
    write_be32(body.data() + 12, static_cast<uint32_t>(right));
    write_be32(body.data() + 16, static_cast<uint32_t>(bottom));
    write_be32(body.data() + 20, data_len);

    uint8_t* dst = body.data() + header_fields_bytes;
    for (int row = 0; row < h; ++row) {
        const uint32_t* src_row = pBuffer + (top + row) * stride_pixels + left;
        for (int col = 0; col < w; ++col) {
            // GKC color_quad layout: bit31..0 = a r g b
            //   → little-endian bytes in memory: B, G, R, A
            // Protocol wire (PIXEL_DATA pixelData): [A, R, G, B] per pixel.
            // Equivalent to writing the color_quad in big-endian byte order.
            const uint32_t cq = src_row[col];
            write_be32(dst, cq);
            dst += 4;
        }
    }
    return body;
}
