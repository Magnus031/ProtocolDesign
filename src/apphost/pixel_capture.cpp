#include "src/apphost/pixel_capture.h"
#include <cstring>

std::vector<uint8_t> capture_rect(const uint8_t* pixels, int stride_pixels,
                                   int left, int top, int right, int bottom) {
    // left <= x < right
    // top <= y < bottom                               
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
