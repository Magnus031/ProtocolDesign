#include "src/apphost/apphost.h"
#include "src/protocol/protocol.h"
#include "src/protocol/message_parser.h"
#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

static AppHost* g_server = nullptr;

static void on_signal(int) {
    if (g_server) g_server->stop();
}

// Parse --port=<n> or --port <n> from argv.  Returns 0 on parse error.
static uint16_t parse_port(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        // --port=19001
        if (std::strncmp(arg, "--port=", 7) == 0) {
            long v = std::strtol(arg + 7, nullptr, 10);
            return (v > 0 && v < 65536) ? static_cast<uint16_t>(v) : 0;
        }
        // --port 19001
        if (std::strcmp(arg, "--port") == 0 && i + 1 < argc) {
            long v = std::strtol(argv[++i], nullptr, 10);
            return (v > 0 && v < 65536) ? static_cast<uint16_t>(v) : 0;
        }
    }
    return 19001;  // default
}

static const char* find_arg_value(int argc, char* argv[], const char* name) {
    const size_t n = std::strlen(name);
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], name, n) == 0 && argv[i][n] == '=')
            return argv[i] + n + 1;
        if (std::strcmp(argv[i], name) == 0 && i + 1 < argc)
            return argv[i + 1];
    }
    return nullptr;
}

static bool parse_u32_arg(const char* s, uint32_t* out) {
    if (!s) return false;
    char* end = nullptr;
    unsigned long v = std::strtoul(s, &end, 0);
    if (end == s || *end != '\0')
        return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

static int connect_to_gateway(const char* host, uint16_t port) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        int sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
            return -1;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_aton(host, &addr.sin_addr);
        if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
            return sock;
        ::close(sock);
        usleep(200 * 1000);
    }
    return -1;
}

static bool send_packet(int sock, CmdType cmd, uint32_t session_id) {
    Header hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.session_id = session_id;
    hdr.cmd_type = cmd;
    hdr.body_length = 0;
    uint8_t buf[HEADER_SIZE];
    serialize_header(hdr, buf);
    size_t sent = 0;
    while (sent < HEADER_SIZE) {
        ssize_t n = ::send(sock, buf + sent, HEADER_SIZE - sent, 0);
        if (n <= 0)
            return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

static int run_gateway_client_mode(int argc, char* argv[]) {
    const char* host = find_arg_value(argc, argv, "--gateway-host");
    const char* port_s = find_arg_value(argc, argv, "--gateway-port");
    const char* session_s = find_arg_value(argc, argv, "--session-id");
    uint32_t session_id = 0;
    if (!host || !port_s || !parse_u32_arg(session_s, &session_id))
        return 1;
    const uint16_t port = static_cast<uint16_t>(std::strtoul(port_s, nullptr, 10));

    int sock = connect_to_gateway(host, port);
    if (sock < 0)
        return 2;
    if (!send_packet(sock, CmdType::APPHOST_READY, session_id)) {
        ::close(sock);
        return 3;
    }

    MessageParser parser;
    uint8_t buf[1024];
    while (true) {
        ssize_t n = ::recv(sock, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        parser.feed(buf, static_cast<size_t>(n));
        Packet pkt;
        while (parser.next_packet(pkt) == ParseResult::OK) {
            if (pkt.header.cmd_type == CmdType::HEARTBEAT)
                send_packet(sock, CmdType::HEARTBEAT, pkt.header.session_id);
            if (pkt.header.cmd_type == CmdType::CLOSE_SESSION) {
                ::close(sock);
                return 0;
            }
        }
    }
    ::close(sock);
    return 0;
}

int main(int argc, char* argv[]) {
    if (find_arg_value(argc, argv, "--gateway-host") != nullptr)
        return run_gateway_client_mode(argc, argv);

    uint16_t port = parse_port(argc, argv);
    if (port == 0) {
        std::fprintf(stderr, "Usage: apphost [--port <1-65535>]\n");
        return 1;
    }

    AppHost server;
    g_server = &server;

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    if (!server.start(port)) {
        std::fprintf(stderr, "AppHost: failed to start on port %u\n", port);
        return 1;
    }

    std::fprintf(stdout, "AppHost: listening on port %u\n", port);

    // Block until stop() is called from a signal handler.
    server.wait();

    return 0;
}
