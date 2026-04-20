#include "src/protocol/message_parser.h"
#include <gtest/gtest.h>
#include <vector>

// Builds a fully serialized wire packet from logical fields.
static std::vector<uint8_t> make_wire(uint32_t session_id,
                                      CmdType  cmd,
                                      const std::vector<uint8_t>& body = {}) {
    Header h{};
    h.magic       = PROTOCOL_MAGIC;
    h.session_id  = session_id;
    h.cmd_type    = cmd;
    h.body_length = static_cast<uint32_t>(body.size());
    h.reserved    = 0;

    std::vector<uint8_t> wire(HEADER_SIZE + body.size());
    serialize_header(h, wire.data());
    std::copy(body.begin(), body.end(), wire.data() + HEADER_SIZE);
    return wire;
}

// ── Group 1: happy path ───────────────────────────────────────────────────────

TEST(MessageParser, HeartbeatZeroBodyParsed) {
    auto wire = make_wire(1, CmdType::HEARTBEAT);

    MessageParser parser;
    parser.feed(wire.data(), wire.size());

    Packet pkt;
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::OK);
    EXPECT_EQ(pkt.header.magic,       PROTOCOL_MAGIC);
    EXPECT_EQ(pkt.header.session_id,  1u);
    EXPECT_EQ(pkt.header.cmd_type,    CmdType::HEARTBEAT);
    EXPECT_EQ(pkt.header.body_length, 0u);
    EXPECT_TRUE(pkt.body.empty());
    // Buffer exhausted — next call must be INCOMPLETE.
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::INCOMPLETE);
}

TEST(MessageParser, PacketWithBodyParsed) {
    std::vector<uint8_t> body = {0x01, 0x02, 0x03, 0x04};
    auto wire = make_wire(42, CmdType::SPAWN_APP, body);

    MessageParser parser;
    parser.feed(wire.data(), wire.size());

    Packet pkt;
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::OK);
    EXPECT_EQ(pkt.header.session_id, 42u);
    EXPECT_EQ(pkt.header.cmd_type,   CmdType::SPAWN_APP);
    ASSERT_EQ(pkt.body.size(),       4u);
    EXPECT_EQ(pkt.body,              body);
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::INCOMPLETE);
}

// ── Group 2: fragmentation ────────────────────────────────────────────────────

TEST(MessageParser, HeaderSplitAcrossFeeds) {
    auto wire = make_wire(1, CmdType::HEARTBEAT);

    MessageParser parser;
    parser.feed(wire.data(), 6);  // first 6 of 13 header bytes

    Packet pkt;
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::INCOMPLETE);

    parser.feed(wire.data() + 6, wire.size() - 6);
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::OK);
    EXPECT_EQ(pkt.header.cmd_type, CmdType::HEARTBEAT);
}

TEST(MessageParser, BodySplitAcrossFeeds) {
    std::vector<uint8_t> body(100, 0xAB);
    auto wire = make_wire(1, CmdType::PIXEL_DATA, body);

    MessageParser parser;
    parser.feed(wire.data(), HEADER_SIZE + 50);  // header + half body

    Packet pkt;
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::INCOMPLETE);

    parser.feed(wire.data() + HEADER_SIZE + 50, 50);  // remaining body
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::OK);
    EXPECT_EQ(pkt.body, body);
}

TEST(MessageParser, OneByteFeedAtATime) {
    std::vector<uint8_t> body = {0xDE, 0xAD};
    auto wire = make_wire(7, CmdType::INPUT_EVENT, body);

    MessageParser parser;
    Packet pkt;
    for (size_t i = 0; i < wire.size() - 1; ++i) {
        parser.feed(&wire[i], 1);
        EXPECT_EQ(parser.next_packet(pkt), ParseResult::INCOMPLETE);
    }
    parser.feed(&wire.back(), 1);
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::OK);
    EXPECT_EQ(pkt.body, body);
}

// ── Group 3: multiple packets ─────────────────────────────────────────────────

TEST(MessageParser, TwoPacketsInOneFeed) {
    auto wire1 = make_wire(1, CmdType::HEARTBEAT);
    auto wire2 = make_wire(2, CmdType::CLOSE_SESSION, {0x00});

    std::vector<uint8_t> combined;
    combined.insert(combined.end(), wire1.begin(), wire1.end());
    combined.insert(combined.end(), wire2.begin(), wire2.end());

    MessageParser parser;
    parser.feed(combined.data(), combined.size());

    Packet pkt;
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::OK);
    EXPECT_EQ(pkt.header.cmd_type, CmdType::HEARTBEAT);

    EXPECT_EQ(parser.next_packet(pkt), ParseResult::OK);
    EXPECT_EQ(pkt.header.cmd_type,   CmdType::CLOSE_SESSION);
    EXPECT_EQ(pkt.header.session_id, 2u);

    EXPECT_EQ(parser.next_packet(pkt), ParseResult::INCOMPLETE);
}

