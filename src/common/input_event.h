#pragma once

// Standard library before GKC headers — GKC opens #pragma pack(push,1) in
// several places and sloppy include order can leak the packing state into
// STL containers.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "base/GkcDef.h"

// INPUT_EVENT body helpers (M5).
//
// The Body of `CmdType::INPUT_EVENT` always begins with a single
// `eventType` byte, which selects mouse vs keyboard layout:
//
//   Mouse Body (13 bytes, eventType in 0x01..0x06):
//     [eventType:1][x:BE32][y:BE32][timestamp:BE64]
//
//   Keyboard Body (11 bytes, eventType in 0x10..0x11):
//     [eventType:1][keyCode:BE16][timestamp:BE64]
//
// The Gateway never inspects this Body — it routes by SessionID only.  Mapping
// to GKC `UiMessageMouse` / `UiMessageKeyboard` happens here, on the AppHost
// side (or later, on the Client side that produces the events).
enum class InputEventType : uint8_t {
    MOUSE_MOVE       = 0x01,
    MOUSE_LEFT_DOWN  = 0x02,
    MOUSE_LEFT_UP    = 0x03,
    MOUSE_RIGHT_DOWN = 0x04,
    MOUSE_RIGHT_UP   = 0x05,
    MOUSE_SCROLL     = 0x06,
    KEY_DOWN         = 0x10,
    KEY_UP           = 0x11,
};

// On-wire sizes — useful for tests and for the parser fast-reject.
static constexpr size_t INPUT_EVENT_MOUSE_BODY_SIZE    = 13;
static constexpr size_t INPUT_EVENT_KEYBOARD_BODY_SIZE = 11;

// Result of parsing a raw INPUT_EVENT Body.  Exactly one of `mouse` /
// `keyboard` is meaningful, selected by `kind`.  All other fields are zero-
// initialised so callers can safely read either union half before checking.
struct ParsedInputEvent {
    enum class Kind { Mouse, Keyboard } kind = Kind::Mouse;

    GKC::UiMessageMouse    mouse{};
    GKC::UiMessageKeyboard keyboard{};

    // Raw eventType byte preserved for diagnostics; not consumed by the
    // dispatcher.
    InputEventType type = InputEventType::MOUSE_MOVE;

    // Original Client-side timestamp in microseconds.  Currently unused by
    // the AppHost demo path but kept around so future telemetry / replay
    // can read it without a second parse pass.
    uint64_t timestamp_us = 0;
};

// Pack a mouse INPUT_EVENT Body.  Returns exactly 13 bytes.
//
// Caller must pass a mouse `eventType` (0x01..0x06).  Out-of-range values
// are still serialised verbatim — the parser is the gate, not the packer.
std::vector<uint8_t> pack_mouse_input_event(InputEventType eventType,
                                            int32_t x, int32_t y,
                                            uint64_t timestamp_us);

// Pack a keyboard INPUT_EVENT Body.  Returns exactly 11 bytes.
//
// `keyCode` follows GKC `UiMessageKeyboard::btKey` semantics (KB_F1, KB_Left,
// etc.).  The parser rejects keyCode > 255; callers building well-formed
// packets must not exceed that range either.
std::vector<uint8_t> pack_keyboard_input_event(InputEventType eventType,
                                               uint16_t keyCode,
                                               uint64_t timestamp_us);

// Parse an INPUT_EVENT Body into `out`.  Returns true on success, false for
// any of the M5 documented reject cases (empty body, unknown eventType,
// wrong length, keyCode > 255).  On failure `out` is left in a valid but
// indeterminate state — callers must not act on it.
//
// The mapping into GKC structs follows §5.1:
//   MOUSE_MOVE       -> uEvent=MOUSE_EVENT_MOVE, btButton=NONE
//   MOUSE_LEFT_DOWN  -> uEvent=MOUSE_EVENT_DOWN, btButton=LEFT,  btState|=LEFT
//   MOUSE_LEFT_UP    -> uEvent=MOUSE_EVENT_UP,   btButton=LEFT,  btState=0
//   MOUSE_RIGHT_DOWN -> uEvent=MOUSE_EVENT_DOWN, btButton=RIGHT, btState|=RIGHT
//   MOUSE_RIGHT_UP   -> uEvent=MOUSE_EVENT_UP,   btButton=RIGHT, btState=0
//   MOUSE_SCROLL     -> uEvent=MOUSE_EVENT_WHEEL,btButton=NONE, btValue=0
//   KEY_DOWN         -> btDown=1, btKey=keyCode
//   KEY_UP           -> btDown=0, btKey=keyCode
bool parse_input_event_body(const uint8_t* data, size_t len,
                            ParsedInputEvent& out);
