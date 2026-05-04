#include <gtest/gtest.h>

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

constexpr int kWindowWidth = 32;
constexpr int kWindowHeight = 16;
constexpr int kSmallLeft = 8;
constexpr int kSmallTop = 4;
constexpr int kSmallRight = 16;
constexpr int kSmallBottom = 12;

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
    const char* workspace = std::getenv("TEST_WORKSPACE");
    if (!test_srcdir || !workspace)
        return rel;
    return std::string(test_srcdir) + "/" + workspace + "/" + rel;
}

uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
            static_cast<uint32_t>(p[3]);
}

bool send_all(int sock, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(sock, data + sent, len - sent, 0);
        if (n <= 0)
            return false;
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
    body[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
    body[3] = static_cast<uint8_t>(len & 0xFF);
    std::memcpy(body.data() + 4, name.data(), name.size());
    return make_packet(CmdType::SPAWN_APP, 0, body);
}

int connect_to(uint16_t port) {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

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
    ~TestClient() {
        if (sock_ >= 0)
            ::close(sock_);
    }

    int sock() const { return sock_; }

    bool send_packet(const std::vector<uint8_t>& packet) {
        return send_all(sock_, packet.data(), packet.size());
    }

    bool recv_one_packet(Packet* out) {
        uint8_t buf[4096];
        while (true) {
            if (parser_.next_packet(*out) == ParseResult::OK)
                return true;
            ssize_t n = ::recv(sock_, buf, sizeof(buf), 0);
            if (n <= 0)
                return false;
            parser_.feed(buf, static_cast<size_t>(n));
        }
    }

private:
    int sock_;
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

void expect_full_demo_frame(const Packet& pkt, uint32_t session_id) {
    ASSERT_EQ(pkt.header.cmd_type, CmdType::PIXEL_DATA);
    EXPECT_EQ(pkt.header.session_id, session_id);
    ASSERT_GE(pkt.body.size(), 24u);

    const uint32_t left = read_be32(pkt.body.data() + 4);
    const uint32_t top = read_be32(pkt.body.data() + 8);
    const uint32_t right = read_be32(pkt.body.data() + 12);
    const uint32_t bottom = read_be32(pkt.body.data() + 16);
    const uint32_t data_len = read_be32(pkt.body.data() + 20);

    EXPECT_EQ(left, 0u);
    EXPECT_EQ(top, 0u);
    EXPECT_EQ(right, static_cast<uint32_t>(kWindowWidth));
    EXPECT_EQ(bottom, static_cast<uint32_t>(kWindowHeight));
    EXPECT_EQ(data_len, static_cast<uint32_t>(kWindowWidth * kWindowHeight * 4));
    ASSERT_EQ(pkt.body.size(), 24u + data_len);

    const uint8_t* px = pkt.body.data() + 24;
    int blue = 0;
    int red = 0;
    int other = 0;
    for (int y = 0; y < kWindowHeight; ++y) {
        for (int x = 0; x < kWindowWidth; ++x) {
            const uint8_t* p = px + (y * kWindowWidth + x) * 4;
            const bool inside = x >= kSmallLeft && x < kSmallRight &&
                                y >= kSmallTop && y < kSmallBottom;
            if (inside) {
                if (p[0] == 0xFF && p[1] == 0xFF && p[2] == 0x00 && p[3] == 0x00)
                    ++red;
                else
                    ++other;
            } else {
                if (p[0] == 0xFF && p[1] == 0x00 && p[2] == 0x00 && p[3] == 0xFF)
                    ++blue;
                else
                    ++other;
            }
        }
    }
    EXPECT_EQ(other, 0);
    EXPECT_EQ(red, (kSmallRight - kSmallLeft) * (kSmallBottom - kSmallTop));
    EXPECT_EQ(blue, kWindowWidth * kWindowHeight - red);
}

void expect_dirty_demo_frame(const Packet& pkt, uint32_t session_id) {
    ASSERT_EQ(pkt.header.cmd_type, CmdType::PIXEL_DATA);
    EXPECT_EQ(pkt.header.session_id, session_id);
    ASSERT_GE(pkt.body.size(), 24u);

    const uint32_t left = read_be32(pkt.body.data() + 4);
    const uint32_t top = read_be32(pkt.body.data() + 8);
    const uint32_t right = read_be32(pkt.body.data() + 12);
    const uint32_t bottom = read_be32(pkt.body.data() + 16);
    const uint32_t data_len = read_be32(pkt.body.data() + 20);
    const int width = kSmallRight - kSmallLeft;
    const int height = kSmallBottom - kSmallTop;

    EXPECT_EQ(left, static_cast<uint32_t>(kSmallLeft));
    EXPECT_EQ(top, static_cast<uint32_t>(kSmallTop));
    EXPECT_EQ(right, static_cast<uint32_t>(kSmallRight));
    EXPECT_EQ(bottom, static_cast<uint32_t>(kSmallBottom));
    EXPECT_EQ(data_len, static_cast<uint32_t>(width * height * 4));
    ASSERT_EQ(pkt.body.size(), 24u + data_len);

    const uint8_t* px = pkt.body.data() + 24;
    for (int i = 0; i < width * height; ++i) {
        const uint8_t* p = px + i * 4;
        EXPECT_EQ(p[0], 0xFF);
        EXPECT_EQ(p[1], 0x00);
        EXPECT_EQ(p[2], 0xFF);
        EXPECT_EQ(p[3], 0x00);
    }
}

} // namespace

TEST(GatewayM4PluginTest, AutoSpawnedAppHostForwardsDemoPluginPixels) {
    Gateway::Config cfg = make_config(
        runfile_path("src/apphost/apphost_bin"),
        runfile_path("plugins/demo_app/libdemo_app.so"));

    Gateway gateway;
    ASSERT_TRUE(gateway.start(cfg));

    TestClient client(cfg.public_port);
    ASSERT_GE(client.sock(), 0);
    ASSERT_TRUE(client.send_packet(make_spawn_app("demo_app")));

    uint32_t session_id = 0;
    std::vector<Packet> pixels;
    for (int i = 0; i < 4 && pixels.size() < 2; ++i) {
        Packet pkt;
        ASSERT_TRUE(client.recv_one_packet(&pkt));
        if (pkt.header.cmd_type == CmdType::SESSION_ACK) {
            session_id = pkt.header.session_id;
            EXPECT_NE(session_id, 0u);
            continue;
        }
        if (pkt.header.cmd_type == CmdType::PIXEL_DATA) {
            pixels.push_back(std::move(pkt));
            continue;
        }
        FAIL() << "unexpected packet type "
               << static_cast<int>(pkt.header.cmd_type);
    }

    ASSERT_NE(session_id, 0u);
    ASSERT_EQ(pixels.size(), 2u);
    expect_full_demo_frame(pixels[0], session_id);
    expect_dirty_demo_frame(pixels[1], session_id);

    ASSERT_TRUE(client.send_packet(
        make_packet(CmdType::CLOSE_SESSION, session_id)));

    gateway.stop();
}
