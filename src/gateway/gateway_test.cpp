#include <gtest/gtest.h>

#include "src/gateway/gateway.h"
#include "src/protocol/message_parser.h"
#include "src/protocol/protocol.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// Ask the OS for an unused loopback TCP port. Tests use dynamic ports so they
// do not collide with a developer's manually running Gateway/AppHost.
uint16_t pick_free_port() {
    // create a TCP socket, and return a fd
    // AF_INET -> Ipv4
    // SOCK_STREAM -> TCP
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    // IPV4 object struct
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    // OS will offers addr a new port
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    ::close(fd);
    // just to explore a free port, after finding just close it.
    return ntohs(addr.sin_port);
}

// Open a blocking POSIX TCP connection to the Gateway listener on 127.0.0.1.
// This deliberately does not use IoPool: the test peers are simple stand-ins
// for real Client/AppHost processes, while Gateway is the component under test.
int connect_to(uint16_t port) {
    // set up for the real connection
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

// send() may write only part of the buffer. Keep calling it until the complete
// serialized protocol packet has been handed to the kernel.
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

// Build one complete wire-format packet:
//   Header(13 bytes, big-endian) + optional Body.
// Gateway should forward most packets exactly in this serialized form.
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

// SPAWN_APP is the only packet in M3a sent before a session exists, so its
// header session_id is 0. Gateway allocates the real session_id and returns it
// in SESSION_ACK.
std::vector<uint8_t> make_spawn_app(const std::string& name) {
    std::vector<uint8_t> body(4 + name.size());
    const uint32_t len = static_cast<uint32_t>(name.size());
    body[0] = static_cast<uint8_t>((len >> 24) & 0xFF);
    body[1] = static_cast<uint8_t>((len >> 16) & 0xFF);
    body[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
    body[3] = static_cast<uint8_t>(len & 0xFF);
    std::memcpy(body.data() + 4, name.data(), name.size());
    return make_packet(CmdType::SPAWN_APP, 0, body);
}

// A minimal blocking socket peer used as either:
//   - TestClient, connected to Gateway's public_port
//   - TestAppHost, connected to Gateway's apphost_internal_port
//
// It owns one persistent MessageParser so recv_one_packet() does not lose a
// second packet if a single recv() call returns multiple protocol frames.
class TestPeer {
public:
    explicit TestPeer(uint16_t port) : sock_(connect_to(port)) {}
    ~TestPeer() {
        if (sock_ >= 0)
            ::close(sock_);
    }

    int sock() const { return sock_; }

    bool send_packet(const std::vector<uint8_t>& packet) {
        return send_all(sock_, packet.data(), packet.size());
    }

    // Block until one complete protocol packet is parsed from the socket.
    // On EOF/error/timeout, return an ERROR_RESP sentinel so assertions fail
    // with a protocol-level value instead of hanging forever.
    Packet recv_one_packet() {
        uint8_t buf[4096];
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

// Client-side session creation handshake:
//   TestClient -> Gateway: SPAWN_APP(session_id=0)
//   Gateway    -> TestClient: SESSION_ACK(header.session_id=<allocated id>)
//
// M3a does not fork a real AppHost yet; it only allocates a session and proves
// Gateway can route once a TestAppHost later binds to the same id.
uint32_t spawn_session(TestPeer& client, const std::string& app_name) {
    EXPECT_TRUE(client.send_packet(make_spawn_app(app_name)));
    Packet ack = client.recv_one_packet();
    EXPECT_EQ(ack.header.cmd_type, CmdType::SESSION_ACK);
    EXPECT_NE(ack.header.session_id, 0u);
    return ack.header.session_id;
}

// AppHost-side binding handshake:
//   TestAppHost -> Gateway: APPHOST_READY(header.session_id=<allocated id>)
//
// This tells Gateway that the current AppHost TCP connection belongs to the
// session created by SPAWN_APP. Without this explicit packet, Gateway could not
// safely match AppHost connections to Client sessions under concurrency.
void bind_apphost(TestPeer& apphost, uint32_t session_id) {
    ASSERT_TRUE(apphost.send_packet(
        make_packet(CmdType::APPHOST_READY, session_id)));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

} // namespace

// IoPool is a process-global singleton, so all Gateway scenarios run in one
// test process with one Gateway instance.
TEST(GatewayTest, M3aRouting) {
    // public_port is the externally visible Client entry point. The internal
    // port is the single shared listener used by all AppHost/TestAppHost
    // connections. One listener port can accept many independent TCP sessions.
    const uint16_t public_port = pick_free_port();
    const uint16_t apphost_internal_port = pick_free_port();

    Gateway gateway;
    ASSERT_TRUE(gateway.start(public_port, apphost_internal_port));

    // Scenario 1: single session routes Client INPUT_EVENT to AppHost and
    // AppHost PIXEL_DATA back to Client.
    {
        // These two peers represent the final architecture:
        //   Client   -> Gateway public port
        //   AppHost  -> Gateway internal AppHost port
        TestPeer client(public_port);
        TestPeer apphost(apphost_internal_port);
        ASSERT_GE(client.sock(), 0);
        ASSERT_GE(apphost.sock(), 0);

        const uint32_t session_id = spawn_session(client, "demo");
        bind_apphost(apphost, session_id);

        // Client-to-AppHost direction. Gateway should not inspect the Body;
        // it should route by header.session_id and preserve the payload bytes.
        std::vector<uint8_t> input_body{0x01, 0x02, 0x03, 0x04};
        ASSERT_TRUE(client.send_packet(
            make_packet(CmdType::INPUT_EVENT, session_id, input_body)));

        Packet to_apphost = apphost.recv_one_packet();
        EXPECT_EQ(to_apphost.header.cmd_type, CmdType::INPUT_EVENT);
        EXPECT_EQ(to_apphost.header.session_id, session_id);
        EXPECT_EQ(to_apphost.body, input_body);

        // AppHost-to-Client direction. This is the same route in reverse,
        // using PIXEL_DATA as the representative AppHost-produced packet.
        std::vector<uint8_t> pixel_body{0xAA, 0xBB, 0xCC};
        ASSERT_TRUE(apphost.send_packet(
            make_packet(CmdType::PIXEL_DATA, session_id, pixel_body)));

        Packet to_client = client.recv_one_packet();
        EXPECT_EQ(to_client.header.cmd_type, CmdType::PIXEL_DATA);
        EXPECT_EQ(to_client.header.session_id, session_id);
        EXPECT_EQ(to_client.body, pixel_body);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Scenario 2: two sessions stay isolated and do not cross-route payloads.
    {
        // All peers share the same two Gateway listener ports, but each accepted
        // connection gets its own IoPool handle and ConnContext inside Gateway.
        TestPeer client_a(public_port);
        TestPeer client_b(public_port);
        TestPeer apphost_a(apphost_internal_port);
        TestPeer apphost_b(apphost_internal_port);
        ASSERT_GE(client_a.sock(), 0);
        ASSERT_GE(client_b.sock(), 0);
        ASSERT_GE(apphost_a.sock(), 0);
        ASSERT_GE(apphost_b.sock(), 0);

        const uint32_t session_a = spawn_session(client_a, "app-a");
        const uint32_t session_b = spawn_session(client_b, "app-b");
        ASSERT_NE(session_a, session_b);
        bind_apphost(apphost_a, session_a);
        bind_apphost(apphost_b, session_b);

        // Send distinct payloads on each session. If SessionTable routing is
        // wrong, apphost_a/apphost_b will observe swapped or duplicated bodies.
        std::vector<uint8_t> body_a{0xA1};
        std::vector<uint8_t> body_b{0xB1, 0xB2};
        ASSERT_TRUE(client_a.send_packet(
            make_packet(CmdType::INPUT_EVENT, session_a, body_a)));
        ASSERT_TRUE(client_b.send_packet(
            make_packet(CmdType::INPUT_EVENT, session_b, body_b)));

        Packet app_a_pkt = apphost_a.recv_one_packet();
        Packet app_b_pkt = apphost_b.recv_one_packet();
        EXPECT_EQ(app_a_pkt.header.session_id, session_a);
        EXPECT_EQ(app_a_pkt.body, body_a);
        EXPECT_EQ(app_b_pkt.header.session_id, session_b);
        EXPECT_EQ(app_b_pkt.body, body_b);

        // Verify the reverse direction is isolated too: AppHost A's pixels
        // must return only to Client A, and AppHost B's only to Client B.
        std::vector<uint8_t> pixels_a{0x10, 0x11};
        std::vector<uint8_t> pixels_b{0x20, 0x21, 0x22};
        ASSERT_TRUE(apphost_a.send_packet(
            make_packet(CmdType::PIXEL_DATA, session_a, pixels_a)));
        ASSERT_TRUE(apphost_b.send_packet(
            make_packet(CmdType::PIXEL_DATA, session_b, pixels_b)));

        Packet client_a_pkt = client_a.recv_one_packet();
        Packet client_b_pkt = client_b.recv_one_packet();
        EXPECT_EQ(client_a_pkt.header.session_id, session_a);
        EXPECT_EQ(client_a_pkt.body, pixels_a);
        EXPECT_EQ(client_b_pkt.header.session_id, session_b);
        EXPECT_EQ(client_b_pkt.body, pixels_b);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Scenario 3: CLOSE_SESSION from Client is routed to the paired AppHost.
    {
        TestPeer client(public_port);
        TestPeer apphost(apphost_internal_port);
        ASSERT_GE(client.sock(), 0);
        ASSERT_GE(apphost.sock(), 0);

        const uint32_t session_id = spawn_session(client, "close-demo");
        bind_apphost(apphost, session_id);

        // M3a only verifies that the close control packet reaches the paired
        // AppHost. Full process cleanup and child reaping belong to M3b.
        ASSERT_TRUE(client.send_packet(
            make_packet(CmdType::CLOSE_SESSION, session_id)));
        Packet close = apphost.recv_one_packet();
        EXPECT_EQ(close.header.cmd_type, CmdType::CLOSE_SESSION);
        EXPECT_EQ(close.header.session_id, session_id);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    gateway.stop();
}
