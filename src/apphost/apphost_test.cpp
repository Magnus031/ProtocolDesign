#include <gtest/gtest.h>
#include "src/apphost/apphost.h"
#include "src/protocol/message_parser.h"
#include "src/protocol/protocol.h"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <chrono>
#include <unistd.h>

static constexpr uint32_t TEST_SESSION = 42;

// ── Port helpers ─────────────────────────────────────────────────────────────

// Ask the OS for a free ephemeral port to avoid collisions in CI.
static uint16_t pick_free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    ::close(fd);
    return ntohs(addr.sin_port);
}

// ── TestClient helpers ───────────────────────────────────────────────────────

static int connect_to(uint16_t port) {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct timeval tv{5, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    ::inet_aton("127.0.0.1", &addr.sin_addr);

    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(sock);
        return -1;
    }
    return sock;
}

static bool send_all(int sock, const uint8_t* buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = ::send(sock, buf + sent, n - sent, 0);
        if (r <= 0) return false;
        sent += static_cast<size_t>(r);
    }
    return true;
}

class BlockingTestClient {
public:
    explicit BlockingTestClient(uint16_t port) : sock_(connect_to(port)) {}
    ~BlockingTestClient() {
        if (sock_ >= 0)
            ::close(sock_);
    }

    int sock() const { return sock_; }

    bool send_packet(const std::vector<uint8_t>& packet) {
        return send_all(sock_, packet.data(), packet.size());
    }

    bool send_bytes(const uint8_t* data, size_t len) {
        return send_all(sock_, data, len);
    }

    // Reads until the persistent parser emits one complete packet.
    // Returns ERROR_RESP on recv error/timeout.
    Packet recv_one_packet() {
        uint8_t buf[4096];
        Packet  pkt;
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
    int           sock_;
    MessageParser parser_;
};

static std::vector<uint8_t> make_spawn_app(uint32_t session_id,
                                            const std::string& app_name) {
    uint32_t name_len = static_cast<uint32_t>(app_name.size());
    std::vector<uint8_t> body(4 + name_len);
    body[0] = static_cast<uint8_t>((name_len >> 24) & 0xFF);
    body[1] = static_cast<uint8_t>((name_len >> 16) & 0xFF);
    body[2] = static_cast<uint8_t>((name_len >>  8) & 0xFF);
    body[3] = static_cast<uint8_t>( name_len        & 0xFF);
    std::memcpy(body.data() + 4, app_name.data(), name_len);

    Header hdr{};
    hdr.magic       = PROTOCOL_MAGIC;
    hdr.session_id  = session_id;
    hdr.cmd_type    = CmdType::SPAWN_APP;
    hdr.body_length = static_cast<uint32_t>(body.size());

    std::vector<uint8_t> pkt(HEADER_SIZE + body.size());
    serialize_header(hdr, pkt.data());
    std::memcpy(pkt.data() + HEADER_SIZE, body.data(), body.size());
    return pkt;
}

static std::vector<uint8_t> make_heartbeat(uint32_t session_id) {
    Header hdr{};
    hdr.magic       = PROTOCOL_MAGIC;
    hdr.session_id  = session_id;
    hdr.cmd_type    = CmdType::HEARTBEAT;
    hdr.body_length = 0;

    std::vector<uint8_t> pkt(HEADER_SIZE);
    serialize_header(hdr, pkt.data());
    return pkt;
}

static uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) <<  8) |
            static_cast<uint32_t>(p[3]);
}

// ── Test ─────────────────────────────────────────────────────────────────────

