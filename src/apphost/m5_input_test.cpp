// M5 AppHost local integration test.
//
// Reuses the M4 fake-Gateway harness: pick a port, fork+exec apphost_bin with
// libdemo_app.so as its plugin, accept the inbound connection, then drain the
// startup APPHOST_READY + two PIXEL_DATA frames before sending INPUT_EVENT
// packets and asserting that the resulting plugin behaviour shows up on the
// wire as new PIXEL_DATA.
//
// What this exercises specifically:
//   - INPUT_EVENT body parsing in apphost.cpp
//   - UiHostImpl::post_mouse_input / post_keyboard_input owns-data queue
//   - PostWork drain on the plugin main thread
//   - DemoWindow::DoMouse / DoKeyboard -> Damage() -> dispatch_draw_and_send
//
// What this deliberately does NOT exercise:
//   - Real Gateway (covered by gateway_m5_input_test.cpp)
//   - Wheel-delta semantics (protocol does not yet carry one)

#include <gtest/gtest.h>

#include "src/common/input_event.h"
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

// Demo plugin geometry — must match plugins/demo_app/demo_app.cpp constants.
constexpr int kWindowWidth  = 32;
constexpr int kWindowHeight = 16;
constexpr int kSmallLeft    = 8;
constexpr int kSmallTop     = 4;
constexpr int kSmallRight   = 16;
constexpr int kSmallBottom  = 12;

// KB_F1 from third_party/GKC/public/include/base/system/ui_types.h.
constexpr uint16_t kKeyF1 = 0x70;

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

std::vector<uint8_t> serialize_packet(CmdType cmd, uint32_t session_id,
                                      const std::vector<uint8_t>& body) {
    Header hdr{};
    hdr.magic       = PROTOCOL_MAGIC;
    hdr.session_id  = session_id;
    hdr.cmd_type    = cmd;
    hdr.body_length = static_cast<uint32_t>(body.size());
    std::vector<uint8_t> out(HEADER_SIZE + body.size());
    serialize_header(hdr, out.data());
    if (!body.empty())
        std::memcpy(out.data() + HEADER_SIZE, body.data(), body.size());
    return out;
}

std::vector<uint8_t> close_session_packet() {
    return serialize_packet(CmdType::CLOSE_SESSION, kSessionId, {});
}

std::vector<uint8_t> mouse_event_packet(InputEventType type, int32_t x, int32_t y) {
    return serialize_packet(CmdType::INPUT_EVENT, kSessionId,
                            pack_mouse_input_event(type, x, y, 0));
}
std::vector<uint8_t> keyboard_event_packet(InputEventType type, uint16_t key) {
    return serialize_packet(CmdType::INPUT_EVENT, kSessionId,
                            pack_keyboard_input_event(type, key, 0));
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

// Receive packets from `sock`, with an explicit per-recv timeout.  Returns
// true with `out` populated on success; false on timeout / closed socket.
struct PacketRecv {
    MessageParser parser;
    int           sock = -1;

    bool recv_one(Packet& out, int timeout_secs) {
        timeval tv{timeout_secs, 0};
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        uint8_t buf[4096];
        while (true) {
            ParseResult r = parser.next_packet(out);
            if (r == ParseResult::OK) return true;
            ssize_t n = ::recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            parser.feed(buf, static_cast<size_t>(n));
        }
    }
};

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
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    return false;
}

// Match a single A/R/G/B byte sequence (wire order is [A,R,G,B]).
bool pixel_is(const uint8_t* p, uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return p[0] == a && p[1] == r && p[2] == g && p[3] == b;
}

}  // namespace

class M5InputTest : public ::testing::Test {
protected:
    pid_t      child_pid_    = 0;
    int        listen_fd_    = -1;
    int        apphost_sock_ = -1;
    PacketRecv recv_{};

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
            int status = 0;
            if (::waitpid(child_pid_, &status, WNOHANG) == 0) {
                ::kill(child_pid_, SIGKILL);
                ::waitpid(child_pid_, nullptr, 0);
            }
        }
    }

    // Drain APPHOST_READY + the two M4 startup PIXEL_DATA frames.  Stores the
    // most recent frameSeq so input-driven frames can be checked against it.
    void drain_startup(uint32_t* last_seq) {
        Packet pkt;
        ASSERT_TRUE(recv_.recv_one(pkt, /*timeout_secs=*/5));
        ASSERT_EQ(pkt.header.cmd_type, CmdType::APPHOST_READY);

        ASSERT_TRUE(recv_.recv_one(pkt, /*timeout_secs=*/5));
        ASSERT_EQ(pkt.header.cmd_type, CmdType::PIXEL_DATA);
        ASSERT_GE(pkt.body.size(), 24u);
        const uint32_t seq1 = read_be32(pkt.body.data() + 0);

        ASSERT_TRUE(recv_.recv_one(pkt, /*timeout_secs=*/5));
        ASSERT_EQ(pkt.header.cmd_type, CmdType::PIXEL_DATA);
        ASSERT_GE(pkt.body.size(), 24u);
        const uint32_t seq2 = read_be32(pkt.body.data() + 0);
        ASSERT_GT(seq2, seq1);
        if (last_seq != nullptr) *last_seq = seq2;
    }

    // Send CLOSE_SESSION and verify the AppHost child exits cleanly.
    void close_and_expect_clean_exit() {
        auto pkt = close_session_packet();
        ASSERT_TRUE(send_all(apphost_sock_, pkt.data(), pkt.size()));
        int exit_code = -1;
        EXPECT_TRUE(wait_child(child_pid_, /*timeout_ms=*/3000, &exit_code))
            << "AppHost did not exit within 3s after CLOSE_SESSION";
        child_pid_ = 0;
        EXPECT_EQ(exit_code, 0);
    }
};

