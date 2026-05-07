// app_2048 input integration test.
//
// Spawns the real apphost_bin with libapp_2048.so as its plugin, accepts
// the inbound connection on a fake-Gateway listener, drains APPHOST_READY
// + the initial Show(true) PIXEL_DATA, then sends an INPUT_EVENT and
// asserts the resulting plugin behaviour.
//
// Specifically asserted:
//
//   * MOUSE_LEFT_DOWN at the RESET button's centre triggers a new full-
//     window PIXEL_DATA frame (the reset path matches F2, see
//     plugins/app_2048/app_2048.cpp App2048Window::do_reset).  This is the
//     "AppHost/plugin-level or integration test that sends MOUSE_LEFT_DOWN
//     at the reset button coordinates and observes a reset-induced
//     PIXEL_DATA" required by docs/design/2048.md.
//
//   * MOUSE_LEFT_DOWN outside the button rect produces no frame — guards
//     against the plugin damaging the window for arbitrary mouse events,
//     which would inflate bandwidth and break the "tile clicks are out of
//     scope for MVP" rule.
//
//   * KEY_DOWN(KB_F2) triggers the same reset path, proving F2 and the
//     button share a code path at the application boundary.
//
// What this deliberately does NOT exercise:
//   * Real Gateway routing (covered by gateway_m5_input_test.cpp).
//   * Pure board logic (covered by app_2048_logic_test.cpp).
//   * The reset-button hit-test (covered by app_2048_view_test.cpp).

#include <gtest/gtest.h>

#include "plugins/app_2048/app_2048_view.h"
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

// KB_F2 from third_party/GKC/public/include/base/system/ui_types.h.
constexpr uint16_t kKeyF2 = 0x71;

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
        // Pass the canvas size to apphost_bin even though the plugin's
        // Create(true, 160, 200) is the actual source of truth — keeps the
        // command line consistent with the manual demo invocation in
        // docs/design/2048.md.
        std::string a_host    = "--gateway-host=127.0.0.1";
        std::string a_port    = "--gateway-port=" +
                                std::to_string(static_cast<unsigned>(port));
        std::string a_session = "--session-id=" +
                                std::to_string(static_cast<unsigned long long>(session_id));
        std::string a_plugin  = "--plugin=" + plugin_path;
        std::string a_width   = "--width="  + std::to_string(app_2048::kCanvasW);
        std::string a_height  = "--height=" + std::to_string(app_2048::kCanvasH);

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

}  // namespace

class App2048InputTest : public ::testing::Test {
protected:
    pid_t      child_pid_    = 0;
    int        listen_fd_    = -1;
    int        apphost_sock_ = -1;
    PacketRecv recv_{};

    std::string apphost_bin_path_;
    std::string plugin_so_path_;

