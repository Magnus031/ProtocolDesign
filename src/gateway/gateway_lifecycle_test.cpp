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

    bool send_packet(const std::vector<uint8_t>& pkt) {
        return send_all(sock_, pkt.data(), pkt.size());
    }

    Packet recv_one_packet() {
        uint8_t buf[1024];
        Packet pkt;
        while (true) {
            if (parser_.next_packet(pkt) == ParseResult::OK)
                return pkt;
            ssize_t n = ::recv(sock_, buf, sizeof(buf), 0);
            if (n <= 0) {
                pkt.header.cmd_type = CmdType::ERROR_RESP;
                return pkt;
            }
            parser_.feed(buf, static_cast<size_t>(n));
        }
    }

private:
    int sock_;
    MessageParser parser_;
};

Gateway::Config make_config(const std::string& apphost_bin) {
    Gateway::Config cfg{};
    cfg.public_port = pick_free_port();
    cfg.apphost_internal_port = pick_free_port();
    cfg.apphost_bind_host = "127.0.0.1";
    cfg.apphost_bin = apphost_bin;
    cfg.auto_spawn_apphost = true;
    cfg.spawn_timeout_ms = 1000;
    cfg.monitor_interval_ms = 50;
    cfg.client_timeout_ms = 5000;
    cfg.apphost_timeout_ms = 5000;
    cfg.app_allowlist["demo_app"] = Gateway::AppEntry{"/bin/true", 800, 600};
    return cfg;
}

} // namespace

TEST(GatewayLifecycleTest, M3bLifecycle) {
    Gateway::Config cfg = make_config(runfile_path("src/apphost/apphost_bin"));
    Gateway gateway;
    ASSERT_TRUE(gateway.start(cfg));

    {
        TestClient client(cfg.public_port);
        ASSERT_GE(client.sock(), 0);
        ASSERT_TRUE(client.send_packet(make_spawn_app("demo_app")));

        Packet ack = client.recv_one_packet();
        EXPECT_EQ(ack.header.cmd_type, CmdType::SESSION_ACK);
        EXPECT_NE(ack.header.session_id, 0u);
    }

    {
        TestClient client(cfg.public_port);
        ASSERT_GE(client.sock(), 0);
        ASSERT_TRUE(client.send_packet(make_spawn_app("../bad")));

        Packet err = client.recv_one_packet();
        EXPECT_EQ(err.header.cmd_type, CmdType::ERROR_RESP);
    }

    gateway.stop();
}
