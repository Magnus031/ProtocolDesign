// M5 Gateway-coupled integration test.
//
// Builds on gateway_m4_plugin_test.cpp: a real Gateway with auto-spawn
// turned on, a fake Client connecting to its public port, and the demo_app
// plugin loaded by the AppHost the Gateway forks for us.  Once the M4
// startup path (SPAWN_APP -> SESSION_ACK -> two PIXEL_DATA frames) is
// drained, the fake Client sends an INPUT_EVENT and we expect the AppHost
// to respond with a new input-driven PIXEL_DATA frame routed back through
// the Gateway.
//
// What this proves over m5_input_test:
//   - Gateway forwards INPUT_EVENT Body bytes verbatim by SessionID
//     (i.e. it does not parse the body, but routing still works)
//   - The session_id allocated by SESSION_ACK is the one AppHost
//     ultimately receives — public/internal port binding is intact

#include <gtest/gtest.h>

#include "src/common/input_event.h"
#include "src/gateway/gateway.h"
#include "src/protocol/message_parser.h"
#include "src/protocol/protocol.h"

#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kWindowWidth  = 32;
constexpr int kWindowHeight = 16;

// KB_F1 from third_party/GKC/public/include/base/system/ui_types.h.
constexpr uint16_t kKeyF1 = 0x70;

uint16_t pick_free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    ::close(fd);
    return ntohs(addr.sin_port);
}

std::string runfile_path(const std::string& rel) {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* workspace   = std::getenv("TEST_WORKSPACE");
    if (!test_srcdir || !workspace) return rel;
    return std::string(test_srcdir) + "/" + workspace + "/" + rel;
}

uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) <<  8) |
            static_cast<uint32_t>(p[3]);
}

