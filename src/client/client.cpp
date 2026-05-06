#include "src/client/client.h"

#include <array>
#include <chrono>
#include <cstring>

#include "src/protocol/protocol.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

void write_be32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
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

    if (!options_.connect_gateway) return false;

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
    if (!open_socket()) return;
    connected_.store(true);

    if (!send_spawn_app()) {
        close_socket();
        connected_.store(false);
        return;
    }

    std::array<uint8_t, 4096> buffer{};
    while (!stop_requested_.load()) {
#if defined(_WIN32)
        const int n = recv(static_cast<SOCKET>(socket_),
                           reinterpret_cast<char*>(buffer.data()),
                           static_cast<int>(buffer.size()), 0);
#else
        const int n = static_cast<int>(
            recv(socket_, buffer.data(), buffer.size(), 0));
#endif
        if (n <= 0) break;

        parser_.feed(buffer.data(), static_cast<size_t>(n));
        Packet pkt;
        while (parser_.next_packet(pkt) == ParseResult::OK) {
            handle_packet(pkt);
        }
    }

    close_socket();
    connected_.store(false);
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
    return true;
}

void ClientRuntime::handle_packet(const Packet& packet) {
    switch (packet.header.cmd_type) {
        case CmdType::SESSION_ACK:
            session_id_.store(packet.header.session_id);
            break;
        case CmdType::PIXEL_DATA:
            if (renderer_ != nullptr &&
                renderer_->apply_pixel_data_body(packet.body.data(),
                                                 packet.body.size()) ==
                    PixelRenderer::ApplyResult::OK &&
                repaint_callback_) {
                repaint_callback_();
            }
            break;
        case CmdType::ERROR_RESP:
            set_error("Gateway returned ERROR_RESP");
            break;
        default:
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
