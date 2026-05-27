#include "src/protocol/protocol.h"
#include <cstdlib>
#include <iostream>
#include <gtest/gtest.h>

namespace {

bool test_log_enabled() {
    return std::getenv("PD_TEST_LOG") != nullptr;
}

void log_case(const char* name, const Header& h) {
    if (!test_log_enabled()) {
        return;
    }
    std::cout << "[protocol-test] " << name
              << " magic=0x" << std::hex << h.magic
              << " session=0x" << h.session_id
              << " cmd=0x" << static_cast<int>(h.cmd_type)
              << " body=" << std::dec << h.body_length
              << " reserved=" << h.reserved << '\n';
}

void log_bytes(const char* name, const uint8_t* buf, size_t len) {
    if (!test_log_enabled()) {
        return;
    }
    std::cout << "[protocol-test] " << name << " wire=";
    for (size_t i = 0; i < len; ++i) {
        std::cout << (i == 0 ? "" : " ") << "0x"
                  << std::hex << static_cast<int>(buf[i]);
    }
    std::cout << std::dec << '\n';
}

}  // namespace

// ── serialize_header ─────────────────────────────────────────────────────────

TEST(SerializeHeader, MagicBytes) {
    Header h{};
    h.magic = PROTOCOL_MAGIC;
    uint8_t buf[HEADER_SIZE] = {};
    serialize_header(h, buf);
    log_bytes("serialize magic", buf, HEADER_SIZE);
    EXPECT_EQ(buf[0], 0xBE);
    EXPECT_EQ(buf[1], 0xEF);
}

TEST(SerializeHeader, SessionIdBigEndian) {
    Header h{};
    h.magic      = PROTOCOL_MAGIC;
    h.session_id = 0x01020304;
    uint8_t buf[HEADER_SIZE] = {};
    serialize_header(h, buf);
    log_bytes("serialize session big-endian", buf, HEADER_SIZE);
    EXPECT_EQ(buf[2], 0x01);
    EXPECT_EQ(buf[3], 0x02);
    EXPECT_EQ(buf[4], 0x03);
    EXPECT_EQ(buf[5], 0x04);
}

TEST(SerializeHeader, CmdTypeByte) {
    Header h{};
    h.magic    = PROTOCOL_MAGIC;
    h.cmd_type = CmdType::HEARTBEAT;
    uint8_t buf[HEADER_SIZE] = {};
    serialize_header(h, buf);
    EXPECT_EQ(buf[6], 0x01);
}

TEST(SerializeHeader, BodyLengthBigEndian) {
    Header h{};
    h.magic       = PROTOCOL_MAGIC;
    h.body_length = 0x00010203;
    uint8_t buf[HEADER_SIZE] = {};
    serialize_header(h, buf);
    log_bytes("serialize body length big-endian", buf, HEADER_SIZE);
    EXPECT_EQ(buf[7],  0x00);
    EXPECT_EQ(buf[8],  0x01);
    EXPECT_EQ(buf[9],  0x02);
    EXPECT_EQ(buf[10], 0x03);
}

TEST(SerializeHeader, ReservedZero) {
    Header h{};
    h.magic    = PROTOCOL_MAGIC;
    h.reserved = 0;
    uint8_t buf[HEADER_SIZE] = {};
    serialize_header(h, buf);
    EXPECT_EQ(buf[11], 0x00);
    EXPECT_EQ(buf[12], 0x00);
}

// ── deserialize_header ───────────────────────────────────────────────────────

TEST(DeserializeHeader, MagicRestored) {
    uint8_t buf[HEADER_SIZE] = {};
    buf[0] = 0xBE; buf[1] = 0xEF;
    Header h{};
    deserialize_header(buf, h);
    log_case("deserialize magic", h);
    EXPECT_EQ(h.magic, PROTOCOL_MAGIC);
}

TEST(DeserializeHeader, SessionIdRestored) {
    uint8_t buf[HEADER_SIZE] = {};
    buf[2] = 0x01; buf[3] = 0x02; buf[4] = 0x03; buf[5] = 0x04;
    Header h{};
    deserialize_header(buf, h);
    log_case("deserialize session", h);
    EXPECT_EQ(h.session_id, 0x01020304u);
}

TEST(DeserializeHeader, CmdTypeRestored) {
    uint8_t buf[HEADER_SIZE] = {};
    buf[6] = 0x20;
    Header h{};
    deserialize_header(buf, h);
    log_case("deserialize cmd", h);
    EXPECT_EQ(h.cmd_type, CmdType::INPUT_EVENT);
}

TEST(DeserializeHeader, BodyLengthRestored) {
    uint8_t buf[HEADER_SIZE] = {};
    buf[7] = 0x00; buf[8] = 0x00; buf[9] = 0x01; buf[10] = 0x00;
    Header h{};
    deserialize_header(buf, h);
    log_case("deserialize body length", h);
    EXPECT_EQ(h.body_length, 0x00000100u);
}

// ── RoundTrip ────────────────────────────────────────────────────────────────

TEST(RoundTrip, AllFields) {
    Header original{};
    original.magic       = PROTOCOL_MAGIC;
    original.session_id  = 0xDEADBEEF;
    original.cmd_type    = CmdType::PIXEL_DATA;
    original.body_length = 512;
    original.reserved    = 0;

    uint8_t buf[HEADER_SIZE] = {};
    serialize_header(original, buf);

    Header restored{};
    deserialize_header(buf, restored);
    log_bytes("round-trip serialized header", buf, HEADER_SIZE);
    log_case("round-trip restored header", restored);

    EXPECT_EQ(restored.magic,       original.magic);
    EXPECT_EQ(restored.session_id,  original.session_id);
    EXPECT_EQ(restored.cmd_type,    original.cmd_type);
    EXPECT_EQ(restored.body_length, original.body_length);
    EXPECT_EQ(restored.reserved,    original.reserved);
}

TEST(RoundTrip, ZeroSessionId) {
    Header original{};
    original.magic      = PROTOCOL_MAGIC;
    original.session_id = 0x00000000;
    original.cmd_type   = CmdType::HEARTBEAT;

    uint8_t buf[HEADER_SIZE] = {};
    serialize_header(original, buf);

    Header restored{};
    deserialize_header(buf, restored);

    EXPECT_EQ(restored.session_id, 0x00000000u);
}

TEST(RoundTrip, MaxBodyLength) {
    Header original{};
    original.magic       = PROTOCOL_MAGIC;
    original.body_length = MAX_BODY_LENGTH;

    uint8_t buf[HEADER_SIZE] = {};
    serialize_header(original, buf);

    Header restored{};
    deserialize_header(buf, restored);

    EXPECT_EQ(restored.body_length, MAX_BODY_LENGTH);
}
