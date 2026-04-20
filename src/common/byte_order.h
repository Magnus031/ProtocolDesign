#pragma once
// Byte-swap primitives and host↔network conversion macros.
// BE16/32/64 convert a value to/from big-endian (protocol wire format).
// LE16/32/64 convert a value to/from little-endian.
// Both sets are no-ops on the matching native byte order.
#include <cstdint>

#define BSWAP16(x) \
    ((uint16_t)(((uint16_t)(x) >> 8) | ((uint16_t)(x) << 8)))

#define BSWAP32(x) \
    ((uint32_t)(                                \
        (((uint32_t)(x) & 0xFF000000u) >> 24) | \
        (((uint32_t)(x) & 0x00FF0000u) >>  8) | \
        (((uint32_t)(x) & 0x0000FF00u) <<  8) | \
        (((uint32_t)(x) & 0x000000FFu) << 24)))

#define BSWAP64(x) \
    ((uint64_t)(                                          \
        (((uint64_t)(x) & 0xFF00000000000000ull) >> 56) | \
        (((uint64_t)(x) & 0x00FF000000000000ull) >> 40) | \
        (((uint64_t)(x) & 0x0000FF0000000000ull) >> 24) | \
        (((uint64_t)(x) & 0x000000FF00000000ull) >>  8) | \
        (((uint64_t)(x) & 0x00000000FF000000ull) <<  8) | \
        (((uint64_t)(x) & 0x0000000000FF0000ull) << 24) | \
        (((uint64_t)(x) & 0x000000000000FF00ull) << 40) | \
        (((uint64_t)(x) & 0x00000000000000FFull) << 56)))

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
#  define PROTOCOL_LITTLE_ENDIAN (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#elif defined(_WIN32)
#  define PROTOCOL_LITTLE_ENDIAN 1
#else
#  error "Cannot determine byte order"
#endif

#if PROTOCOL_LITTLE_ENDIAN
#  define BE16(x) BSWAP16(x)
#  define BE32(x) BSWAP32(x)
#  define BE64(x) BSWAP64(x)
#  define LE16(x) ((uint16_t)(x))   // PROTOCOL_LITTLE_ENDIAN DO NOT NEED MODIFY
#  define LE32(x) ((uint32_t)(x))
#  define LE64(x) ((uint64_t)(x))
#else
#  define BE16(x) ((uint16_t)(x))
#  define BE32(x) ((uint32_t)(x))
#  define BE64(x) ((uint64_t)(x))
#  define LE16(x) BSWAP16(x)
#  define LE32(x) BSWAP32(x)
#  define LE64(x) BSWAP64(x)
#endif
