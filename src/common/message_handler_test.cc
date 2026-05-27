#include <gtest/gtest.h>
#include "src/common/message_handler.h"
#include "src/protocol/protocol.h"
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

bool test_log_enabled() {
    return std::getenv("PD_TEST_LOG") != nullptr;
}

const char* cmd_name(CmdType cmd) {
    switch (cmd) {
        case CmdType::HEARTBEAT: return "HEARTBEAT";
        case CmdType::SPAWN_APP: return "SPAWN_APP";
        case CmdType::SESSION_ACK: return "SESSION_ACK";
        case CmdType::APPHOST_READY: return "APPHOST_READY";
        case CmdType::PIXEL_DATA: return "PIXEL_DATA";
        case CmdType::INPUT_EVENT: return "INPUT_EVENT";
        case CmdType::CLOSE_SESSION: return "CLOSE_SESSION";
        case CmdType::ERROR_RESP: return "ERROR_RESP";
    }
    return "UNKNOWN";
}

void log_feed(const char* scenario, CmdType cmd, uint32_t session_id,
              size_t bytes) {
    if (!test_log_enabled()) {
        return;
    }
    std::cout << "[message-handler-test] " << scenario
              << " feed cmd=" << cmd_name(cmd)
              << " session=" << session_id
              << " bytes=" << bytes << '\n';
}

void log_dispatch(const char* handler_name, const Packet& packet) {
    if (!test_log_enabled()) {
        return;
    }
    std::cout << "[message-handler-test] dispatch handler=" << handler_name
              << " cmd=" << cmd_name(packet.header.cmd_type)
              << " session=" << packet.header.session_id
              << " body=" << packet.body.size() << '\n';
}

}  // namespace

// ── Packet builder helpers ───────────────────────────────────────────────────

static std::vector<uint8_t> make_packet(CmdType cmd, uint32_t session_id,
                                        const std::vector<uint8_t>& body = {}) {
    Header hdr{};
    hdr.magic       = PROTOCOL_MAGIC;
    hdr.session_id  = session_id;
    hdr.cmd_type    = cmd;
    hdr.body_length = static_cast<uint32_t>(body.size());

    std::vector<uint8_t> pkt(HEADER_SIZE + body.size());
    serialize_header(hdr, pkt.data());
    if (!body.empty())
        std::memcpy(pkt.data() + HEADER_SIZE, body.data(), body.size());
    return pkt;
}

// ── Tests ────────────────────────────────────────────────────────────────────

// Core M2 requirement: two handlers registered, each packet dispatched to the
// correct handler exactly once.
TEST(MessageHandlerTest, DispatchToRegisteredHandlers) {
    MessageHandler handler;

    int         heartbeat_calls   = 0;
    uint32_t    heartbeat_session = 0;
    handler.register_handler(CmdType::HEARTBEAT, [&](const Packet& p) {
        ++heartbeat_calls;
        heartbeat_session = p.header.session_id;
        log_dispatch("heartbeat", p);
    });

    int         pixel_calls   = 0;
    uint32_t    pixel_session = 0;
    handler.register_handler(CmdType::PIXEL_DATA, [&](const Packet& p) {
        ++pixel_calls;
        pixel_session = p.header.session_id;
        log_dispatch("pixel", p);
    });

    // Feed a HEARTBEAT — only HEARTBEAT handler must fire.
    auto hb = make_packet(CmdType::HEARTBEAT, 0xABCD);
    log_feed("registered heartbeat", CmdType::HEARTBEAT, 0xABCD, hb.size());
    handler.feed(hb.data(), hb.size());

    EXPECT_EQ(heartbeat_calls,   1);
    EXPECT_EQ(heartbeat_session, 0xABCDu);
    EXPECT_EQ(pixel_calls,       0);

    // Feed a PIXEL_DATA packet with a small body.
    std::vector<uint8_t> body(8, 0xFF);
    auto px = make_packet(CmdType::PIXEL_DATA, 0x1234, body);
    log_feed("registered pixel", CmdType::PIXEL_DATA, 0x1234, px.size());
    handler.feed(px.data(), px.size());

    EXPECT_EQ(heartbeat_calls, 1);   // unchanged
    EXPECT_EQ(pixel_calls,     1);
    EXPECT_EQ(pixel_session,   0x1234u);
}

// Packets whose CmdType has no registered handler are silently dropped.
TEST(MessageHandlerTest, UnknownCmdTypeIgnored) {
    MessageHandler handler;
    // Deliberately register nothing.
    auto hb = make_packet(CmdType::HEARTBEAT, 1);
    log_feed("unregistered heartbeat", CmdType::HEARTBEAT, 1, hb.size());
    EXPECT_NO_FATAL_FAILURE(handler.feed(hb.data(), hb.size()));
}

// Bytes fed one at a time simulate extreme TCP fragmentation.
TEST(MessageHandlerTest, ByteByByteFeed) {
    MessageHandler handler;
    int calls = 0;
    handler.register_handler(CmdType::HEARTBEAT, [&](const Packet& p) {
        ++calls;
        log_dispatch("byte-by-byte heartbeat", p);
    });

    auto hb = make_packet(CmdType::HEARTBEAT, 7);
    for (const uint8_t& b : hb) {
        log_feed("byte-by-byte", CmdType::HEARTBEAT, 7, 1);
        handler.feed(&b, 1);
    }

    EXPECT_EQ(calls, 1);
}

// Two back-to-back packets in a single feed call are both dispatched.
TEST(MessageHandlerTest, TwoPacketsInOneFeed) {
    MessageHandler handler;
    int calls = 0;
    handler.register_handler(CmdType::HEARTBEAT, [&](const Packet& p) {
        ++calls;
        log_dispatch("heartbeat", p);
    });

    auto p1 = make_packet(CmdType::HEARTBEAT, 1);
    auto p2 = make_packet(CmdType::HEARTBEAT, 2);
    std::vector<uint8_t> combined(p1);
    combined.insert(combined.end(), p2.begin(), p2.end());

    log_feed("two packets one feed", CmdType::HEARTBEAT, 0, combined.size());
    handler.feed(combined.data(), combined.size());
    EXPECT_EQ(calls, 2);
}

// reset() discards any buffered partial state; subsequent complete packets
// are dispatched normally.
TEST(MessageHandlerTest, ResetClearsPartialState) {
    MessageHandler handler;
    int calls = 0;
    handler.register_handler(CmdType::HEARTBEAT, [&](const Packet&) { ++calls; });

    auto hb = make_packet(CmdType::HEARTBEAT, 1);

    // Feed only part of the header — no packet emitted yet.
    handler.feed(hb.data(), 4);
    EXPECT_EQ(calls, 0);

    // Reset discards the partial bytes.
    handler.reset();

    // A fresh complete packet must now dispatch correctly.
    handler.feed(hb.data(), hb.size());
    EXPECT_EQ(calls, 1);
}

// Registering a handler for the same CmdType twice replaces the first.
TEST(MessageHandlerTest, RegisterOverwritesPreviousHandler) {
    MessageHandler handler;
    int first_calls  = 0;
    int second_calls = 0;

    handler.register_handler(CmdType::HEARTBEAT,
                             [&](const Packet&) { ++first_calls;  });
    handler.register_handler(CmdType::HEARTBEAT,
                             [&](const Packet&) { ++second_calls; });

    auto hb = make_packet(CmdType::HEARTBEAT, 1);
    handler.feed(hb.data(), hb.size());

    EXPECT_EQ(first_calls,  0);
    EXPECT_EQ(second_calls, 1);
}
