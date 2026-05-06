// Unit tests for input_event.h pack / parse helpers.  These cover the bad-
// packet rejection paths called out in process.md §5.1.1, plus a couple of
// happy-path field-mapping cases so the AppHost integration test can rely on
// helper output matching the GKC structs it dispatches into the plugin.

#include <gtest/gtest.h>

#include "src/common/input_event.h"

#include "base/GkcDef.h"

namespace {

constexpr uint64_t kTs = 0x0102030405060708ull;

}  // namespace

TEST(InputEventPack, MouseBodyIs13Bytes) {
    auto body = pack_mouse_input_event(InputEventType::MOUSE_LEFT_DOWN,
                                       7, 11, kTs);
    ASSERT_EQ(body.size(), INPUT_EVENT_MOUSE_BODY_SIZE);
    EXPECT_EQ(body[0],
              static_cast<uint8_t>(InputEventType::MOUSE_LEFT_DOWN));
    // x = 7 in BE32
    EXPECT_EQ(body[1], 0u); EXPECT_EQ(body[2], 0u);
    EXPECT_EQ(body[3], 0u); EXPECT_EQ(body[4], 7u);
    // y = 11 in BE32
    EXPECT_EQ(body[5], 0u); EXPECT_EQ(body[6], 0u);
    EXPECT_EQ(body[7], 0u); EXPECT_EQ(body[8], 11u);
    // timestamp BE64
    EXPECT_EQ(body[9],  0x01u); EXPECT_EQ(body[10], 0x02u);
    EXPECT_EQ(body[11], 0x03u); EXPECT_EQ(body[12], 0x04u);
    // body[13..] does not exist; size is exactly 13
}

TEST(InputEventPack, KeyboardBodyIs11Bytes) {
    auto body = pack_keyboard_input_event(InputEventType::KEY_DOWN,
                                          0x70 /*KB_F1*/, kTs);
    ASSERT_EQ(body.size(), INPUT_EVENT_KEYBOARD_BODY_SIZE);
    EXPECT_EQ(body[0], static_cast<uint8_t>(InputEventType::KEY_DOWN));
    EXPECT_EQ(body[1], 0x00u);
    EXPECT_EQ(body[2], 0x70u);
}

TEST(InputEventParse, RejectsEmptyBody) {
    ParsedInputEvent ev;
    EXPECT_FALSE(parse_input_event_body(nullptr, 0, ev));
    uint8_t dummy = 0x01;
    EXPECT_FALSE(parse_input_event_body(&dummy, 0, ev));
}

TEST(InputEventParse, RejectsUnknownEventType) {
    // 0x00 / 0x07 / 0x0F / 0x12 are not assigned in the M5 protocol.
    for (uint8_t bad : {uint8_t{0x00}, uint8_t{0x07}, uint8_t{0x0F},
                       uint8_t{0x12}, uint8_t{0xFF}}) {
        std::vector<uint8_t> body(INPUT_EVENT_MOUSE_BODY_SIZE, 0u);
        body[0] = bad;
        ParsedInputEvent ev;
        EXPECT_FALSE(parse_input_event_body(body.data(), body.size(), ev))
            << "unexpectedly accepted eventType=0x"
            << std::hex << static_cast<int>(bad);
    }
}

TEST(InputEventParse, RejectsWrongLengthMouse) {
    auto body = pack_mouse_input_event(InputEventType::MOUSE_MOVE,
                                       1, 2, kTs);
    ParsedInputEvent ev;
    // Truncated by one byte.
    EXPECT_FALSE(parse_input_event_body(body.data(), body.size() - 1, ev));
    // Padded by one byte.
    body.push_back(0u);
    EXPECT_FALSE(parse_input_event_body(body.data(), body.size(), ev));
}

TEST(InputEventParse, RejectsWrongLengthKeyboard) {
    auto body = pack_keyboard_input_event(InputEventType::KEY_DOWN,
                                          0x70, kTs);
    ParsedInputEvent ev;
    EXPECT_FALSE(parse_input_event_body(body.data(), body.size() - 1, ev));
    body.push_back(0u);
    EXPECT_FALSE(parse_input_event_body(body.data(), body.size(), ev));
}

