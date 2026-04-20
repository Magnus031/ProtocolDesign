#pragma once
#include "src/protocol/protocol.h"
#include <cstddef>
#include <cstdint>
#include <vector>

enum class ParseResult {
    OK,
    INCOMPLETE,
    ERROR,
};

class MessageParser {
public:
    // Appends len bytes of raw TCP stream data into the internal buffer.
    // Safe to call with len == 0. Does nothing if the parser is POISONED.
    void        feed(const uint8_t* data, size_t len);

    // Attempts to extract one complete Packet from the buffer.
    // Returns OK and populates out on success; INCOMPLETE if more bytes are needed;
    // ERROR on a protocol violation (magic mismatch or body_length > MAX_BODY_LENGTH).
    // Once ERROR is returned the parser is POISONED and must be reset() or destroyed.
    ParseResult next_packet(Packet& out);

    // Clears the internal buffer and returns the parser to its initial WAIT_HEADER state.
    // Call this when the underlying TCP connection is closed or after an ERROR.
    void        reset();

private:
    enum class State {
        // Waiting to accumulate 13 header bytes. This is the initial state and
        // is re-entered after each successfully emitted Packet.
        // e.g. recv() returned only 6 bytes — header is incomplete, stay here until next feed().
        WAIT_HEADER,

        // Header parsed; waiting for pending_header_.body_length bytes of body to arrive.
        // e.g. header declares a 1024-byte body but only 300 bytes received — wait for the remaining 724.
        // Special case: body_length == 0 (e.g. HEARTBEAT) falls through instantly without blocking.
        WAIT_BODY,

        // Unrecoverable protocol error. All subsequent feed()/next_packet() calls return ERROR immediately.
        // Triggered when: magic != 0xBEEF, or body_length > MAX_BODY_LENGTH (64 MB).
        POISONED,
    };
    // Get the number of bytes that we can read.
    size_t readable() const { return buf_.size() - read_pos_; }
    
    void   compact();

    std::vector<uint8_t> buf_;
    size_t               read_pos_       = 0;
    State                state_          = State::WAIT_HEADER;
    Header               pending_header_ = {};
};
