#include "src/protocol/protocol.h"

// 协议头内存布局（偏移量，共 13 字节）：
//   0-1  : Magic      (uint16_t, 大端)
//   2-5  : SessionID  (uint32_t, 大端)
//   6    : CmdType    (uint8_t)
//   7-10 : BodyLength (uint32_t, 大端)
//  11-12 : Reserved   (uint16_t, 大端)

void serialize_header(const Header& h, uint8_t* buf) {
    buf[0]  = (h.magic        >> 8) & 0xFF;
    buf[1]  =  h.magic              & 0xFF;

    buf[2]  = (h.session_id   >> 24) & 0xFF;
    buf[3]  = (h.session_id   >> 16) & 0xFF;
    buf[4]  = (h.session_id   >>  8) & 0xFF;
    buf[5]  =  h.session_id          & 0xFF;

    buf[6]  = static_cast<uint8_t>(h.cmd_type);

    buf[7]  = (h.body_length  >> 24) & 0xFF;
    buf[8]  = (h.body_length  >> 16) & 0xFF;
    buf[9]  = (h.body_length  >>  8) & 0xFF;
    buf[10] =  h.body_length         & 0xFF;

    buf[11] = (h.reserved     >> 8) & 0xFF;
    buf[12] =  h.reserved           & 0xFF;
}

void deserialize_header(const uint8_t* buf, Header& h) {
    h.magic = static_cast<uint16_t>(
        (static_cast<uint16_t>(buf[0]) << 8) | buf[1]
    );

    h.session_id =
        (static_cast<uint32_t>(buf[2]) << 24) |
        (static_cast<uint32_t>(buf[3]) << 16) |
        (static_cast<uint32_t>(buf[4]) <<  8) |
         static_cast<uint32_t>(buf[5]);

    h.cmd_type = static_cast<CmdType>(buf[6]);

    h.body_length =
        (static_cast<uint32_t>(buf[7])  << 24) |
        (static_cast<uint32_t>(buf[8])  << 16) |
        (static_cast<uint32_t>(buf[9])  <<  8) |
         static_cast<uint32_t>(buf[10]);

    h.reserved = static_cast<uint16_t>(
        (static_cast<uint16_t>(buf[11]) << 8) | buf[12]
    );
}