TEST_F(M5InputTest, MouseClickChangesPixel) {
    uint32_t startup_seq = 0;
    ASSERT_NO_FATAL_FAILURE(drain_startup(&startup_seq));

    // Click at the centre of the small rect with LEFT_DOWN.  After the M4
    // startup the small rect is GREEN; the toggle moves it to YELLOW.
    auto click = mouse_event_packet(InputEventType::MOUSE_LEFT_DOWN,
                                    /*x=*/12, /*y=*/8);
    ASSERT_TRUE(send_all(apphost_sock_, click.data(), click.size()));

    Packet pkt;
    ASSERT_TRUE(recv_.recv_one(pkt, /*timeout_secs=*/5));
    ASSERT_EQ(pkt.header.cmd_type, CmdType::PIXEL_DATA);
    ASSERT_GE(pkt.body.size(), 24u);

    EXPECT_GT(read_be32(pkt.body.data() + 0), startup_seq);
    EXPECT_EQ(read_be32(pkt.body.data() +  4), static_cast<uint32_t>(kSmallLeft));
    EXPECT_EQ(read_be32(pkt.body.data() +  8), static_cast<uint32_t>(kSmallTop));
    EXPECT_EQ(read_be32(pkt.body.data() + 12), static_cast<uint32_t>(kSmallRight));
    EXPECT_EQ(read_be32(pkt.body.data() + 16), static_cast<uint32_t>(kSmallBottom));

    const int sw = kSmallRight - kSmallLeft;
    const int sh = kSmallBottom - kSmallTop;
    const uint32_t dlen = read_be32(pkt.body.data() + 20);
    ASSERT_EQ(dlen, static_cast<uint32_t>(sw * sh * 4));
    ASSERT_EQ(pkt.body.size(), 24u + dlen);

    // Every pixel of the dirty rect must be YELLOW (R=255, G=255, B=0, A=255).
    const uint8_t* px = pkt.body.data() + 24;
    int yellow = 0;
    for (int i = 0; i < sw * sh; ++i) {
        if (pixel_is(px + i * 4, 0xFF, 0xFF, 0xFF, 0x00)) ++yellow;
    }
    EXPECT_EQ(yellow, sw * sh);

    close_and_expect_clean_exit();
}

TEST_F(M5InputTest, KeyboardEventRouted) {
    uint32_t startup_seq = 0;
    ASSERT_NO_FATAL_FAILURE(drain_startup(&startup_seq));

    auto key = keyboard_event_packet(InputEventType::KEY_DOWN, kKeyF1);
    ASSERT_TRUE(send_all(apphost_sock_, key.data(), key.size()));

    Packet pkt;
    ASSERT_TRUE(recv_.recv_one(pkt, /*timeout_secs=*/5));
    ASSERT_EQ(pkt.header.cmd_type, CmdType::PIXEL_DATA);
    ASSERT_GE(pkt.body.size(), 24u);

    // KB_F1 toggles bg -> CYAN and damages the full window.
    EXPECT_GT(read_be32(pkt.body.data() + 0), startup_seq);
    EXPECT_EQ(read_be32(pkt.body.data() +  4), 0u);
    EXPECT_EQ(read_be32(pkt.body.data() +  8), 0u);
    EXPECT_EQ(read_be32(pkt.body.data() + 12), static_cast<uint32_t>(kWindowWidth));
    EXPECT_EQ(read_be32(pkt.body.data() + 16), static_cast<uint32_t>(kWindowHeight));

    const uint32_t dlen = read_be32(pkt.body.data() + 20);
    ASSERT_EQ(dlen, static_cast<uint32_t>(kWindowWidth * kWindowHeight * 4));
    ASSERT_EQ(pkt.body.size(), 24u + dlen);

    // Background should be CYAN (R=0, G=255, B=255, A=255).  Small rect is the
    // post-startup latched GREEN (R=0, G=255, B=0).  Both colours have R=0 and
    // G=255, so we just spot-check a corner pixel for CYAN and the small-rect
    // centre for GREEN.
    const uint8_t* px       = pkt.body.data() + 24;
    const uint8_t* corner   = px + (0 * kWindowWidth + 0) * 4;
    const uint8_t* sm_center = px + (8 * kWindowWidth + 12) * 4;
    EXPECT_TRUE(pixel_is(corner,    0xFF, 0x00, 0xFF, 0xFF))
        << "expected CYAN at (0,0)";
    EXPECT_TRUE(pixel_is(sm_center, 0xFF, 0x00, 0xFF, 0x00))
        << "expected GREEN at small-rect centre";

    close_and_expect_clean_exit();
}

