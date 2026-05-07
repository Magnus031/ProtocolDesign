#include "src/client/client.h"

#include <array>
#include <chrono>
#include <cstring>

#include "src/client/client_log.h"
#include "src/protocol/protocol.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

constexpr auto kHeartbeatInterval = std::chrono::seconds(1);

void write_be32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}

const char* cmd_name(CmdType cmd) noexcept {
    switch (cmd) {
        case CmdType::HEARTBEAT: return "HEARTBEAT";
        case CmdType::SPAWN_APP: return "SPAWN_APP";
        case CmdType::SESSION_ACK: return "SESSION_ACK";
        case CmdType::APPHOST_READY: return "APPHOST_READY";
        case CmdType::PIXEL_DATA: return "PIXEL_DATA";
        case CmdType::INPUT_EVENT: return "INPUT_EVENT";
        case CmdType::CLOSE_SESSION: return "CLOSE_SESSION";
        case CmdType::ERROR_RESP: return "ERROR_RESP";
        default: return "UNKNOWN";
    }
}

std::vector<uint8_t> pack_spawn_app_body(const std::string& app_name) {
    std::vector<uint8_t> body(4 + app_name.size());
    write_be32(body.data(), static_cast<uint32_t>(app_name.size()));
    std::memcpy(body.data() + 4, app_name.data(), app_name.size());
    return body;
}

#if defined(_WIN32)
bool ensure_winsock(std::string& error) {
    static bool initialized = false;
    if (initialized) return true;
    WSADATA data{};
    const int rc = WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
        error = "WSAStartup failed";
        return false;
    }
    initialized = true;
    return true;
}
#endif

void set_recv_timeout(uintptr_t socket_value) {
#if defined(_WIN32)
    const DWORD timeout_ms = 250;
    setsockopt(static_cast<SOCKET>(socket_value), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 250000;
    setsockopt(static_cast<int>(socket_value), SOL_SOCKET, SO_RCVTIMEO, &tv,
               sizeof(tv));
#endif
}

bool is_recv_timeout() noexcept {
#if defined(_WIN32)
    return WSAGetLastError() == WSAETIMEDOUT;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

}  // namespace

ClientRuntime::ClientRuntime() = default;

ClientRuntime::~ClientRuntime() {
    stop();
}

bool ClientRuntime::start(const Options& options, PixelRenderer* renderer,
                          std::function<void()> repaint_callback) {
    stop();
    options_ = options;
    renderer_ = renderer;
    repaint_callback_ = std::move(repaint_callback);
    stop_requested_.store(false);
    session_id_.store(0);
    connected_.store(false);
    set_error(std::string());

    client_log_line("runtime: start host=" + options_.gateway_host +
                    " port=" + std::to_string(options_.gateway_port) +
                    " app=" + options_.app_name);

    if (!options_.connect_gateway) {
        client_log_line("runtime: connect_gateway=false");
        return false;
    }

    thread_ = std::thread(&ClientRuntime::run, this);
    return true;
}

void ClientRuntime::stop() {
    stop_requested_.store(true);
    close_socket();
    if (thread_.joinable()) thread_.join();
    connected_.store(false);
}

bool ClientRuntime::send_input_event(const std::vector<uint8_t>& body) {
    const uint32_t sid = session_id_.load();
    if (sid == 0 || !connected_.load()) return false;
    return send_packet(CmdType::INPUT_EVENT, sid, body);
}

uint32_t ClientRuntime::session_id() const {
    return session_id_.load();
}

bool ClientRuntime::connected() const {
    return connected_.load();
}

const std::string& ClientRuntime::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

void ClientRuntime::run() {
    if (!open_socket()) {
        client_log_line("runtime: open_socket failed: " + last_error());
        return;
    }
    connected_.store(true);
    set_recv_timeout(socket_);
    client_log_line("runtime: connected");

    if (!send_spawn_app()) {
        client_log_line("runtime: send SPAWN_APP failed: " + last_error());
        close_socket();
        connected_.store(false);
        return;
    }
    client_log_line("runtime: sent SPAWN_APP");

    std::array<uint8_t, 4096> buffer{};
    auto next_heartbeat = std::chrono::steady_clock::now() + kHeartbeatInterval;
    while (!stop_requested_.load()) {
        const uint32_t sid = session_id_.load();
        const auto now = std::chrono::steady_clock::now();
        if (sid != 0 && now >= next_heartbeat) {
            if (send_packet(CmdType::HEARTBEAT, sid, {})) {
                client_log_line("runtime: sent HEARTBEAT session=" +
                                std::to_string(sid));
            }
            next_heartbeat = now + kHeartbeatInterval;
        }

#if defined(_WIN32)
        const int n = recv(static_cast<SOCKET>(socket_),
                           reinterpret_cast<char*>(buffer.data()),
                           static_cast<int>(buffer.size()), 0);
#else
        const int n = static_cast<int>(
            recv(socket_, buffer.data(), buffer.size(), 0));
#endif
        if (n <= 0) {
            if (is_recv_timeout()) continue;
            break;
        }
        client_log_line("runtime: recv bytes=" + std::to_string(n));

        parser_.feed(buffer.data(), static_cast<size_t>(n));
        Packet pkt;
        while (parser_.next_packet(pkt) == ParseResult::OK) {
            handle_packet(pkt);
        }
    }

#if defined(_WIN32)
    if (!stop_requested_.load()) {
        client_log_line("runtime: recv ended n<=0 wsa_error=" +
                        std::to_string(WSAGetLastError()));
    }
#else
    if (!stop_requested_.load()) client_log_line("runtime: recv ended n<=0");
#endif

    close_socket();
    connected_.store(false);
    client_log_line("runtime: stopped");
}

bool ClientRuntime::open_socket() {
#if defined(_WIN32)
    std::string error;
    if (!ensure_winsock(error)) {
        set_error(error);
        return false;
    }
#endif

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const std::string port = std::to_string(options_.gateway_port);
    const int rc =
        getaddrinfo(options_.gateway_host.c_str(), port.c_str(), &hints, &result);
    if (rc != 0 || result == nullptr) {
        set_error("getaddrinfo failed for " + options_.gateway_host + ":" + port);
        return false;
    }

    bool ok = false;
    for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
#if defined(_WIN32)
        SOCKET s = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) {
            socket_ = static_cast<uintptr_t>(s);
            ok = true;
            break;
        }
        closesocket(s);
#else
        int s = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (s < 0) continue;
        if (connect(s, it->ai_addr, it->ai_addrlen) == 0) {
            socket_ = s;
            ok = true;
            break;
        }
        close(s);
#endif
    }
    freeaddrinfo(result);

    if (!ok) {
        set_error("connect failed for " + options_.gateway_host + ":" + port);
        client_log_line("runtime: connect failed for " + options_.gateway_host +
                        ":" + port);
    }
    return ok;
}

