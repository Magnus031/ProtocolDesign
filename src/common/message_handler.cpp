#include "src/common/message_handler.h"

void MessageHandler::register_handler(CmdType cmd, HandlerFn fn) {
    handlers_[static_cast<uint8_t>(cmd)] = std::move(fn);
}

void MessageHandler::feed(const uint8_t* data, size_t len) {
    parser_.feed(data, len);
    Packet pkt;
    while (parser_.next_packet(pkt) == ParseResult::OK) {
        auto it = handlers_.find(static_cast<uint8_t>(pkt.header.cmd_type));
        if (it != handlers_.end())
            it->second(pkt);
    }
}

void MessageHandler::reset() {
    parser_ = MessageParser{};
}
