// Lifecycle test for the M4 apphost binary.  Inherits the network-level
// coverage that the old (M2) apphost_test had — connection, HEARTBEAT echo,
// CLOSE_SESSION exit, multi-packet ordering — but exercises the new
// reverse-connect (AppHost-as-client, fake Gateway-as-server) topology.

#include <gtest/gtest.h>

#include "src/protocol/message_parser.h"
#include "src/protocol/protocol.h"

#include <arpa/inet.h>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint32_t kSessionId = 0xA1B2C3u;

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
    if (test_srcdir == nullptr || workspace == nullptr)
        return rel;
    return std::string(test_srcdir) + "/" + workspace + "/" + rel;
}

bool send_all(int sock, const uint8_t* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(sock, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

std::vector<uint8_t> serialize_zero_body(CmdType cmd, uint32_t session_id) {
    Header hdr{};
    hdr.magic       = PROTOCOL_MAGIC;
    hdr.session_id  = session_id;
    hdr.cmd_type    = cmd;
    hdr.body_length = 0;
    std::vector<uint8_t> out(HEADER_SIZE);
    serialize_header(hdr, out.data());
    return out;
}

int start_listener(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(fd, 1) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

pid_t spawn_apphost(const std::string& bin, const std::string& plugin,
                    uint16_t port, uint32_t session_id) {
    pid_t pid = ::fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        std::string a_host    = "--gateway-host=127.0.0.1";
        std::string a_port    = "--gateway-port=" + std::to_string(static_cast<unsigned>(port));
        std::string a_session = "--session-id=" +
                                std::to_string(static_cast<unsigned long long>(session_id));
        std::string a_plugin  = "--plugin=" + plugin;
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(bin.c_str()));
        argv.push_back(const_cast<char*>(a_session.c_str()));
        argv.push_back(const_cast<char*>(a_host.c_str()));
        argv.push_back(const_cast<char*>(a_port.c_str()));
        argv.push_back(const_cast<char*>(a_plugin.c_str()));
        argv.push_back(nullptr);
        ::execv(bin.c_str(), argv.data());
        ::_exit(127);
    }
    return pid;
}

bool wait_child(pid_t pid, int timeout_ms, int* exit_code) {
    const int step_ms = 25;
    int waited = 0;
    while (waited < timeout_ms) {
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid) {
            if (exit_code != nullptr && WIFEXITED(status))
                *exit_code = WEXITSTATUS(status);
            return WIFEXITED(status);
        }
        ::usleep(step_ms * 1000);
        waited += step_ms;
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    return false;
}

// Read packets one at a time with a 5-second recv timeout.  Returns false
// on EOF / error / timeout.
struct PacketRecv {
    MessageParser parser;
    int           sock = -1;

    bool recv_one(Packet& out) {
        uint8_t buf[4096];
        while (true) {
            if (parser.next_packet(out) == ParseResult::OK) return true;
            timeval tv{5, 0};
            ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ssize_t n = ::recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            parser.feed(buf, static_cast<size_t>(n));
        }
    }

    // Drain packets and return the first one whose CmdType matches `want`.
    // Useful when other packet types (e.g. PIXEL_DATA) interleave with the
    // packet under test.
    bool recv_until(CmdType want, Packet& out, int max_packets = 10) {
        for (int i = 0; i < max_packets; ++i) {
            if (!recv_one(out)) return false;
            if (out.header.cmd_type == want) return true;
        }
        return false;
    }
};

}  // namespace

class AppHostLifecycleTest : public ::testing::Test {
protected:
    pid_t      child_pid_    = 0;
    int        listen_fd_    = -1;
    int        sock_         = -1;
    PacketRecv recv_{};

    void SetUp() override {
        const std::string bin    = runfile_path("src/apphost/apphost_bin");
        const std::string plugin = runfile_path("plugins/demo_app/libdemo_app.so");

        const uint16_t port = pick_free_port();
        listen_fd_ = start_listener(port);
        ASSERT_GE(listen_fd_, 0);

        child_pid_ = spawn_apphost(bin, plugin, port, kSessionId);
        ASSERT_GT(child_pid_, 0);

        timeval tv{5, 0};
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        sockaddr_in peer{};
        socklen_t   plen = sizeof(peer);
        sock_ = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
        ASSERT_GE(sock_, 0);
        recv_.sock = sock_;
    }

