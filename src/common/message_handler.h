#pragma once
#include "src/protocol/message_parser.h"
#include "src/protocol/protocol.h"
#include <cstdint>
#include <functional>
#include <unordered_map>

// Wraps MessageParser and routes complete packets to registered per-CmdType handlers.
// Shared by AppHost, Gateway, and Client: each component registers its own handlers
// and calls feed() from its IoPool IO_TYPE_RECEIVED callback.
class MessageHandler {
public:
    using HandlerFn = std::function<void(const Packet&)>;

    // Register a handler for cmd. Replaces any existing handler for the same type.
    void register_handler(CmdType cmd, HandlerFn fn);

    // Push raw bytes into the internal MessageParser, then dispatch any complete
    // packets to registered handlers. Unregistered CmdTypes are silently dropped.
    // Call this from IO_TYPE_RECEIVED.
    void feed(const uint8_t* data, size_t len);

    // Reset internal parser state. Call when a connection is reused or poisoned.
    void reset();

private:
    MessageParser                            parser_;
    std::unordered_map<uint8_t, HandlerFn>  handlers_;
};
