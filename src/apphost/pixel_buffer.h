#pragma once
#include <cstdint>
#include <vector>
#include <cstddef>

// ARGB pixel buffer: row-major, 4 bytes per pixel in [A, R, G, B] wire order,
// matching the protocol spec's 0xAARRGGBB big-endian convention.
//
// For a 3x2 buffer, pixels_ stores bytes in this order:
//   row 0: (0,0) [A,R,G,B], (1,0) [A,R,G,B], (2,0) [A,R,G,B]
//   row 1: (0,1) [A,R,G,B], (1,1) [A,R,G,B], (2,1) [A,R,G,B]
//
// In general, pixel (x, y) starts at byte index:
//   ((y * width) + x) * 4
class PixelBuffer {
public:
    PixelBuffer(int width, int height);

    // Fill every pixel with a solid color. Arguments are r, g, b, a; stored
    // bytes use ARGB wire order [A, R, G, B].
    void fill_solid_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

    int    width()     const { return width_;  }
    int    height()    const { return height_; }

    // Raw pixel bytes, width * height * 4 bytes total, in [A, R, G, B] order per pixel.
    const uint8_t* data()      const { return pixels_.data(); }
    size_t         byte_size() const { return pixels_.size(); }

private:
    int                  width_;
    int                  height_;
    std::vector<uint8_t> pixels_;
};