    void TearDown() override {
        if (sock_       >= 0) ::close(sock_);
        if (listen_fd_  >= 0) ::close(listen_fd_);
        if (child_pid_  > 0) {
            int status = 0;
            if (::waitpid(child_pid_, &status, WNOHANG) == 0) {
                ::kill(child_pid_, SIGKILL);
                ::waitpid(child_pid_, nullptr, 0);
            }
        }
    }

    bool send_close() {
        auto p = serialize_zero_body(CmdType::CLOSE_SESSION, kSessionId);
        return send_all(sock_, p.data(), p.size());
    }
};

// AppHost connects out, fake Gateway accepts, AppHost sends APPHOST_READY
// with the configured session_id and an empty body.
TEST_F(AppHostLifecycleTest, ConnectAndReady) {
    Packet pkt;
    ASSERT_TRUE(recv_.recv_one(pkt));
    EXPECT_EQ(pkt.header.cmd_type,    CmdType::APPHOST_READY);
    EXPECT_EQ(pkt.header.session_id,  kSessionId);
    EXPECT_EQ(pkt.header.body_length, 0u);
    EXPECT_TRUE(pkt.body.empty());

    ASSERT_TRUE(send_close());
    int rc = -1;
    EXPECT_TRUE(wait_child(child_pid_, 3000, &rc));
    child_pid_ = 0;
}

// HEARTBEAT sent from the fake Gateway is echoed back by AppHost with the
// original session_id and an empty body (per protocol spec).
TEST_F(AppHostLifecycleTest, HeartbeatEcho) {
    // Drain APPHOST_READY first.
    Packet pkt;
    ASSERT_TRUE(recv_.recv_one(pkt));
    ASSERT_EQ(pkt.header.cmd_type, CmdType::APPHOST_READY);

    // Send a heartbeat and look for an echo (skipping PIXEL_DATA frames
    // that demo_app emits during the same period).
    auto hb = serialize_zero_body(CmdType::HEARTBEAT, kSessionId);
    ASSERT_TRUE(send_all(sock_, hb.data(), hb.size()));

    Packet echo;
    ASSERT_TRUE(recv_.recv_until(CmdType::HEARTBEAT, echo))
        << "no HEARTBEAT echo received";
    EXPECT_EQ(echo.header.session_id,  kSessionId);
    EXPECT_EQ(echo.header.body_length, 0u);

    ASSERT_TRUE(send_close());
    int rc = -1;
    EXPECT_TRUE(wait_child(child_pid_, 3000, &rc));
    child_pid_ = 0;
}

// CLOSE_SESSION causes the AppHost child to exit cleanly within the
// shutdown grace.
TEST_F(AppHostLifecycleTest, CloseSessionExits) {
    Packet pkt;
    ASSERT_TRUE(recv_.recv_one(pkt));   // APPHOST_READY
    ASSERT_EQ(pkt.header.cmd_type, CmdType::APPHOST_READY);

    ASSERT_TRUE(send_close());

    int rc = -1;
    EXPECT_TRUE(wait_child(child_pid_, 3000, &rc))
        << "AppHost did not exit within 3s after CLOSE_SESSION";
    EXPECT_EQ(rc, 0);
    child_pid_ = 0;
}

// Multi-packet ordering: the demo plugin emits two PIXEL_DATA frames after
// startup; verify both arrive in frame_seq order without truncation or
// reordering.  Substitutes for the M2 send-queue regression test.
TEST_F(AppHostLifecycleTest, MultiPacketOrdering) {
    Packet pkt;
    ASSERT_TRUE(recv_.recv_one(pkt));   // APPHOST_READY

    Packet p1, p2;
    ASSERT_TRUE(recv_.recv_until(CmdType::PIXEL_DATA, p1));
    ASSERT_TRUE(recv_.recv_until(CmdType::PIXEL_DATA, p2));

    auto seq_of = [](const Packet& p) -> uint32_t {
        return (static_cast<uint32_t>(p.body[0]) << 24) |
               (static_cast<uint32_t>(p.body[1]) << 16) |
               (static_cast<uint32_t>(p.body[2]) <<  8) |
                static_cast<uint32_t>(p.body[3]);
    };
    EXPECT_GT(seq_of(p2), seq_of(p1));
    EXPECT_GE(p1.body.size(), 24u);
    EXPECT_GE(p2.body.size(), 24u);

    ASSERT_TRUE(send_close());
    int rc = -1;
    EXPECT_TRUE(wait_child(child_pid_, 3000, &rc));
    child_pid_ = 0;
}
