// Standard headers before GKC headers (GKC #pragma pack hygiene).
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "src/apphost/apphost.h"
#include "src/apphost/plugin_loader.h"
#include "src/apphost/ui_host_impl.h"
#include "src/common/input_event.h"
#include "src/common/message_handler.h"
#include "src/protocol/protocol.h"

namespace {

constexpr int kConnectAttempts   = 5;
constexpr int kConnectDelayMs    = 200;
constexpr size_t kRecvChunkBytes = 4096;

int connect_to_gateway(const std::string& host, uint16_t port) {
    for (int attempt = 0; attempt < kConnectAttempts; ++attempt) {
        int sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return -1;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        ::inet_aton(host.c_str(), &addr.sin_addr);
        if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
            return sock;
        ::close(sock);
        std::this_thread::sleep_for(std::chrono::milliseconds(kConnectDelayMs));
    }
    return -1;
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

}  // namespace

// ── AppHost::Impl ──────────────────────────────────────────────────────────

struct AppHost::Impl {
    int                          sock_      = -1;
    std::mutex                   send_mtx_;          // serializes writes to sock_
    std::atomic<bool>            shutting_down_{false};

    // MessageHandler internally owns a MessageParser; feed() parses and
    // dispatches to registered CmdType handlers in one call.
    MessageHandler               handler_;
    UiHostImpl                   ui_host_;
    PluginLoader                 plugin_;
    uint32_t                     session_id_ = 0;

    std::thread                  reader_;

    // ── Wire helpers ──────────────────────────────────────────────────────

    bool send_packet(CmdType cmd, const std::vector<uint8_t>& body) {
        Header hdr{};
        hdr.magic       = PROTOCOL_MAGIC;
        hdr.session_id  = session_id_;
        hdr.cmd_type    = cmd;
        hdr.body_length = static_cast<uint32_t>(body.size());

        std::vector<uint8_t> wire(HEADER_SIZE + body.size());
        serialize_header(hdr, wire.data());
        if (!body.empty())
            std::memcpy(wire.data() + HEADER_SIZE, body.data(), body.size());

        std::lock_guard<std::mutex> lk(send_mtx_);
        if (sock_ < 0) return false;
        return send_all(sock_, wire.data(), wire.size());
    }

    bool send_apphost_ready() {
        return send_packet(CmdType::APPHOST_READY, {});
    }

    bool send_heartbeat() {
        return send_packet(CmdType::HEARTBEAT, {});
    }

    bool send_pixel_data(std::vector<uint8_t> body) {
        return send_packet(CmdType::PIXEL_DATA, body);
    }

    // ── Reader thread ─────────────────────────────────────────────────────

    void reader_loop() {
        std::vector<uint8_t> buf(kRecvChunkBytes);
        while (!shutting_down_.load(std::memory_order_acquire)) {
            ssize_t n = ::recv(sock_, buf.data(), buf.size(), 0);
            if (n <= 0) {
                // Connection closed or error: signal plugin to exit Loop.
                ui_host_.request_quit();
                return;
            }
            handler_.feed(buf.data(), static_cast<size_t>(n));
        }
    }

    // ── Run ───────────────────────────────────────────────────────────────

    int run(const AppHost::Config& cfg) {
        session_id_ = cfg.session_id;
        // try to reversely connect to gateway 
        sock_ = connect_to_gateway(cfg.gateway_host, cfg.gateway_port);
        if (sock_ < 0) return -1;
        if (!send_apphost_ready()) {
            ::close(sock_); sock_ = -1;
            return -2;
        }

        // Plugin handlers.  The reader thread feeds the parser and dispatches
        // these synchronously on the reader thread. HEARTBEAT is a quick echo,
        // CLOSE_SESSION only asks the UI loop to quit, and INPUT_EVENT is
        // parsed here but posted to UiHostImpl for main-thread delivery.
        handler_.register_handler(CmdType::HEARTBEAT,
            [this](const Packet&) { send_heartbeat(); });
        handler_.register_handler(CmdType::CLOSE_SESSION,
            [this](const Packet&) { ui_host_.request_quit(); });
        // INPUT_EVENT (M5): the reader thread is NOT allowed to call into the
        // plugin handler directly — plugin DoMouse/DoKeyboard must run on the
        // main thread that owns the GKC backing store and synchronously emits
        // PIXEL_DATA via Damage().  We parse the Body here and hand the event
        // to UiHostImpl, which copies it into an owns-data queue and uses
        // PostWork to ship it to the main thread.
        //
        // Invalid Body shapes (empty / unknown eventType / wrong length /
        // keyCode > 255) are silently dropped per §5.1.1 — input is data
        // plane, a malformed packet must not kill the session.
        handler_.register_handler(CmdType::INPUT_EVENT,
            [this](const Packet& pkt) {
                ParsedInputEvent ev;
                if (!parse_input_event_body(pkt.body.data(), pkt.body.size(), ev))
                    return;
                if (ev.kind == ParsedInputEvent::Kind::Mouse)
                    ui_host_.post_mouse_input(ev.mouse);
                else
                    ui_host_.post_keyboard_input(ev.keyboard);
            });

        // Pixel sink: wire up the PIXEL_DATA output channel before the plugin
        // starts rendering.  The chain inside UiHostImpl is:
        //
        //   1. Plugin calls Show(true) or Damage(rect)
        //      -> on_window_show / on_window_damage
        //   2. dispatch_draw_and_send builds UiMessageDraw, passes the GKC
        //      color_quad buffer pointer to the plugin's DoDraw (Process)
        //   3. Plugin renders into pBuffer inside DoDraw
        //   4. pack_pixel_data_rect converts GKC little-endian BGRA into
        //      wire-format big-endian [A, R, G, B]
        //   5. s->sink(std::move(body)) invokes this lambda
        //      -> send_pixel_data -> TCP to Gateway
        //
        // Must be set before plugin_.run() so the first Show(true) frame
        // has a valid sink; otherwise dispatch_draw_and_send drops the data.
        ui_host_.set_pixel_data_sink([this](std::vector<uint8_t> body) {
            (void)send_pixel_data(std::move(body));
        });

        if (!plugin_.open(cfg.plugin_path)) {
            // Plugin load failure: tear down connection.
            ::close(sock_); sock_ = -1;
            return -3;
        }

        reader_ = std::thread([this] { reader_loop(); });

        // Start executing plugin code.  make_interface() packages AppHost's
        // fake UiHostImpl as {context=this UiHostImpl, funcs=&kHostTable};
        // _SA_UIMain stores that pair in the plugin-local GKC::g_ui_host
        // before calling the plugin's ProgramEntryPoint::GuiMain().
        const int rc = plugin_.run(ui_host_.make_interface());

        // Plugin returned: shut everything down.
        shutting_down_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(send_mtx_);
            if (sock_ >= 0) {
                ::shutdown(sock_, SHUT_RDWR);
                ::close(sock_);
                sock_ = -1;
            }
        }
        if (reader_.joinable()) reader_.join();
        return rc;
    }
};

// ── AppHost public API ────────────────────────────────────────────────────

AppHost::AppHost() : impl_(std::make_unique<Impl>()) {}
AppHost::~AppHost() = default;

int AppHost::run(const Config& cfg) {
    return impl_->run(cfg);
}
