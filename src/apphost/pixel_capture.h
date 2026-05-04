#pragma once
#include <cstdint>
#include <vector>

// ── capture_rect ─────────────────────────────────────────────────────────────
// Plain byte-level rectangular crop. Kept from M2 for legacy/test callers and
// for cases where the source buffer is already in protocol wire format (ARGB
// big-endian: bytes [A, R, G, B] per pixel). Does NOT do any byte-order
// conversion — the caller owns format compatibility.
//
// pixels        : raw 4-bytes-per-pixel buffer, row-major.
// stride_pixels : width of the full buffer in pixels (bytes per row = stride_pixels * 4).
// left, top     : inclusive top-left coordinate.
// right, bottom : exclusive bottom-right boundary.
//                 Captured ranges are x in [left, right) and y in [top, bottom).
//
// Returns a tightly-packed (right-left) * (bottom-top) * 4 byte buffer.
std::vector<uint8_t> capture_rect(const uint8_t* pixels, int stride_pixels,
                                   int left, int top, int right, int bottom);

// ── pack_pixel_data_rect ─────────────────────────────────────────────────────
// Build a complete PIXEL_DATA Body from a GKC-style color_quad pixel buffer.
// Performs three steps in one pass:
//   1. Crop the rectangle [left,right) x [top,bottom) out of the full buffer.
//   2. Convert each pixel from GKC's in-memory layout (little-endian
//      color_quad = uint32_t with bit31..0 = a r g b → bytes B,G,R,A) to the
//      protocol's wire layout [A, R, G, B].
//   3. Lay out the PIXEL_DATA Body:
//        [frameSeq(4)] [rectLeft(4)] [rectTop(4)] [rectRight(4)]
//        [rectBottom(4)] [dataLen(4)] [pixelData...]
//      All uint32 fields are written big-endian (network order).
//
// pBuffer       : start of the full window pixel buffer (typed as uint32_t to
//                 avoid pulling GKC headers into pixel_capture; values are
//                 GKC color_quads — see byte-order note above).
// stride_pixels : full window width in pixels.
// left/top/right/bottom : inclusive/exclusive rectangle coordinates.
// frame_seq     : frame sequence number, written as the first body field.
//
// Returns the Body bytes only; the caller is responsible for prepending the
// 13-byte protocol Header with cmd_type=PIXEL_DATA and body_length=size().
std::vector<uint8_t> pack_pixel_data_rect(const uint32_t* pBuffer,
                                          int stride_pixels,
                                          int left, int top,
                                          int right, int bottom,
                                          uint32_t frame_seq);
