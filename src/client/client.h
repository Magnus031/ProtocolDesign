#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "src/client/pixel_renderer.h"
#ifdef ERROR
#undef ERROR
#endif
#include "src/protocol/message_parser.h"

// ClientRuntime is the M6 client-side protocol runtime.  M6a can run without a
// Gateway, but the runtime already knows how to spawn an app, receive
// SESSION_ACK/PIXEL_DATA, update PixelRenderer, and send INPUT_EVENT packets.
class ClientRuntime {
public:
    struct Options {
        std::string gateway_host = "127.0.0.1";
        uint16_t gateway_port = 19000;
        std::string app_name = "demo_app";
        bool connect_gateway = true;
    };

    ClientRuntime();
    ~ClientRuntime();

    ClientRuntime(const ClientRuntime&) = delete;
    ClientRuntime& operator=(const ClientRuntime&) = delete;

    // Starts the background network thread.  Returns true if a thread was
    // started.  Connection failures are reported through last_error().
    bool start(const Options& options, PixelRenderer* renderer,
               std::function<void()> repaint_callback);

    // Stops the network thread and closes the socket.
    void stop();

    // Sends one already-packed INPUT_EVENT body to the Gateway.
    bool send_input_event(const std::vector<uint8_t>& body);

    uint32_t session_id() const;
    bool connected() const;
    const std::string& last_error() const;

private:
    void run();
    bool open_socket();
    bool send_spawn_app();
    bool send_packet(CmdType cmd, uint32_t session_id,
                     const std::vector<uint8_t>& body);
    void handle_packet(const Packet& packet);
    void close_socket();
    void set_error(const std::string& error);

    Options options_;
    PixelRenderer* renderer_ = nullptr;
    std::function<void()> repaint_callback_;

    std::thread thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint32_t> session_id_{0};

    mutable std::mutex mutex_;
    std::string last_error_;

#if defined(_WIN32)
    uintptr_t socket_ = static_cast<uintptr_t>(~0ull);
#else
    int socket_ = -1;
#endif
    std::mutex send_mutex_;
    MessageParser parser_;
};
