// M4 end-to-end test: spawn apphost_bin as a child process, have it load
// libdemo_app.so via the fake IUiHost, and observe APPHOST_READY +
// PIXEL_DATA packets arriving over a fake Gateway socket.
//
// Test layout matches the M3b reverse-connect convention:
//   1. fake Gateway picks a free port, listen()
//   2. fork + exec apphost_bin with --gateway-host/port/session-id/plugin
//   3. accept the inbound AppHost connection
//   4. recv APPHOST_READY → recv full-frame PIXEL_DATA → recv dirty-rect
//      PIXEL_DATA
//   5. send CLOSE_SESSION; waitpid the child for graceful exit

#include <gtest/gtest.h>

#include "src/protocol/message_parser.h"
#include "src/protocol/protocol.h"

#include <arpa/inet.h>
#include <cerrno>
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

// Demo plugin geometry — must match plugins/demo_app/demo_app.cpp constants.
constexpr int kWindowWidth   = 32;
constexpr int kWindowHeight  = 16;
constexpr int kSmallLeft     = 8;
constexpr int kSmallTop      = 4;
constexpr int kSmallRight    = 16;
constexpr int kSmallBottom   = 12;

constexpr uint32_t kSessionId = 0xC0FFEEu;

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

uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) <<  8) |
            static_cast<uint32_t>(p[3]);
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

std::vector<uint8_t> serialize_close_session(uint32_t session_id) {
    Header hdr{};
    hdr.magic       = PROTOCOL_MAGIC;
    hdr.session_id  = session_id;
    hdr.cmd_type    = CmdType::CLOSE_SESSION;
    hdr.body_length = 0;
    std::vector<uint8_t> out(HEADER_SIZE);
    serialize_header(hdr, out.data());
    return out;
}

// Bind a TCP listener on `port`, mark it listen-ready, and return its fd.
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

// Read packets from `sock` until either we have `wanted_count` packets that
// pass `pred`, or recv fails.  The returned vector contains every packet
// received while waiting.
struct PacketRecv {
    MessageParser parser;
    int           sock;

    bool recv_one(Packet& out) {
        uint8_t buf[4096];
        while (true) {
            ParseResult r = parser.next_packet(out);
            if (r == ParseResult::OK) return true;
            timeval tv{5, 0};
            ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ssize_t n = ::recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            parser.feed(buf, static_cast<size_t>(n));
        }
    }
};

// Helper: fork+exec apphost_bin.  Returns the child's pid (0 on failure).
pid_t spawn_apphost(const std::string& apphost_bin,
                    const std::string& plugin_path,
                    uint16_t port,
                    uint32_t session_id) {
    pid_t pid = ::fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        std::string a_host    = "--gateway-host=127.0.0.1";
        std::string a_port    = "--gateway-port=" + std::to_string(static_cast<unsigned>(port));
        std::string a_session = "--session-id=" +
                                std::to_string(static_cast<unsigned long long>(session_id));
        std::string a_plugin  = "--plugin=" + plugin_path;
        std::string a_width   = "--width=" + std::to_string(kWindowWidth);
        std::string a_height  = "--height=" + std::to_string(kWindowHeight);

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(apphost_bin.c_str()));
        argv.push_back(const_cast<char*>(a_session.c_str()));
        argv.push_back(const_cast<char*>(a_host.c_str()));
        argv.push_back(const_cast<char*>(a_port.c_str()));
        argv.push_back(const_cast<char*>(a_plugin.c_str()));
        argv.push_back(const_cast<char*>(a_width.c_str()));
        argv.push_back(const_cast<char*>(a_height.c_str()));
        argv.push_back(nullptr);
        ::execv(apphost_bin.c_str(), argv.data());
        ::_exit(127);
    }
    return pid;
}

