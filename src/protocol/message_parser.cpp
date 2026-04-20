#include "src/protocol/message_parser.h"

void MessageParser::feed(const uint8_t* data, size_t len) {
    if (state_ == State::POISONED) return;
    // Only when read_pos_ > （buf_.size() / 2）, we can compact the buf_.
    if (read_pos_ > buf_.size() / 2 && read_pos_ > 0)
        compact();
    buf_.insert(buf_.end(), data, data + len);
}

ParseResult MessageParser::next_packet(Packet& out) {
    if (state_ == State::POISONED)
        return ParseResult::ERROR;

    if (state_ == State::WAIT_HEADER) {
        if (readable() < HEADER_SIZE)
            return ParseResult::INCOMPLETE;

        deserialize_header(buf_.data() + read_pos_, pending_header_);
        read_pos_ += HEADER_SIZE;

        if (pending_header_.magic != PROTOCOL_MAGIC) {
            state_ = State::POISONED;
            return ParseResult::ERROR;
        }
        if (pending_header_.body_length > MAX_BODY_LENGTH) {
            state_ = State::POISONED;
            return ParseResult::ERROR;
        }

        state_ = State::WAIT_BODY;
        // 直接 fall-through：body_length==0 的包（HEARTBEAT）无需等待额外字节
    }

    // state_ == WAIT_BODY
    if (readable() < pending_header_.body_length)
        return ParseResult::INCOMPLETE;

    out.header = pending_header_;
    out.body.assign(buf_.data() + read_pos_,
                    buf_.data() + read_pos_ + pending_header_.body_length);
    read_pos_ += pending_header_.body_length;
    state_ = State::WAIT_HEADER;
    return ParseResult::OK;
}

void MessageParser::reset() {
    buf_.clear();
    read_pos_       = 0;
    state_          = State::WAIT_HEADER;
    pending_header_ = {};
}

void MessageParser::compact() {
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(read_pos_));
    read_pos_ = 0;
}
