#include "src/apphost/pixel_buffer.h"

PixelBuffer::PixelBuffer(int width, int height)
    : width_(width), height_(height), pixels_(width * height * 4, 0)
{}

void PixelBuffer::fill_solid_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int i = 0; i < width_ * height_; ++i) {
        pixels_[i * 4 + 0] = a;  // wire byte[0] = A  (0xAARRGGBB big-endian)
        pixels_[i * 4 + 1] = r;  // wire byte[1] = R
        pixels_[i * 4 + 2] = g;  // wire byte[2] = G
        pixels_[i * 4 + 3] = b;  // wire byte[3] = B
    }
}