TEST_F(M5InputTest, NoInputNoPixelData) {
    uint32_t startup_seq = 0;
    ASSERT_NO_FATAL_FAILURE(drain_startup(&startup_seq));

    // No INPUT_EVENT sent.  We expect recv to time out (return false) within
    // a short window — demo_app must not produce frames on a timer.
    Packet pkt;
    EXPECT_FALSE(recv_.recv_one(pkt, /*timeout_secs=*/1))
        << "AppHost emitted an unexpected PIXEL_DATA without input; got cmd "
        << static_cast<int>(pkt.header.cmd_type);

    close_and_expect_clean_exit();
}

TEST_F(M5InputTest, InvalidInputIgnored) {
    uint32_t startup_seq = 0;
    ASSERT_NO_FATAL_FAILURE(drain_startup(&startup_seq));

    // 1) Empty body.
    auto empty = serialize_packet(CmdType::INPUT_EVENT, kSessionId, {});
    ASSERT_TRUE(send_all(apphost_sock_, empty.data(), empty.size()));

    // 2) Unknown eventType.
    std::vector<uint8_t> bad_type(INPUT_EVENT_MOUSE_BODY_SIZE, 0u);
    bad_type[0] = 0x7Fu;
    auto bad_type_pkt = serialize_packet(CmdType::INPUT_EVENT, kSessionId, bad_type);
    ASSERT_TRUE(send_all(apphost_sock_, bad_type_pkt.data(), bad_type_pkt.size()));

    // 3) Mouse eventType but wrong length (too short).
    std::vector<uint8_t> short_mouse(5, 0u);
    short_mouse[0] = static_cast<uint8_t>(InputEventType::MOUSE_MOVE);
    auto short_mouse_pkt = serialize_packet(CmdType::INPUT_EVENT, kSessionId, short_mouse);
    ASSERT_TRUE(send_all(apphost_sock_, short_mouse_pkt.data(), short_mouse_pkt.size()));

    // 4) Keyboard eventType with keyCode > 0xFF.
    auto big_key = keyboard_event_packet(InputEventType::KEY_DOWN, 0x0100);
    ASSERT_TRUE(send_all(apphost_sock_, big_key.data(), big_key.size()));

    // None of the above should produce a PIXEL_DATA.  Then send a real F1
    // KEY_DOWN to confirm the AppHost is still alive and routing input.
    Packet pkt;
    EXPECT_FALSE(recv_.recv_one(pkt, /*timeout_secs=*/1))
        << "bad INPUT_EVENT produced an unexpected packet (cmd "
        << static_cast<int>(pkt.header.cmd_type) << ")";

    auto good = keyboard_event_packet(InputEventType::KEY_DOWN, kKeyF1);
    ASSERT_TRUE(send_all(apphost_sock_, good.data(), good.size()));
    ASSERT_TRUE(recv_.recv_one(pkt, /*timeout_secs=*/5));
    EXPECT_EQ(pkt.header.cmd_type, CmdType::PIXEL_DATA);

    close_and_expect_clean_exit();
}

TEST_F(M5InputTest, PostWorkThreadSafety) {
    uint32_t startup_seq = 0;
    ASSERT_NO_FATAL_FAILURE(drain_startup(&startup_seq));

    // Burst of LEFT_DOWN events alternating in/out of the small rect.  The
    // input_work_pending coalescing means the AppHost does NOT need to
    // produce 100 PIXEL_DATA frames; we only assert it doesn't crash or
    // deadlock and still services CLOSE_SESSION cleanly.  We also drain any
    // PIXEL_DATA the AppHost emits while the burst is in flight, so its
    // socket send buffer never fills up enough to back-pressure us.
    constexpr int kBurst = 100;
    for (int i = 0; i < kBurst; ++i) {
        const int x = (i % 2 == 0) ? 12 : 0;
        const int y = (i % 2 == 0) ?  8 : 0;
        auto p = mouse_event_packet(InputEventType::MOUSE_LEFT_DOWN, x, y);
        ASSERT_TRUE(send_all(apphost_sock_, p.data(), p.size()));
    }

    // Drain whatever frames arrive within a short window — we don't care
    // about the count, only that the AppHost stays responsive.
    while (true) {
        Packet pkt;
        if (!recv_.recv_one(pkt, /*timeout_secs=*/1)) break;
        if (pkt.header.cmd_type != CmdType::PIXEL_DATA) {
            FAIL() << "unexpected packet type during burst: "
                   << static_cast<int>(pkt.header.cmd_type);
        }
    }

    close_and_expect_clean_exit();
}