bool send_all(int sock, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(sock, data + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

std::vector<uint8_t> make_packet(CmdType cmd, uint32_t session_id,
                                 const std::vector<uint8_t>& body = {}) {
    Header hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.session_id = session_id;
    hdr.cmd_type = cmd;
    hdr.body_length = static_cast<uint32_t>(body.size());
    std::vector<uint8_t> out(HEADER_SIZE + body.size());
    serialize_header(hdr, out.data());
    if (!body.empty())
        std::memcpy(out.data() + HEADER_SIZE, body.data(), body.size());
    return out;
}

std::vector<uint8_t> make_spawn_app(const std::string& name) {
    const uint32_t len = static_cast<uint32_t>(name.size());
    std::vector<uint8_t> body(4 + name.size());
    body[0] = static_cast<uint8_t>((len >> 24) & 0xFF);
    body[1] = static_cast<uint8_t>((len >> 16) & 0xFF);
    body[2] = static_cast<uint8_t>((len >>  8) & 0xFF);
    body[3] = static_cast<uint8_t>( len        & 0xFF);
    std::memcpy(body.data() + 4, name.data(), name.size());
    return make_packet(CmdType::SPAWN_APP, 0, body);
}

int connect_to(uint16_t port) {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    timeval tv{5, 0};
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_aton("127.0.0.1", &addr.sin_addr);
    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(sock);
        return -1;
    }
    return sock;
}

class TestClient {
public:
    explicit TestClient(uint16_t port) : sock_(connect_to(port)) {}
    ~TestClient() { if (sock_ >= 0) ::close(sock_); }

    int sock() const { return sock_; }
    bool send_packet(const std::vector<uint8_t>& packet) {
        return send_all(sock_, packet.data(), packet.size());
    }
    bool recv_one_packet(Packet* out) {
        uint8_t buf[4096];
        while (true) {
            if (parser_.next_packet(*out) == ParseResult::OK) return true;
            ssize_t n = ::recv(sock_, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            parser_.feed(buf, static_cast<size_t>(n));
        }
    }

private:
    int           sock_;
    MessageParser parser_;
};

Gateway::Config make_config(const std::string& apphost_bin,
                            const std::string& plugin_path) {
    Gateway::Config cfg{};
    cfg.public_port = pick_free_port();
    cfg.apphost_internal_port = pick_free_port();
    cfg.apphost_bind_host = "127.0.0.1";
    cfg.apphost_bin = apphost_bin;
    cfg.auto_spawn_apphost = true;
    cfg.spawn_timeout_ms = 2000;
    cfg.monitor_interval_ms = 50;
    cfg.client_timeout_ms = 5000;
    cfg.apphost_timeout_ms = 5000;
    cfg.app_allowlist["demo_app"] = Gateway::AppEntry{
        plugin_path, kWindowWidth, kWindowHeight};
    return cfg;
}

}  // namespace

TEST(GatewayM5InputTest, GatewayRoutesInputEventToAppHostAndPixelsBack) {
    Gateway::Config cfg = make_config(
        runfile_path("src/apphost/apphost_bin"),
        runfile_path("plugins/demo_app/libdemo_app.so"));

    Gateway gateway;
    ASSERT_TRUE(gateway.start(cfg));

    TestClient client(cfg.public_port);
    ASSERT_GE(client.sock(), 0);
    ASSERT_TRUE(client.send_packet(make_spawn_app("demo_app")));

    // Drain SESSION_ACK + the two M4 startup PIXEL_DATA frames.
    uint32_t session_id = 0;
    int      pixel_seen = 0;
    uint32_t last_seq   = 0;
    for (int i = 0; i < 6 && pixel_seen < 2; ++i) {
        Packet pkt;
        ASSERT_TRUE(client.recv_one_packet(&pkt));
        if (pkt.header.cmd_type == CmdType::SESSION_ACK) {
            session_id = pkt.header.session_id;
            EXPECT_NE(session_id, 0u);
            continue;
        }
        if (pkt.header.cmd_type == CmdType::PIXEL_DATA) {
            ASSERT_GE(pkt.body.size(), 24u);
            last_seq = read_be32(pkt.body.data() + 0);
            ++pixel_seen;
            continue;
        }
        FAIL() << "unexpected packet during startup: "
               << static_cast<int>(pkt.header.cmd_type);
    }
    ASSERT_NE(session_id, 0u);
    ASSERT_EQ(pixel_seen, 2);

    // Send an F1 KEY_DOWN through the Gateway.  The Gateway must route the
    // packet to the AppHost by SessionID without inspecting the body.
    auto key_packet = make_packet(CmdType::INPUT_EVENT, session_id,
                                  pack_keyboard_input_event(InputEventType::KEY_DOWN,
                                                            kKeyF1, 0));
    ASSERT_TRUE(client.send_packet(key_packet));

    // Expect a new full-window PIXEL_DATA caused by the F1 toggle.
    Packet pkt;
    ASSERT_TRUE(client.recv_one_packet(&pkt));
    ASSERT_EQ(pkt.header.cmd_type, CmdType::PIXEL_DATA);
    EXPECT_EQ(pkt.header.session_id, session_id);
    ASSERT_GE(pkt.body.size(), 24u);

    EXPECT_GT(read_be32(pkt.body.data() + 0), last_seq);
    EXPECT_EQ(read_be32(pkt.body.data() +  4), 0u);
    EXPECT_EQ(read_be32(pkt.body.data() +  8), 0u);
    EXPECT_EQ(read_be32(pkt.body.data() + 12), static_cast<uint32_t>(kWindowWidth));
    EXPECT_EQ(read_be32(pkt.body.data() + 16), static_cast<uint32_t>(kWindowHeight));
    const uint32_t dlen = read_be32(pkt.body.data() + 20);
    ASSERT_EQ(dlen, static_cast<uint32_t>(kWindowWidth * kWindowHeight * 4));
    ASSERT_EQ(pkt.body.size(), 24u + dlen);

    // Background must be CYAN; small rect must be GREEN (M4 startup latched it
    // before the toggle).  Spot-check a corner pixel and the small-rect centre.
    const uint8_t* px = pkt.body.data() + 24;
    const uint8_t* corner    = px + (0 * kWindowWidth + 0) * 4;
    const uint8_t* sm_center = px + (8 * kWindowWidth + 12) * 4;
    EXPECT_EQ(corner[0], 0xFFu); EXPECT_EQ(corner[1], 0x00u);
    EXPECT_EQ(corner[2], 0xFFu); EXPECT_EQ(corner[3], 0xFFu);
    EXPECT_EQ(sm_center[0], 0xFFu); EXPECT_EQ(sm_center[1], 0x00u);
    EXPECT_EQ(sm_center[2], 0xFFu); EXPECT_EQ(sm_center[3], 0x00u);

    ASSERT_TRUE(client.send_packet(
        make_packet(CmdType::CLOSE_SESSION, session_id)));

    gateway.stop();
}