bool ClientRuntime::send_spawn_app() {
    return send_packet(CmdType::SPAWN_APP, 0,
                       pack_spawn_app_body(options_.app_name));
}

bool ClientRuntime::send_packet(CmdType cmd, uint32_t session_id,
                                const std::vector<uint8_t>& body) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (!connected_.load()) return false;

    Header header{PROTOCOL_MAGIC, session_id, cmd,
                  static_cast<uint32_t>(body.size()), 0};
    std::vector<uint8_t> packet(HEADER_SIZE + body.size());
    serialize_header(header, packet.data());
    if (!body.empty()) {
        std::memcpy(packet.data() + HEADER_SIZE, body.data(), body.size());
    }

    size_t sent = 0;
    while (sent < packet.size()) {
#if defined(_WIN32)
        const int n = send(static_cast<SOCKET>(socket_),
                           reinterpret_cast<const char*>(packet.data() + sent),
                           static_cast<int>(packet.size() - sent), 0);
#else
        const int n = static_cast<int>(
            send(socket_, packet.data() + sent, packet.size() - sent, 0));
#endif
        if (n <= 0) {
            set_error("send failed");
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    client_log_line("runtime: sent packet cmd=" + std::string(cmd_name(cmd)) +
                    " session=" + std::to_string(session_id) +
                    " bytes=" + std::to_string(packet.size()));
    return true;
}

void ClientRuntime::handle_packet(const Packet& packet) {
    client_log_line("runtime: packet cmd=" +
                    std::string(cmd_name(packet.header.cmd_type)) +
                    " session=" + std::to_string(packet.header.session_id) +
                    " body=" + std::to_string(packet.body.size()));
    switch (packet.header.cmd_type) {
        case CmdType::SESSION_ACK:
            session_id_.store(packet.header.session_id);
            client_log_line("runtime: SESSION_ACK session_id=" +
                            std::to_string(packet.header.session_id));
            break;
        case CmdType::PIXEL_DATA:
            if (renderer_ != nullptr) {
                const auto result = renderer_->apply_pixel_data_body(
                    packet.body.data(), packet.body.size());
                client_log_line("runtime: PIXEL_DATA len=" +
                                std::to_string(packet.body.size()) +
                                " result=" +
                                std::to_string(static_cast<int>(result)));
                if (result == PixelRenderer::ApplyResult::OK &&
                    repaint_callback_) {
                    client_log_line("runtime: invoking repaint_callback");
                    repaint_callback_();
                }
            }
            break;
        case CmdType::ERROR_RESP:
            set_error("Gateway returned ERROR_RESP");
            client_log_line("runtime: ERROR_RESP");
            break;
        case CmdType::CLOSE_SESSION:
            client_log_line("runtime: CLOSE_SESSION");
            break;
        default:
            client_log_line("runtime: ignored packet cmd_value=" +
                            std::to_string(static_cast<int>(
                                packet.header.cmd_type)));
            break;
    }
}

void ClientRuntime::close_socket() {
#if defined(_WIN32)
    const uintptr_t invalid = static_cast<uintptr_t>(~0ull);
    if (socket_ != invalid) {
        closesocket(static_cast<SOCKET>(socket_));
        socket_ = invalid;
    }
#else
    if (socket_ >= 0) {
        close(socket_);
        socket_ = -1;
    }
#endif
}

void ClientRuntime::set_error(const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = error;
}