// Wait for `pid` to exit, with a soft deadline of `timeout_ms`.  Returns
// true with `*exit_code` set on clean exit; false otherwise.
bool wait_child(pid_t pid, int timeout_ms, int* exit_code) {
    const int step_ms = 25;
    int waited = 0;
    while (waited < timeout_ms) {
        int status = 0;
        pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (exit_code != nullptr && WIFEXITED(status))
                *exit_code = WEXITSTATUS(status);
            return WIFEXITED(status);
        }
        ::usleep(step_ms * 1000);
        waited += step_ms;
    }
    // Force-kill so the test doesn't leak a process.
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    return false;
}

}  // namespace

class M4PluginEndToEndTest : public ::testing::Test {
protected:
    pid_t          child_pid_     = 0;
    int            listen_fd_     = -1;
    int            apphost_sock_  = -1;
    PacketRecv     recv_{};

    std::string apphost_bin_path_;
    std::string plugin_so_path_;

    void SetUp() override {
        apphost_bin_path_ = runfile_path("src/apphost/apphost_bin");
        plugin_so_path_   = runfile_path("plugins/demo_app/libdemo_app.so");

        const uint16_t port = pick_free_port();
        listen_fd_ = start_listener(port);
        ASSERT_GE(listen_fd_, 0) << "fake Gateway listen() failed";

        child_pid_ = spawn_apphost(apphost_bin_path_, plugin_so_path_, port, kSessionId);
        ASSERT_GT(child_pid_, 0) << "fork/exec apphost_bin failed";

        // Accept the inbound AppHost connection.
        timeval tv{5, 0};
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        sockaddr_in peer{};
        socklen_t   peer_len = sizeof(peer);
        apphost_sock_ = ::accept(listen_fd_,
                                 reinterpret_cast<sockaddr*>(&peer), &peer_len);
        ASSERT_GE(apphost_sock_, 0) << "accept() from AppHost failed";
        recv_.sock = apphost_sock_;
    }

    void TearDown() override {
        if (apphost_sock_ >= 0) ::close(apphost_sock_);
        if (listen_fd_    >= 0) ::close(listen_fd_);
        if (child_pid_    > 0) {
            // Best-effort: ensure the child is reaped if the test bailed early.
            int status = 0;
            if (::waitpid(child_pid_, &status, WNOHANG) == 0) {
                ::kill(child_pid_, SIGKILL);
                ::waitpid(child_pid_, nullptr, 0);
            }
        }
    }
};

