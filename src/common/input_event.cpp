// Standard library before GKC headers (pack-state hygiene).
#include <cstdint>
#include <cstring>
#include <vector>

#include "src/common/input_event.h"
#include "src/common/byte_order.h"

#include "base/GkcDef.h"

namespace {

// Big-endian readers / writers.  Use byte_order.h's BE* macros through
// memcpy so the code remains endian-correct without requiring aligned access.
void write_be32(uint8_t* p, uint32_t v) noexcept {
    const uint32_t be = BE32(v);
    std::memcpy(p, &be, sizeof(be));
}
void write_be16(uint8_t* p, uint16_t v) noexcept {
    const uint16_t be = BE16(v);
    std::memcpy(p, &be, sizeof(be));
}
void write_be64(uint8_t* p, uint64_t v) noexcept {
    const uint64_t be = BE64(v);
    std::memcpy(p, &be, sizeof(be));
}

uint32_t read_be32(const uint8_t* p) noexcept {
    uint32_t be = 0;
    std::memcpy(&be, p, sizeof(be));
    return BE32(be);
}
uint16_t read_be16(const uint8_t* p) noexcept {
    uint16_t be = 0;
    std::memcpy(&be, p, sizeof(be));
    return BE16(be);
}
uint64_t read_be64(const uint8_t* p) noexcept {
    uint64_t be = 0;
    std::memcpy(&be, p, sizeof(be));
    return BE64(be);
}

bool fill_mouse_message(InputEventType type,
                        int32_t x, int32_t y,
                        GKC::UiMessageMouse& m) noexcept {
    m = GKC::UiMessageMouse{};
    m.x = x;
    m.y = y;
    switch (type) {
        case InputEventType::MOUSE_MOVE:
            m.uEvent   = MOUSE_EVENT_MOVE;
            m.btButton = MOUSE_BUTTON_NONE;
            return true;
        case InputEventType::MOUSE_LEFT_DOWN:
            m.uEvent   = MOUSE_EVENT_DOWN;
            m.btButton = MOUSE_BUTTON_LEFT;
            m.btState  = MOUSE_STATE_LEFT;
            return true;
        case InputEventType::MOUSE_LEFT_UP:
            m.uEvent   = MOUSE_EVENT_UP;
            m.btButton = MOUSE_BUTTON_LEFT;
            return true;
        case InputEventType::MOUSE_RIGHT_DOWN:
            m.uEvent   = MOUSE_EVENT_DOWN;
            m.btButton = MOUSE_BUTTON_RIGHT;
            m.btState  = MOUSE_STATE_RIGHT;
            return true;
        case InputEventType::MOUSE_RIGHT_UP:
            m.uEvent   = MOUSE_EVENT_UP;
            m.btButton = MOUSE_BUTTON_RIGHT;
            return true;
        case InputEventType::MOUSE_SCROLL:
            m.uEvent   = MOUSE_EVENT_WHEEL;
            m.btButton = MOUSE_BUTTON_NONE;
            m.btValue  = 0;
            return true;
        default:
            return false;
    }
}

bool fill_keyboard_message(InputEventType type, uint16_t key_code,
                           GKC::UiMessageKeyboard& kb) noexcept {
    if (key_code > 0xFF) return false;
    kb = GKC::UiMessageKeyboard{};
    // GKC::UiMessageKeyboard::btKey is the global `byte` typedef (unsigned char).
    kb.btKey   = static_cast<unsigned char>(key_code);
    kb.btState = 0;
    kb.btValue = 0;
    kb.chFull  = 0;
    if (type == InputEventType::KEY_DOWN) {
        kb.btDown = 1;
        return true;
    }
    if (type == InputEventType::KEY_UP) {
        kb.btDown = 0;
        return true;
    }
    return false;
}

bool is_mouse_event(uint8_t raw) noexcept {
    return raw >= 0x01u && raw <= 0x06u;
}
bool is_keyboard_event(uint8_t raw) noexcept {
    return raw == 0x10u || raw == 0x11u;
}

}  // namespace

std::vector<uint8_t> pack_mouse_input_event(InputEventType eventType,
                                            int32_t x, int32_t y,
                                            uint64_t timestamp_us) {
    std::vector<uint8_t> out(INPUT_EVENT_MOUSE_BODY_SIZE);
    out[0] = static_cast<uint8_t>(eventType);
    write_be32(out.data() + 1, static_cast<uint32_t>(x));
    write_be32(out.data() + 5, static_cast<uint32_t>(y));
    write_be64(out.data() + 9, timestamp_us);
    return out;
}

std::vector<uint8_t> pack_keyboard_input_event(InputEventType eventType,
                                               uint16_t keyCode,
                                               uint64_t timestamp_us) {
    std::vector<uint8_t> out(INPUT_EVENT_KEYBOARD_BODY_SIZE);
    out[0] = static_cast<uint8_t>(eventType);
    write_be16(out.data() + 1, keyCode);
    write_be64(out.data() + 3, timestamp_us);
    return out;
}

bool parse_input_event_body(const uint8_t* data, size_t len,
                            ParsedInputEvent& out) {
    if (data == nullptr || len == 0) return false;

    const uint8_t raw = data[0];

    if (is_mouse_event(raw)) {
        if (len != INPUT_EVENT_MOUSE_BODY_SIZE) return false;
        const auto type = static_cast<InputEventType>(raw);
        const int32_t x = static_cast<int32_t>(read_be32(data + 1));
        const int32_t y = static_cast<int32_t>(read_be32(data + 5));
        out = ParsedInputEvent{};
        out.kind         = ParsedInputEvent::Kind::Mouse;
        out.type         = type;
        out.timestamp_us = read_be64(data + 9);
        return fill_mouse_message(type, x, y, out.mouse);
    }

    if (is_keyboard_event(raw)) {
        if (len != INPUT_EVENT_KEYBOARD_BODY_SIZE) return false;
        const auto type = static_cast<InputEventType>(raw);
        const uint16_t key_code = read_be16(data + 1);
        out = ParsedInputEvent{};
        out.kind         = ParsedInputEvent::Kind::Keyboard;
        out.type         = type;
        out.timestamp_us = read_be64(data + 3);
        return fill_keyboard_message(type, key_code, out.keyboard);
    }

    return false;
}