    void SetUp() override {
        apphost_bin_path_ = runfile_path("src/apphost/apphost_bin");
        plugin_so_path_   = runfile_path("plugins/app_2048/libapp_2048.so");

        const uint16_t port = pick_free_port();
        listen_fd_ = start_listener(port);
        ASSERT_GE(listen_fd_, 0) << "fake Gateway listen() failed";

        child_pid_ = spawn_apphost(apphost_bin_path_, plugin_so_path_,
                                   port, kSessionId);
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

    // Drain APPHOST_READY + the initial Show(true) PIXEL_DATA.  Unlike
    // demo_app, app_2048 only emits one startup frame (no follow-up Damage
    // for a small rect), so we expect exactly one PIXEL_DATA here.
    void drain_startup(uint32_t* last_seq) {
        Packet pkt;
        ASSERT_TRUE(recv_.recv_one(pkt, /*timeout_secs=*/5))
            << "did not receive APPHOST_READY";
        ASSERT_EQ(pkt.header.cmd_type, CmdType::APPHOST_READY);

        ASSERT_TRUE(recv_.recv_one(pkt, /*timeout_secs=*/5))
            << "did not receive initial PIXEL_DATA";
        ASSERT_EQ(pkt.header.cmd_type, CmdType::PIXEL_DATA);
        ASSERT_GE(pkt.body.size(), 24u);
        // Initial Show(true) paints the full canvas.
        EXPECT_EQ(read_be32(pkt.body.data() +  4), 0u);
        EXPECT_EQ(read_be32(pkt.body.data() +  8), 0u);
        EXPECT_EQ(read_be32(pkt.body.data() + 12),
                  static_cast<uint32_t>(app_2048::kCanvasW));
        EXPECT_EQ(read_be32(pkt.body.data() + 16),
                  static_cast<uint32_t>(app_2048::kCanvasH));
        if (last_seq != nullptr)
            *last_seq = read_be32(pkt.body.data() + 0);
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

    // Common assertion: a full-canvas PIXEL_DATA frame with frame_seq strictly
    // greater than `prev_seq`, indicating the plugin damaged the window.
    void expect_full_canvas_frame_after(uint32_t prev_seq, uint32_t* new_seq) {
        Packet pkt;
        ASSERT_TRUE(recv_.recv_one(pkt, /*timeout_secs=*/5))
            << "did not receive expected PIXEL_DATA";
        ASSERT_EQ(pkt.header.cmd_type, CmdType::PIXEL_DATA);
        ASSERT_GE(pkt.body.size(), 24u);
        const uint32_t seq = read_be32(pkt.body.data() + 0);
        EXPECT_GT(seq, prev_seq);
        EXPECT_EQ(read_be32(pkt.body.data() +  4), 0u);
        EXPECT_EQ(read_be32(pkt.body.data() +  8), 0u);
        EXPECT_EQ(read_be32(pkt.body.data() + 12),
                  static_cast<uint32_t>(app_2048::kCanvasW));
        EXPECT_EQ(read_be32(pkt.body.data() + 16),
                  static_cast<uint32_t>(app_2048::kCanvasH));
        const uint32_t dlen = read_be32(pkt.body.data() + 20);
        EXPECT_EQ(dlen, static_cast<uint32_t>(
                            app_2048::kCanvasW * app_2048::kCanvasH * 4));
        if (new_seq != nullptr) *new_seq = seq;
    }
};

// MOUSE_LEFT_DOWN inside the RESET button rect must trigger a reset-induced
// full-canvas PIXEL_DATA frame.  Coordinates come from app_2048_view.h so
// they cannot drift from what the plugin actually draws.
TEST_F(App2048InputTest, ResetButtonClickProducesFullFrame) {
    uint32_t startup_seq = 0;
    ASSERT_NO_FATAL_FAILURE(drain_startup(&startup_seq));

    auto click = mouse_event_packet(InputEventType::MOUSE_LEFT_DOWN,
                                    app_2048::kResetButtonCenterX,
                                    app_2048::kResetButtonCenterY);
    ASSERT_TRUE(send_all(apphost_sock_, click.data(), click.size()));

    uint32_t after_seq = 0;
    ASSERT_NO_FATAL_FAILURE(expect_full_canvas_frame_after(startup_seq, &after_seq));

    close_and_expect_clean_exit();
}

// MOUSE_LEFT_DOWN outside the button rect must not produce any PIXEL_DATA.
// We pick a coordinate well inside the board area; the plugin explicitly
// drops mouse events that don't hit the reset button.  A regression here
// (e.g. unconditional Damage on every click) would inflate bandwidth and
// re-introduce tile-click semantics that MVP rules out.
TEST_F(App2048InputTest, ClickOutsideButtonProducesNoFrame) {
    uint32_t startup_seq = 0;
    ASSERT_NO_FATAL_FAILURE(drain_startup(&startup_seq));

    // Centre of the board area — far from the reset rect at (16,176)-(72,194).
    auto click = mouse_event_packet(InputEventType::MOUSE_LEFT_DOWN,
                                    app_2048::kCanvasW / 2,
                                    app_2048::kCanvasH / 2);
    ASSERT_TRUE(send_all(apphost_sock_, click.data(), click.size()));

    // recv_one with a 1-second timeout must time out (no frame arrives).
    Packet pkt;
    EXPECT_FALSE(recv_.recv_one(pkt, /*timeout_secs=*/1))
        << "unexpected packet after non-button click: cmd="
        << static_cast<int>(pkt.header.cmd_type);

    close_and_expect_clean_exit();
}

// KB_F2 must drive the same reset path as the mouse button, producing a
// full-canvas PIXEL_DATA.  Demonstrates that the keyboard and mouse reset
// triggers share a code path at the application boundary, as required by
// docs/design/2048.md "Step 1 — Pure Game Logic" test list.
TEST_F(App2048InputTest, F2KeyDownProducesFullFrame) {
    uint32_t startup_seq = 0;
    ASSERT_NO_FATAL_FAILURE(drain_startup(&startup_seq));

    auto key = keyboard_event_packet(InputEventType::KEY_DOWN, kKeyF2);
    ASSERT_TRUE(send_all(apphost_sock_, key.data(), key.size()));

    uint32_t after_seq = 0;
    ASSERT_NO_FATAL_FAILURE(expect_full_canvas_frame_after(startup_seq, &after_seq));

    close_and_expect_clean_exit();
}