TEST_F(M4PluginEndToEndTest, AppHostReadyThenTwoPixelFramesThenClose) {
    // 1. APPHOST_READY
    Packet pkt;
    ASSERT_TRUE(recv_.recv_one(pkt)) << "no APPHOST_READY received";
    EXPECT_EQ(pkt.header.cmd_type,   CmdType::APPHOST_READY);
    EXPECT_EQ(pkt.header.session_id, kSessionId);
    EXPECT_EQ(pkt.header.body_length, 0u);

    // 2. Initial full-frame PIXEL_DATA
    ASSERT_TRUE(recv_.recv_one(pkt)) << "no initial PIXEL_DATA received";
    ASSERT_EQ(pkt.header.cmd_type, CmdType::PIXEL_DATA);
    ASSERT_GE(pkt.body.size(), 24u);

    const uint32_t seq1   = read_be32(pkt.body.data() +  0);
    const uint32_t left1  = read_be32(pkt.body.data() +  4);
    const uint32_t top1   = read_be32(pkt.body.data() +  8);
    const uint32_t right1 = read_be32(pkt.body.data() + 12);
    const uint32_t bot1   = read_be32(pkt.body.data() + 16);
    const uint32_t dlen1  = read_be32(pkt.body.data() + 20);

    EXPECT_EQ(left1,  0u);
    EXPECT_EQ(top1,   0u);
    EXPECT_EQ(right1, static_cast<uint32_t>(kWindowWidth));
    EXPECT_EQ(bot1,   static_cast<uint32_t>(kWindowHeight));
    EXPECT_EQ(dlen1,  static_cast<uint32_t>(kWindowWidth * kWindowHeight * 4));
    ASSERT_EQ(pkt.body.size(), 24u + dlen1);

    // Verify the pixel pattern: blue background + red small rect.
    const uint8_t* px1 = pkt.body.data() + 24;
    int blue = 0, red = 0, other = 0;
    for (int y = 0; y < kWindowHeight; ++y) {
        for (int x = 0; x < kWindowWidth; ++x) {
            const uint8_t* p = px1 + (y * kWindowWidth + x) * 4;
            const bool inside = (x >= kSmallLeft && x < kSmallRight &&
                                 y >= kSmallTop  && y < kSmallBottom);
            // BLUE on wire: A=FF R=00 G=00 B=FF. RED: A=FF R=FF G=00 B=00.
            if (!inside) {
                if (p[0] == 0xFF && p[1] == 0x00 && p[2] == 0x00 && p[3] == 0xFF) ++blue;
                else ++other;
            } else {
                if (p[0] == 0xFF && p[1] == 0xFF && p[2] == 0x00 && p[3] == 0x00) ++red;
                else ++other;
            }
        }
    }
    EXPECT_EQ(other, 0) << "unexpected pixel colors in initial frame";
    EXPECT_EQ(red,   (kSmallRight - kSmallLeft) * (kSmallBottom - kSmallTop));
    EXPECT_EQ(blue,  kWindowWidth * kWindowHeight - red);

    // 3. Dirty-rect PIXEL_DATA (small_rect, all green).
    ASSERT_TRUE(recv_.recv_one(pkt)) << "no dirty PIXEL_DATA received";
    ASSERT_EQ(pkt.header.cmd_type, CmdType::PIXEL_DATA);
    ASSERT_GE(pkt.body.size(), 24u);

    const uint32_t seq2   = read_be32(pkt.body.data() +  0);
    const uint32_t left2  = read_be32(pkt.body.data() +  4);
    const uint32_t top2   = read_be32(pkt.body.data() +  8);
    const uint32_t right2 = read_be32(pkt.body.data() + 12);
    const uint32_t bot2   = read_be32(pkt.body.data() + 16);
    const uint32_t dlen2  = read_be32(pkt.body.data() + 20);

    EXPECT_EQ(left2,  static_cast<uint32_t>(kSmallLeft));
    EXPECT_EQ(top2,   static_cast<uint32_t>(kSmallTop));
    EXPECT_EQ(right2, static_cast<uint32_t>(kSmallRight));
    EXPECT_EQ(bot2,   static_cast<uint32_t>(kSmallBottom));
    EXPECT_EQ(dlen2,  static_cast<uint32_t>((kSmallRight - kSmallLeft) *
                                            (kSmallBottom - kSmallTop) * 4));
    ASSERT_EQ(pkt.body.size(), 24u + dlen2);

    // frame_seq strictly increases across the two PIXEL_DATA packets.
    EXPECT_GT(seq2, seq1);

    // Dirty rect: demo_app now paints GREEN for the second DoDraw so
    // the dirty-rect PIXEL_DATA carries different content than the
    // initial full-frame PIXEL_DATA (which was RED).
    //
    // GREEN on wire: A=FF R=00 G=FF B=00
    const uint8_t* px2 = pkt.body.data() + 24;
    const int sw = kSmallRight - kSmallLeft;
    const int sh = kSmallBottom - kSmallTop;
    int g_count = 0, other2 = 0;
    for (int i = 0; i < sw * sh; ++i) {
        const uint8_t* p = px2 + i * 4;
        if (p[0] == 0xFF && p[1] == 0x00 && p[2] == 0xFF && p[3] == 0x00) ++g_count;
        else ++other2;
    }
    EXPECT_EQ(other2,  0);
    EXPECT_EQ(g_count, sw * sh);

    // 4. Tell AppHost to shut down and verify it exits cleanly within 2s.
    auto close_pkt = serialize_close_session(kSessionId);
    ASSERT_TRUE(send_all(apphost_sock_, close_pkt.data(), close_pkt.size()));

    int exit_code = -1;
    EXPECT_TRUE(wait_child(child_pid_, /*timeout_ms=*/3000, &exit_code))
        << "AppHost did not exit within 3s after CLOSE_SESSION";
    child_pid_ = 0;  // already reaped
    EXPECT_EQ(exit_code, 0) << "AppHost exited with non-zero code";
}
