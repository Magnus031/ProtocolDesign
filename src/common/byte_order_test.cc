#include "src/common/byte_order.h"
#include <cstdlib>
#include <iostream>
#include <gtest/gtest.h>

namespace {

bool test_log_enabled() {
    return std::getenv("PD_TEST_LOG") != nullptr;
}

template <typename T>
void log_bytes(const char* name, T value) {
    if (!test_log_enabled()) {
        return;
    }
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    std::cout << "[byte-order-test] " << name << " bytes=";
    for (size_t i = 0; i < sizeof(T); ++i) {
        std::cout << (i == 0 ? "" : " ") << "0x"
                  << std::hex << static_cast<int>(bytes[i]);
    }
    std::cout << std::dec << '\n';
}

}  // namespace

TEST(BE16Test, KnownValue) {
    uint16_t val = BE16(0x1234);
    log_bytes("BE16 0x1234", val);
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&val);
    EXPECT_EQ(b[0], 0x12);
    EXPECT_EQ(b[1], 0x34);
}

TEST(BE32Test, KnownValue) {
    uint32_t val = BE32(0x12345678);
    log_bytes("BE32 0x12345678", val);
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&val);
    EXPECT_EQ(b[0], 0x12);
    EXPECT_EQ(b[1], 0x34);
    EXPECT_EQ(b[2], 0x56);
    EXPECT_EQ(b[3], 0x78);
}

TEST(BE16Test, RoundTrip) {
    uint16_t original = 0xBEEF;
    EXPECT_EQ(BE16(BE16(original)), original);
}

TEST(BE32Test, RoundTrip) {
    uint32_t original = 0xDEADBEEF;
    EXPECT_EQ(BE32(BE32(original)), original);
}

TEST(LE16Test, KnownValue) {
    uint16_t val = LE16(0x1234);
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&val);
    EXPECT_EQ(b[0], 0x34);
    EXPECT_EQ(b[1], 0x12);
}

TEST(LE32Test, KnownValue) {
    uint32_t val = LE32(0x12345678);
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&val);
    EXPECT_EQ(b[0], 0x78);
    EXPECT_EQ(b[1], 0x56);
    EXPECT_EQ(b[2], 0x34);
    EXPECT_EQ(b[3], 0x12);
}

TEST(BE64Test, KnownValue) {
    uint64_t val = BE64(0x0102030405060708ull);
    log_bytes("BE64 0x0102030405060708", val);
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&val);
    EXPECT_EQ(b[0], 0x01);
    EXPECT_EQ(b[1], 0x02);
    EXPECT_EQ(b[2], 0x03);
    EXPECT_EQ(b[3], 0x04);
    EXPECT_EQ(b[4], 0x05);
    EXPECT_EQ(b[5], 0x06);
    EXPECT_EQ(b[6], 0x07);
    EXPECT_EQ(b[7], 0x08);
}

TEST(BE64Test, RoundTrip) {
    uint64_t original = 0xDEADBEEFCAFEBABEull;
    EXPECT_EQ(BE64(BE64(original)), original);
}

TEST(LE64Test, KnownValue) {
    uint64_t val = LE64(0x0102030405060708ull);
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&val);
    EXPECT_EQ(b[0], 0x08);
    EXPECT_EQ(b[1], 0x07);
    EXPECT_EQ(b[2], 0x06);
    EXPECT_EQ(b[3], 0x05);
    EXPECT_EQ(b[4], 0x04);
    EXPECT_EQ(b[5], 0x03);
    EXPECT_EQ(b[6], 0x02);
    EXPECT_EQ(b[7], 0x01);
}