TEST(MessageParser, ManySequentialPacketsStayCorrect) {
    // Exercises compact() by feeding and consuming one packet at a time.
    MessageParser parser;
    for (uint32_t i = 0; i < 20; ++i) {
        auto wire = make_wire(i, CmdType::HEARTBEAT);
        parser.feed(wire.data(), wire.size());

        Packet pkt;
        ASSERT_EQ(parser.next_packet(pkt), ParseResult::OK);
        EXPECT_EQ(pkt.header.session_id, i);
        ASSERT_EQ(parser.next_packet(pkt), ParseResult::INCOMPLETE);
    }
}

// ── Group 4: POISONED state ───────────────────────────────────────────────────

TEST(MessageParser, BadMagicPoisons) {
    auto wire = make_wire(1, CmdType::HEARTBEAT);
    wire[0] = 0xDE;  // corrupt magic byte
    wire[1] = 0xAD;

    MessageParser parser;
    parser.feed(wire.data(), wire.size());

    Packet pkt;
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::ERROR);
    // POISONED: all subsequent calls must also return ERROR.
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::ERROR);
}

TEST(MessageParser, BodyLengthOverMaxPoisons) {
    auto wire = make_wire(1, CmdType::PIXEL_DATA);
    // Overwrite body_length field with MAX_BODY_LENGTH + 1.
    const uint32_t bad = MAX_BODY_LENGTH + 1;
    wire[7]  = (bad >> 24) & 0xFF;
    wire[8]  = (bad >> 16) & 0xFF;
    wire[9]  = (bad >>  8) & 0xFF;
    wire[10] =  bad        & 0xFF;

    MessageParser parser;
    parser.feed(wire.data(), wire.size());

    Packet pkt;
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::ERROR);
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::ERROR);
}

TEST(MessageParser, FeedAfterPoisonedIsNoop) {
    auto wire = make_wire(1, CmdType::HEARTBEAT);
    wire[0] = 0x00;  // bad magic

    MessageParser parser;
    parser.feed(wire.data(), wire.size());

    Packet pkt;
    ASSERT_EQ(parser.next_packet(pkt), ParseResult::ERROR);

    // Feeding valid data into a poisoned parser must not recover it.
    auto valid = make_wire(1, CmdType::HEARTBEAT);
    parser.feed(valid.data(), valid.size());
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::ERROR);
}

// ── Group 5: reset() ──────────────────────────────────────────────────────────

TEST(MessageParser, ResetClearsPartialHeader) {
    auto wire = make_wire(1, CmdType::HEARTBEAT);

    MessageParser parser;
    parser.feed(wire.data(), 6);  // partial header

    Packet pkt;
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::INCOMPLETE);

    parser.reset();

    // After reset, a complete packet must parse correctly.
    parser.feed(wire.data(), wire.size());
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::OK);
    EXPECT_EQ(pkt.header.cmd_type, CmdType::HEARTBEAT);
}

TEST(MessageParser, ResetAfterPoisonedAllowsReuse) {
    auto bad = make_wire(1, CmdType::HEARTBEAT);
    bad[0] = 0x00;  // corrupt magic

    MessageParser parser;
    parser.feed(bad.data(), bad.size());

    Packet pkt;
    ASSERT_EQ(parser.next_packet(pkt), ParseResult::ERROR);

    parser.reset();

    auto good = make_wire(1, CmdType::HEARTBEAT);
    parser.feed(good.data(), good.size());
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::OK);
}

// ── Group 6: edge cases ───────────────────────────────────────────────────────

TEST(MessageParser, NextPacketOnFreshParserIsIncomplete) {
    MessageParser parser;
    Packet pkt;
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::INCOMPLETE);
}

TEST(MessageParser, ZeroLengthFeedIsNoop) {
    auto wire = make_wire(1, CmdType::HEARTBEAT);

    MessageParser parser;
    const uint8_t dummy = 0;
    parser.feed(&dummy, 0);  // zero-length feed, valid pointer

    Packet pkt;
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::INCOMPLETE);

    // Normal feed still works after a zero-length feed.
    parser.feed(wire.data(), wire.size());
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::OK);
}

TEST(MessageParser, ExactlyHeaderSizeWithZeroBody) {
    // Feeds precisely HEADER_SIZE bytes for a zero-body packet.
    auto wire = make_wire(99, CmdType::HEARTBEAT);
    ASSERT_EQ(wire.size(), HEADER_SIZE);

    MessageParser parser;
    parser.feed(wire.data(), wire.size());

    Packet pkt;
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::OK);
    EXPECT_EQ(pkt.header.session_id, 99u);
    EXPECT_TRUE(pkt.body.empty());
}
