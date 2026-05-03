#pragma once
#include <cstdint>
#include <vector>
#include <cstddef>

// ── CmdType ──────────────────────────────────────────────────────────────────

enum class CmdType : uint8_t {
    HEARTBEAT     = 0x01,  // Keep-alive. Body must be empty (body_length == 0).
    SPAWN_APP     = 0x02,  // Client → Gateway: request to launch an AppHost process.
    SESSION_ACK   = 0x03,  // Gateway → Client: session allocated; header.session_id carries the id.
    APPHOST_READY = 0x04,  // AppHost → Gateway: bind this connection to session_id.
    PIXEL_DATA    = 0x10,  // AppHost → Client: dirty-rectangle pixel data.
    INPUT_EVENT   = 0x20,  // Client → AppHost: mouse or keyboard event.
    CLOSE_SESSION = 0xFE,  // Either direction: graceful session teardown.
    ERROR_RESP    = 0xFF,  // Either direction: protocol-level error notification.
};

// ── Header ───────────────────────────────────────────────────────────────────

// Logical representation of the 13-byte wire header.
// On-wire layout (big-endian): Magic(2) | SessionID(4) | CmdType(1) | BodyLength(4) | Reserved(2)
struct Header {
    uint16_t magic;        // Must be PROTOCOL_MAGIC (0xBEEF).
    uint32_t session_id;
    CmdType  cmd_type;
    uint32_t body_length;
    uint16_t reserved;     // Always 0; ignored on read.
};

static constexpr uint16_t PROTOCOL_MAGIC       = 0xBEEF;
static constexpr size_t   HEADER_SIZE          = 13;
static constexpr uint32_t MAX_BODY_LENGTH      = 64u * 1024u * 1024u;  // 64 MB

// ── Packet ───────────────────────────────────────────────────────────────────

struct Packet {
    Header               header;
    std::vector<uint8_t> body;   // Length == header.body_length. Empty for HEARTBEAT.
};

// ── Serialization ────────────────────────────────────────────────────────────

// Serializes header into HEADER_SIZE (13) bytes of big-endian network byte order.
// buf must have at least HEADER_SIZE bytes available for writing.
void serialize_header(const Header& header, uint8_t* buf);

// Deserializes HEADER_SIZE (13) bytes from buf into header.
// buf must have at least HEADER_SIZE bytes available for reading.
void deserialize_header(const uint8_t* buf, Header& header);