TEST(InputEventParse, RejectsKeyCodeAbove255) {
    // pack_keyboard_input_event still serialises any uint16_t; the parser is
    // the gate that enforces "btKey is one byte".
    auto body = pack_keyboard_input_event(InputEventType::KEY_DOWN,
                                          0x0100 /* > 0xFF */, kTs);
    ParsedInputEvent ev;
    EXPECT_FALSE(parse_input_event_body(body.data(), body.size(), ev));
}

TEST(InputEventParse, KeyDownF1MapsToGkcKeyboard) {
    auto body = pack_keyboard_input_event(InputEventType::KEY_DOWN,
                                          0x70 /* KB_F1 */, kTs);
    ParsedInputEvent ev;
    ASSERT_TRUE(parse_input_event_body(body.data(), body.size(), ev));
    EXPECT_EQ(ev.kind, ParsedInputEvent::Kind::Keyboard);
    EXPECT_EQ(ev.type, InputEventType::KEY_DOWN);
    EXPECT_EQ(ev.timestamp_us, kTs);
    EXPECT_EQ(ev.keyboard.btDown, 1);
    EXPECT_EQ(ev.keyboard.btKey,  0x70);
    EXPECT_EQ(ev.keyboard.btState, 0);
    EXPECT_EQ(ev.keyboard.btValue, 0);
    EXPECT_EQ(ev.keyboard.chFull,  0u);
}

TEST(InputEventParse, KeyUpClearsDownFlag) {
    auto body = pack_keyboard_input_event(InputEventType::KEY_UP,
                                          0x25 /* KB_Left */, kTs);
    ParsedInputEvent ev;
    ASSERT_TRUE(parse_input_event_body(body.data(), body.size(), ev));
    EXPECT_EQ(ev.keyboard.btDown, 0);
    EXPECT_EQ(ev.keyboard.btKey,  0x25);
}

TEST(InputEventParse, LeftDownMapsToGkcMouse) {
    auto body = pack_mouse_input_event(InputEventType::MOUSE_LEFT_DOWN,
                                       9, 5, kTs);
    ParsedInputEvent ev;
    ASSERT_TRUE(parse_input_event_body(body.data(), body.size(), ev));
    EXPECT_EQ(ev.kind, ParsedInputEvent::Kind::Mouse);
    EXPECT_EQ(ev.type, InputEventType::MOUSE_LEFT_DOWN);
    EXPECT_EQ(ev.mouse.uEvent,    static_cast<unsigned>(MOUSE_EVENT_DOWN));
    EXPECT_EQ(ev.mouse.btButton,  MOUSE_BUTTON_LEFT);
    EXPECT_EQ(ev.mouse.btState & MOUSE_STATE_LEFT, MOUSE_STATE_LEFT);
    EXPECT_EQ(ev.mouse.x, 9);
    EXPECT_EQ(ev.mouse.y, 5);
}

TEST(InputEventParse, MouseMoveHasNoButton) {
    auto body = pack_mouse_input_event(InputEventType::MOUSE_MOVE,
                                       -3, 100, kTs);
    ParsedInputEvent ev;
    ASSERT_TRUE(parse_input_event_body(body.data(), body.size(), ev));
    EXPECT_EQ(ev.mouse.uEvent,   static_cast<unsigned>(MOUSE_EVENT_MOVE));
    EXPECT_EQ(ev.mouse.btButton, MOUSE_BUTTON_NONE);
    EXPECT_EQ(ev.mouse.x, -3);
    EXPECT_EQ(ev.mouse.y, 100);
}

TEST(InputEventParse, ScrollMapsToWheel) {
    auto body = pack_mouse_input_event(InputEventType::MOUSE_SCROLL,
                                       0, 0, kTs);
    ParsedInputEvent ev;
    ASSERT_TRUE(parse_input_event_body(body.data(), body.size(), ev));
    EXPECT_EQ(ev.mouse.uEvent,   static_cast<unsigned>(MOUSE_EVENT_WHEEL));
    EXPECT_EQ(ev.mouse.btButton, MOUSE_BUTTON_NONE);
    EXPECT_EQ(ev.mouse.btValue,  0);
}
