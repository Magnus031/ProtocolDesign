#pragma once
#include <cstdint>
#include <vector>

// Extract a rectangular sub-region from an ARGB pixel buffer.
//
// pixels        : raw ARGB bytes, row-major, 4 bytes per pixel in [A, R, G, B] order.
// stride_pixels : width of the full buffer in pixels (bytes per row = stride_pixels * 4).
// left, top     : inclusive top-left pixel coordinate of the desired rectangle.
// right, bottom : exclusive bottom-right boundary of the desired rectangle.
//                 The captured coordinate ranges are x in [left, right) and
//                 y in [top, bottom), so pixels at x == right or y == bottom
//                 are not included.
//
// Example: left=1, top=0, right=3, bottom=2 captures these pixels:
//   row 0: (1,0), (2,0)
//   row 1: (1,1), (2,1)
// The captured rectangle is 2 pixels wide and 2 pixels high.
//
// Returns the sub-rect pixels as a new buffer in the same ARGB row-major format.
// The caller is responsible for ensuring the rect lies within the buffer bounds.
std::vector<uint8_t> capture_rect(const uint8_t* pixels, int stride_pixels,
                                   int left, int top, int right, int bottom);