// The IoPool singleton cannot be re-initialised in the same process, so all
// AppHost scenarios are exercised in a single test function.
TEST(AppHostTest, M2Pipeline) {
    const uint16_t TEST_PORT = pick_free_port();
    AppHost server;
    ASSERT_TRUE(server.start(TEST_PORT)) << "AppHost failed to bind port " << TEST_PORT;

    // ── Scenario 1: SPAWN_APP → PIXEL_DATA ───────────────────────────────

    {
        BlockingTestClient client(TEST_PORT);
        ASSERT_GE(client.sock(), 0) << "Could not connect to AppHost";

        auto spawn = make_spawn_app(TEST_SESSION, "demo");
        ASSERT_TRUE(client.send_packet(spawn));

        Packet pkt = client.recv_one_packet();
        EXPECT_EQ(pkt.header.cmd_type,    CmdType::PIXEL_DATA);
        EXPECT_EQ(pkt.header.session_id,  TEST_SESSION);

        // PIXEL_DATA body layout:
        //   frameSeq(4) rectLeft(4) rectTop(4) rectRight(4) rectBottom(4)
        //   dataLen(4)  pixelData(variable)
        ASSERT_GE(pkt.body.size(), 24u) << "PIXEL_DATA body too short";

        const uint8_t* b = pkt.body.data();
        uint32_t rect_right  = read_be32(b + 12);
        uint32_t rect_bottom = read_be32(b + 16);
        uint32_t data_len    = read_be32(b + 20);

        EXPECT_EQ(read_be32(b +  4), 0u);   // rectLeft
        EXPECT_EQ(read_be32(b +  8), 0u);   // rectTop
        EXPECT_EQ(rect_right,       16u);
        EXPECT_EQ(rect_bottom,      16u);
        EXPECT_EQ(data_len,         16u * 16u * 4u);
        ASSERT_EQ(pkt.body.size(),  24u + data_len);

        // First pixel: solid red in ARGB wire order [A, R, G, B] = 0xFFFF0000.
        EXPECT_EQ(b[24], 0xFF);  // A
        EXPECT_EQ(b[25], 0xFF);  // R
        EXPECT_EQ(b[26], 0x00);  // G
        EXPECT_EQ(b[27], 0x00);  // B
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // ── Scenario 2: HEARTBEAT echo ────────────────────────────────────────

    {
        BlockingTestClient client(TEST_PORT);
        ASSERT_GE(client.sock(), 0) << "Could not connect for heartbeat test";

        auto hb = make_heartbeat(7);
        ASSERT_TRUE(client.send_packet(hb));

        Packet pkt = client.recv_one_packet();
        EXPECT_EQ(pkt.header.cmd_type,    CmdType::HEARTBEAT);
        EXPECT_EQ(pkt.header.session_id,  7u);
        EXPECT_EQ(pkt.header.body_length, 0u);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // ── Scenario 3: CLOSE_SESSION → server closes the connection ─────────

    {
        BlockingTestClient client(TEST_PORT);
        ASSERT_GE(client.sock(), 0) << "Could not connect for CleanShutdown test";

        Header hdr{};
        hdr.magic       = PROTOCOL_MAGIC;
        hdr.session_id  = TEST_SESSION;
        hdr.cmd_type    = CmdType::CLOSE_SESSION;
        hdr.body_length = 0;
        uint8_t buf[HEADER_SIZE];
        serialize_header(hdr, buf);
        ASSERT_TRUE(client.send_bytes(buf, HEADER_SIZE));

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        uint8_t tmp[64];
        ssize_t n = ::recv(client.sock(), tmp, sizeof(tmp), 0);
        EXPECT_LE(n, 0) << "Expected connection close after CLOSE_SESSION";
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // ── Scenario 4: Two packets in one TCP write → both responses arrive ──
    // Exercises the send queue: SPAWN_APP triggers PIXEL_DATA, then HEARTBEAT
    // triggers another send while the first may still be in flight.
    //
    {
        BlockingTestClient client(TEST_PORT);
        ASSERT_GE(client.sock(), 0) << "Could not connect for two-packet test";

        auto spawn = make_spawn_app(TEST_SESSION, "demo");
        auto hb    = make_heartbeat(TEST_SESSION);

        std::vector<uint8_t> combined(spawn);
        combined.insert(combined.end(), hb.begin(), hb.end());
        ASSERT_TRUE(client.send_packet(combined));

        Packet p1 = client.recv_one_packet();
        EXPECT_EQ(p1.header.cmd_type, CmdType::PIXEL_DATA)
            << "First response should be PIXEL_DATA";

        Packet p2 = client.recv_one_packet();
        EXPECT_EQ(p2.header.cmd_type, CmdType::HEARTBEAT)
            << "Second response should be HEARTBEAT";
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    server.stop();
}
