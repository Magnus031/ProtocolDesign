#include "src/gateway/gateway.h"
#include "src/gateway/process_manager.h"
#include "src/gateway/session_manager.h"
#include "src/protocol/message_parser.h"
#include "src/protocol/protocol.h"

#include "base/GkcDef.h"
#include "sys/_GkcSys.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

// IoPool's send buffer is fixed at 4000 bytes (BUFFER_SIZE in
// third_party/GKC/RT/GkcSys/include/base/system/Linux/sys_io.h:24).  BeginInput
// blindly returns that buffer for any uLen the caller supplies — the bounds
// assert is compiled out in release builds — so passing a uLen > 4000 stomps
// the heap during the subsequent memcpy and crashes once the next allocation
// touches the corrupted region.  All packets larger than this must therefore
// be split into ≤ kIoPoolMaxChunk pieces before being handed to BeginInput.
// TCP delivers the wire bytes as a stream, so chunking is invisible to the
// peer's MessageParser.
constexpr size_t kIoPoolMaxChunk = 4000;

// ── TEMP PERF DIAGNOSTIC ────────────────────────────────────────────────────
// Inline timestamp logging used to attribute the ~40 ms unaccounted-for
// portion of end-to-end latency.  Activated only when the GATEWAY_PERF_TRACE
// environment variable is set.  Remove this block once the analysis is done.
static const bool g_perf_trace = std::getenv("GATEWAY_PERF_TRACE") != nullptr;
static const auto g_perf_t0    = std::chrono::steady_clock::now();
static long long perf_now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - g_perf_t0).count();
}
#define PERF_LOG(...) do { if (g_perf_trace) std::fprintf(stderr, __VA_ARGS__); } while (0)
// ────────────────────────────────────────────────────────────────────────────

void push_chunked(std::queue<std::vector<uint8_t>>& q,
                  std::vector<uint8_t> data) {
    if (data.empty()) return;
    if (data.size() <= kIoPoolMaxChunk) {
        q.push(std::move(data));
        return;
    }
    size_t off = 0;
    while (off < data.size()) {
        const size_t take = std::min(kIoPoolMaxChunk, data.size() - off);
        q.emplace(data.begin() + static_cast<std::ptrdiff_t>(off),
                  data.begin() + static_cast<std::ptrdiff_t>(off + take));
        off += take;
    }
}

std::vector<uint8_t> serialize_packet(const Packet& pkt) {
    std::vector<uint8_t> out(HEADER_SIZE + pkt.body.size());
    Header hdr = pkt.header;
    hdr.body_length = static_cast<uint32_t>(pkt.body.size());
    serialize_header(hdr, out.data());
    if (!pkt.body.empty())
        std::memcpy(out.data() + HEADER_SIZE, pkt.body.data(), pkt.body.size());
    return out;
}

std::vector<uint8_t> make_packet(CmdType cmd, uint32_t session_id) {
    Header hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.session_id = session_id;
    hdr.cmd_type = cmd;
    hdr.body_length = 0;

    std::vector<uint8_t> out(HEADER_SIZE);
    serialize_header(hdr, out.data());
    return out;
}

std::vector<uint8_t> make_error(uint32_t session_id) {
    return make_packet(CmdType::ERROR_RESP, session_id);
}

bool read_be32(const std::vector<uint8_t>& body, size_t offset, uint32_t* out) {
    if (offset + 4 > body.size())
        return false;
    *out = (static_cast<uint32_t>(body[offset]) << 24) |
           (static_cast<uint32_t>(body[offset + 1]) << 16) |
           (static_cast<uint32_t>(body[offset + 2]) << 8) |
            static_cast<uint32_t>(body[offset + 3]);
    return true;
}

bool is_valid_app_name(const std::string& name) {
    if (name.empty() || name.size() > 255)
        return false;
    if (name.find("..") != std::string::npos)
        return false;
    for (unsigned char ch : name) {
        if (ch == '/' || ch == '\\')
            return false;
        if (!(std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.'))
            return false;
    }
    return true;
}

bool parse_spawn_app_name(const Packet& pkt, std::string* out) {
    uint32_t len = 0;
    if (!read_be32(pkt.body, 0, &len))
        return false;
    if (len == 0 || len > 255 || pkt.body.size() != 4u + len)
        return false;
    std::string name(reinterpret_cast<const char*>(pkt.body.data() + 4), len);
    if (!is_valid_app_name(name))
        return false;
    *out = std::move(name);
    return true;
}

const char* cmd_type_name(CmdType cmd) {
    switch (cmd) {
    case CmdType::HEARTBEAT:      return "HEARTBEAT";
    case CmdType::SPAWN_APP:      return "SPAWN_APP";
    case CmdType::SESSION_ACK:    return "SESSION_ACK";
    case CmdType::APPHOST_READY:  return "APPHOST_READY";
    case CmdType::PIXEL_DATA:     return "PIXEL_DATA";
    case CmdType::INPUT_EVENT:    return "INPUT_EVENT";
    case CmdType::CLOSE_SESSION:  return "CLOSE_SESSION";
    case CmdType::ERROR_RESP:     return "ERROR_RESP";
    default:                      return "UNKNOWN";
    }
}

const char* key_name(uint8_t key) {
    switch (key) {
    case 0x08: return "BACKSPACE";
    case 0x09: return "TAB";
    case 0x0D: return "ENTER";
    case 0x10: return "SHIFT";
    case 0x11: return "CTRL";
    case 0x12: return "ALT";
    case 0x1B: return "ESC";
    case 0x20: return "SPACE";
    case 0x25: return "LEFT";
    case 0x26: return "UP";
    case 0x27: return "RIGHT";
    case 0x28: return "DOWN";
    case 0x70: return "F1";
    case 0x71: return "F2";
    case 0x72: return "F3";
    case 0x73: return "F4";
    case 0x74: return "F5";
    case 0x75: return "F6";
    case 0x76: return "F7";
    case 0x77: return "F8";
    case 0x78: return "F9";
    case 0x79: return "F10";
    case 0x7A: return "F11";
    case 0x7B: return "F12";
    default:   return nullptr;
    }
}

uint16_t read_input_be16(const uint8_t* p) noexcept {
    return (static_cast<uint16_t>(p[0]) << 8) |
           static_cast<uint16_t>(p[1]);
}

uint32_t read_input_be32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
            static_cast<uint32_t>(p[3]);
}

uint64_t read_input_be64(const uint8_t* p) noexcept {
    return (static_cast<uint64_t>(p[0]) << 56) |
           (static_cast<uint64_t>(p[1]) << 48) |
           (static_cast<uint64_t>(p[2]) << 40) |
           (static_cast<uint64_t>(p[3]) << 32) |
           (static_cast<uint64_t>(p[4]) << 24) |
           (static_cast<uint64_t>(p[5]) << 16) |
           (static_cast<uint64_t>(p[6]) << 8) |
            static_cast<uint64_t>(p[7]);
}

std::string describe_input_event(const Packet& pkt) {
    if (pkt.body.empty()) {
        return "input=INVALID";
    }

    const uint8_t raw = pkt.body[0];
    std::ostringstream os;
    if (raw >= 0x01u && raw <= 0x06u) {
        if (pkt.body.size() != 13u)
            return "input=INVALID_MOUSE";
        const char* action = "UNKNOWN";
        switch (raw) {
        case 0x01: action = "MOUSE_MOVE"; break;
        case 0x02: action = "MOUSE_LEFT_DOWN"; break;
        case 0x03: action = "MOUSE_LEFT_UP"; break;
        case 0x04: action = "MOUSE_RIGHT_DOWN"; break;
        case 0x05: action = "MOUSE_RIGHT_UP"; break;
        case 0x06: action = "MOUSE_SCROLL"; break;
        default: break;
        }
        const int32_t x = static_cast<int32_t>(read_input_be32(pkt.body.data() + 1));
        const int32_t y = static_cast<int32_t>(read_input_be32(pkt.body.data() + 5));
        const uint64_t ts = read_input_be64(pkt.body.data() + 9);
        os << "input=" << action << " x=" << x << " y=" << y << " ts=" << ts;
        return os.str();
    }

    if (raw == 0x10u || raw == 0x11u) {
        if (pkt.body.size() != 11u)
            return "input=INVALID_KEYBOARD";
        const uint16_t key_code = read_input_be16(pkt.body.data() + 1);
        const uint64_t ts = read_input_be64(pkt.body.data() + 3);
        os << "input=" << (raw == 0x10u ? "KEY_DOWN" : "KEY_UP");
        os << " key=";
        const uint8_t key = static_cast<uint8_t>(key_code & 0xFFu);
        const char* name = key_name(key);
        if (name != nullptr && key_code <= 0xFFu) {
            os << name;
        } else {
            os << "0x" << std::hex << static_cast<unsigned>(key_code) << std::dec;
        }
        os << " state=" << (raw == 0x10u ? "DOWN" : "UP") << " ts=" << ts;
        return os.str();
    }

    return "input=UNKNOWN";
}

} // namespace

struct Gateway::Impl {
    struct ClientSession;
    struct AppHostSession;

    struct ClientSession : public node_base {
        uintptr id_ = 0;
        uint64_t generation_ = 0;
        ClientSession* next_active_ = nullptr;
        _IoFunc io_func_;
        MessageParser parser_;
        uint32_t session_id_ = 0;
        uintptr apphost_conn_ = 0;
        std::queue<std::vector<uint8_t>> send_queue_;
        Impl* server_ = nullptr;

        ClientSession() noexcept { io_func_.Exec = on_client_event; }

        static int on_client_event(void* ctx, int type, uintptr param) noexcept {
            auto* conn = static_cast<ClientSession*>(ctx);
            switch (type) {
            case IO_TYPE_RECEIVED: {
                auto* info = reinterpret_cast<_IoRecvInfo*>(param);
                conn->server_->on_client_received(
                    conn, reinterpret_cast<const uint8_t*>(info->p),
                    static_cast<size_t>(info->len));
                break;
            }
            case IO_TYPE_SENT:
                conn->server_->on_client_sent(conn);
                break;
            case IO_TYPE_RECV_ERROR:
            case IO_TYPE_RECV_TIMEOUT:
            case IO_TYPE_BEFORE_CLOSE:
                conn->server_->release_client(conn);
                break;
            default:
                break;
            }
            return 0;
        }
    };

    struct AppHostSession : public node_base {
        uintptr id_ = 0;
        uint64_t generation_ = 0;
        AppHostSession* next_active_ = nullptr;
        _IoFunc io_func_;
        MessageParser parser_;
        uint32_t session_id_ = 0;
        uintptr client_conn_ = 0;
        std::queue<std::vector<uint8_t>> send_queue_;
        Impl* server_ = nullptr;

        AppHostSession() noexcept { io_func_.Exec = on_apphost_event; }

        static int on_apphost_event(void* ctx, int type, uintptr param) noexcept {
            auto* conn = static_cast<AppHostSession*>(ctx);
            switch (type) {
            case IO_TYPE_RECEIVED: {
                auto* info = reinterpret_cast<_IoRecvInfo*>(param);
                conn->server_->on_apphost_received(
                    conn, reinterpret_cast<const uint8_t*>(info->p),
                    static_cast<size_t>(info->len));
                break;
            }
            case IO_TYPE_SENT:
                conn->server_->on_apphost_sent(conn);
                break;
            case IO_TYPE_RECV_ERROR:
            case IO_TYPE_RECV_TIMEOUT:
            case IO_TYPE_BEFORE_CLOSE:
                conn->server_->release_apphost(conn);
                break;
            default:
                break;
            }
            return 0;
        }
    };

    ClientSession* alloc_client(uintptr id) noexcept {
        ClientSession* conn = nullptr;
        while (atomic_compare_exchange((int&)client_lock_, 0, 1)) {}
        call_result cr = client_pool_.FetchFreeNode(conn);
        if (cr.IsSucceeded()) {
            call_constructor(*conn);
            client_pool_.PickFreeNode();
            conn->id_ = id;
            conn->generation_ = ++next_client_generation_;
            conn->server_ = this;
            conn->next_active_ = client_head_;
            client_head_ = conn;
        }
        atomic_compare_exchange((int&)client_lock_, 1, 0);
        return cr.IsSucceeded() ? conn : nullptr;
    }

    AppHostSession* alloc_apphost(uintptr id) noexcept {
        AppHostSession* conn = nullptr;
        while (atomic_compare_exchange((int&)apphost_lock_, 0, 1)) {}
        call_result cr = apphost_pool_.FetchFreeNode(conn);
        if (cr.IsSucceeded()) {
            call_constructor(*conn);
            apphost_pool_.PickFreeNode();
            conn->id_ = id;
            conn->generation_ = ++next_apphost_generation_;
            conn->server_ = this;
            conn->next_active_ = apphost_head_;
            apphost_head_ = conn;
        }
        atomic_compare_exchange((int&)apphost_lock_, 1, 0);
        return cr.IsSucceeded() ? conn : nullptr;
    }

    void release_client(ClientSession* conn) noexcept {
        uint32_t session_id = 0;
        while (atomic_compare_exchange((int&)client_lock_, 0, 1)) {}
        ClientSession** pp = &client_head_;
        while (*pp && *pp != conn)
            pp = &(*pp)->next_active_;
        const bool was_active = (*pp == conn);
        if (was_active) {
            *pp = conn->next_active_;
            session_id = conn->session_id_;
        }
        atomic_compare_exchange((int&)client_lock_, 1, 0);
        if (!was_active)
            return;

        std::fprintf(stderr, "[gateway] client disconnected  id=%lu session=%u\n",
                     static_cast<unsigned long>(conn->id_),
                     static_cast<unsigned>(session_id));

        if (session_id != 0) {
            const uintptr peer = sessions_.find_apphost(session_id);
            sessions_.remove_session(session_id);
            if (peer != 0)
                send_to_apphost(peer, make_packet(CmdType::CLOSE_SESSION, session_id));
        } else {
            sessions_.remove_by_client(conn->id_);
        }

        wait_for_drain_workers();
        call_destructor(*conn);
        while (atomic_compare_exchange((int&)client_lock_, 0, 1)) {}
        client_pool_.PutFreeNode(conn);
        atomic_compare_exchange((int&)client_lock_, 1, 0);
    }

    void release_apphost(AppHostSession* conn) noexcept {
        uint32_t session_id = 0;
        while (atomic_compare_exchange((int&)apphost_lock_, 0, 1)) {}
        AppHostSession** pp = &apphost_head_;
        while (*pp && *pp != conn)
            pp = &(*pp)->next_active_;
        const bool was_active = (*pp == conn);
        if (was_active) {
            *pp = conn->next_active_;
            session_id = conn->session_id_;
        }
        atomic_compare_exchange((int&)apphost_lock_, 1, 0);
        if (!was_active)
            return;

        std::fprintf(stderr, "[gateway] apphost disconnected  id=%lu session=%u\n",
                     static_cast<unsigned long>(conn->id_),
                     static_cast<unsigned>(session_id));

        if (session_id != 0) {
            const uintptr peer = sessions_.find_client(session_id);
            sessions_.remove_session(session_id);
            if (peer != 0)
                send_to_client(peer, make_packet(CmdType::CLOSE_SESSION, session_id));
        } else {
            sessions_.remove_by_apphost(conn->id_);
        }

        wait_for_drain_workers();
        call_destructor(*conn);
        while (atomic_compare_exchange((int&)apphost_lock_, 0, 1)) {}
        apphost_pool_.PutFreeNode(conn);
        atomic_compare_exchange((int&)apphost_lock_, 1, 0);
    }

    static int on_client_listen_event(void* ctx, int type, uintptr param) noexcept {
        // only deal-with the IO_TYPE_ACCEPT_INIT 
        if (type != IO_TYPE_ACCEPT_INIT)
            return 0;
        auto* self = static_cast<Impl*>(ctx);
        ClientSession* conn = self->alloc_client(param);
        if (conn == nullptr)
            return 0;

        std::fprintf(stderr, "[gateway] client connected  id=%lu\n",
                     static_cast<unsigned long>(param));

        bool cancelled = false;
        self->pool_.GetFunc()->SetHandleFunc(
            self->pool_.GetContext(), param, conn->io_func_, conn, cancelled);
        if (cancelled) {
            self->release_client(conn);
            return 0;
        }
        return 1;
    }

    static int on_apphost_listen_event(void* ctx, int type, uintptr param) noexcept {
        if (type != IO_TYPE_ACCEPT_INIT)
            return 0;
        auto* self = static_cast<Impl*>(ctx);
        AppHostSession* conn = self->alloc_apphost(param);
        if (conn == nullptr)
            return 0;

        std::fprintf(stderr, "[gateway] apphost connected  id=%lu\n",
                     static_cast<unsigned long>(param));

        bool cancelled = false;
        self->pool_.GetFunc()->SetHandleFunc(
            self->pool_.GetContext(), param, conn->io_func_, conn, cancelled);
        if (cancelled) {
            self->release_apphost(conn);
            return 0;
        }
        return 1;
    }

    ClientSession* find_client_locked(uintptr id) const noexcept {
        for (ClientSession* p = client_head_; p != nullptr; p = p->next_active_) {
            if (p->id_ == id)
                return p;
        }
        return nullptr;
    }

    AppHostSession* find_apphost_locked(uintptr id) const noexcept {
        for (AppHostSession* p = apphost_head_; p != nullptr; p = p->next_active_) {
            if (p->id_ == id)
                return p;
        }
        return nullptr;
    }

    uint64_t client_generation(ClientSession* conn) noexcept {
        while (atomic_compare_exchange((int&)client_lock_, 0, 1)) {}
        ClientSession* active = find_client_locked(conn->id_);
        const uint64_t generation = (active == conn) ? conn->generation_ : 0;
        atomic_compare_exchange((int&)client_lock_, 1, 0);
        return generation;
    }

    uint64_t apphost_generation(AppHostSession* conn) noexcept {
        while (atomic_compare_exchange((int&)apphost_lock_, 0, 1)) {}
        AppHostSession* active = find_apphost_locked(conn->id_);
        const uint64_t generation = (active == conn) ? conn->generation_ : 0;
        atomic_compare_exchange((int&)apphost_lock_, 1, 0);
        return generation;
    }

    void enqueue_client(ClientSession* conn, std::vector<uint8_t> data) {
        while (atomic_compare_exchange((int&)client_lock_, 0, 1)) {}
        if (find_client_locked(conn->id_) != conn) {
            atomic_compare_exchange((int&)client_lock_, 1, 0);
            return;
        }

        // Split the packet into IoPool-sized chunks before queueing.  A
        // single PIXEL_DATA frame for a 160x200 canvas is ~128 KB; without
        // chunking, BeginInput corrupts the 4 KB send buffer and crashes.
        const bool was_idle = conn->send_queue_.empty();
        push_chunked(conn->send_queue_, std::move(data));
        if (!was_idle || conn->send_queue_.empty()) {
            // Earlier chunks are still in flight; the existing drain path
            // (IO_TYPE_SENT → schedule_client_drain) will pick up the new
            // tail when iSend clears.
            atomic_compare_exchange((int&)client_lock_, 1, 0);
            return;
        }

        // Eager fast path: queue was empty, so iSend is also clear.  Hand
        // the head chunk to BeginInput; if BeginInput refuses (busy or pool
        // cancelled) we leave the chunk in the queue and rely on drain.
        const auto& chunk = conn->send_queue_.front();
        bool cancelled = false;
        byte* raw = pool_.GetFunc()->BeginInput(
            pool_.GetContext(), conn->id_,
            static_cast<uint>(chunk.size()), cancelled);
        if (raw && !cancelled) {
            std::memcpy(raw, chunk.data(), chunk.size());
            conn->send_queue_.pop();
            pool_.GetFunc()->EndInput(pool_.GetContext(), conn->id_);
            PERF_LOG("[perf] +%lld us  enqueue_client EAGER chunk size=%zu queue_left=%zu\n",
                     perf_now_us(), chunk.size(), conn->send_queue_.size());
        }
        atomic_compare_exchange((int&)client_lock_, 1, 0);
    }

    void enqueue_apphost(AppHostSession* conn, std::vector<uint8_t> data) {
        while (atomic_compare_exchange((int&)apphost_lock_, 0, 1)) {}
        if (find_apphost_locked(conn->id_) != conn) {
            atomic_compare_exchange((int&)apphost_lock_, 1, 0);
            return;
        }

        const bool was_idle = conn->send_queue_.empty();
        push_chunked(conn->send_queue_, std::move(data));
        if (!was_idle || conn->send_queue_.empty()) {
            atomic_compare_exchange((int&)apphost_lock_, 1, 0);
            return;
        }

        const auto& chunk = conn->send_queue_.front();
        bool cancelled = false;
        byte* raw = pool_.GetFunc()->BeginInput(
            pool_.GetContext(), conn->id_,
            static_cast<uint>(chunk.size()), cancelled);
        if (raw && !cancelled) {
            std::memcpy(raw, chunk.data(), chunk.size());
            conn->send_queue_.pop();
            pool_.GetFunc()->EndInput(pool_.GetContext(), conn->id_);
        }
        atomic_compare_exchange((int&)apphost_lock_, 1, 0);
    }

    void send_to_client(uintptr id, std::vector<uint8_t> data) {
        ClientSession* target = nullptr;
        while (atomic_compare_exchange((int&)client_lock_, 0, 1)) {}
        target = find_client_locked(id);
        atomic_compare_exchange((int&)client_lock_, 1, 0);
        if (target != nullptr)
            enqueue_client(target, std::move(data));
    }

    void send_to_apphost(uintptr id, std::vector<uint8_t> data) {
        AppHostSession* target = nullptr;
        while (atomic_compare_exchange((int&)apphost_lock_, 0, 1)) {}
        target = find_apphost_locked(id);
        atomic_compare_exchange((int&)apphost_lock_, 1, 0);
        if (target != nullptr)
            enqueue_apphost(target, std::move(data));
    }

    void schedule_client_drain(ClientSession* conn, uint64_t generation) {
        PERF_LOG("[perf] +%lld us  schedule_client_drain CALLED (queue spawning thread)\n",
                 perf_now_us());
        drain_workers_.fetch_add(1, std::memory_order_acq_rel);
        std::thread([this, conn, generation] {
            const long long t_thread_start = perf_now_us();
            PERF_LOG("[perf] +%lld us  client drain thread STARTED (about to sleep_for 1ms)\n",
                     t_thread_start);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const long long t_sleep_end = perf_now_us();
            PERF_LOG("[perf] +%lld us  client drain thread WAKE (slept actual=%lld us)\n",
                     t_sleep_end, t_sleep_end - t_thread_start);
            drain_client_queue(conn, generation);
            if (drain_workers_.fetch_sub(1, std::memory_order_acq_rel) == 1)
                cv_.notify_all();
        }).detach();
    }

    void schedule_apphost_drain(AppHostSession* conn, uint64_t generation) {
        drain_workers_.fetch_add(1, std::memory_order_acq_rel);
        std::thread([this, conn, generation] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            drain_apphost_queue(conn, generation);
            if (drain_workers_.fetch_sub(1, std::memory_order_acq_rel) == 1)
                cv_.notify_all();
        }).detach();
    }

    void drain_client_queue(ClientSession* conn, uint64_t generation) {
        while (atomic_compare_exchange((int&)client_lock_, 0, 1)) {}
        if (find_client_locked(conn->id_) != conn || conn->generation_ != generation ||
            conn->send_queue_.empty()) {
            atomic_compare_exchange((int&)client_lock_, 1, 0);
            return;
        }

        const auto& data = conn->send_queue_.front();
        bool cancelled = false;
        // alloc a data.size()-length part
        byte* raw = pool_.GetFunc()->BeginInput(
            pool_.GetContext(), conn->id_, static_cast<uint>(data.size()), cancelled);
        if (raw && !cancelled) {
            std::memcpy(raw, data.data(), data.size());
            conn->send_queue_.pop();
            // notify socket sends to the client
            pool_.GetFunc()->EndInput(pool_.GetContext(), conn->id_);
            PERF_LOG("[perf] +%lld us  drain_client_queue SENT chunk size=%zu queue_left=%zu\n",
                     perf_now_us(), data.size(), conn->send_queue_.size());
            atomic_compare_exchange((int&)client_lock_, 1, 0);
            return;
        }
        const bool retry = !cancelled;
        atomic_compare_exchange((int&)client_lock_, 1, 0);
        if (retry)
            schedule_client_drain(conn, generation);
    }

    void drain_apphost_queue(AppHostSession* conn, uint64_t generation) {
        while (atomic_compare_exchange((int&)apphost_lock_, 0, 1)) {}
        if (find_apphost_locked(conn->id_) != conn || conn->generation_ != generation ||
            conn->send_queue_.empty()) {
            atomic_compare_exchange((int&)apphost_lock_, 1, 0);
            return;
        }

        const auto& data = conn->send_queue_.front();
        bool cancelled = false;
        byte* raw = pool_.GetFunc()->BeginInput(
            pool_.GetContext(), conn->id_, static_cast<uint>(data.size()), cancelled);
        if (raw && !cancelled) {
            std::memcpy(raw, data.data(), data.size());
            conn->send_queue_.pop();
            pool_.GetFunc()->EndInput(pool_.GetContext(), conn->id_);
            atomic_compare_exchange((int&)apphost_lock_, 1, 0);
            return;
        }
        const bool retry = !cancelled;
        atomic_compare_exchange((int&)apphost_lock_, 1, 0);
        if (retry)
            schedule_apphost_drain(conn, generation);
    }

    void on_client_sent(ClientSession* conn) {
        PERF_LOG("[perf] +%lld us  on_client_sent CALLBACK (IoPool finished prior chunk)\n",
                 perf_now_us());
        const uint64_t generation = client_generation(conn);
        if (generation != 0)
            schedule_client_drain(conn, generation);
    }

    void on_apphost_sent(AppHostSession* conn) {
        const uint64_t generation = apphost_generation(conn);
        if (generation != 0)
            schedule_apphost_drain(conn, generation);
    }

    void on_client_received(ClientSession* conn, const uint8_t* data, size_t len) {
        PERF_LOG("[perf] +%lld us  on_client_received bytes=%zu\n",
                 perf_now_us(), len);
        conn->parser_.feed(data, len);
        Packet pkt;
        while (conn->parser_.next_packet(pkt) == ParseResult::OK) {
            if (pkt.header.cmd_type == CmdType::INPUT_EVENT) {
                const std::string input = describe_input_event(pkt);
                std::fprintf(stderr,
                             "[gateway] recv from client  id=%lu cmd=%s session=%u body=%u %s\n",
                             static_cast<unsigned long>(conn->id_),
                             cmd_type_name(pkt.header.cmd_type),
                             static_cast<unsigned>(pkt.header.session_id),
                             static_cast<unsigned>(pkt.header.body_length),
                             input.c_str());
            } else {
                std::fprintf(stderr,
                             "[gateway] recv from client  id=%lu cmd=%s session=%u body=%u\n",
                             static_cast<unsigned long>(conn->id_),
                             cmd_type_name(pkt.header.cmd_type),
                             static_cast<unsigned>(pkt.header.session_id),
                             static_cast<unsigned>(pkt.header.body_length));
            }

            if (conn->session_id_ != 0)
                sessions_.mark_client_seen(conn->session_id_, GatewayClock::now());
            switch (pkt.header.cmd_type) {
            case CmdType::SPAWN_APP:
                handle_spawn_app(conn, pkt);
                break;
            case CmdType::HEARTBEAT:
                // Echo back to the client so its liveness check is satisfied,
                // and forward to the apphost so its `last_apphost_seen` advances
                // via the apphost's own HEARTBEAT echo. Without the forward, an
                // apphost that never Damages (game-over screen, idle UI) hits
                // apphost_timeout_ms even while the client is heart-beating.
                enqueue_client(conn, make_packet(CmdType::HEARTBEAT, pkt.header.session_id));
                if (conn->session_id_ != 0)
                    forward_to_apphost(pkt);
                break;
            case CmdType::CLOSE_SESSION:
                close_session(pkt.header.session_id);
                break;
            default:
                forward_to_apphost(pkt);
                break;
            }
        }
    }

    void on_apphost_received(AppHostSession* conn, const uint8_t* data, size_t len) {
        PERF_LOG("[perf] +%lld us  on_apphost_received bytes=%zu\n",
                 perf_now_us(), len);
        conn->parser_.feed(data, len);
        Packet pkt;
        while (conn->parser_.next_packet(pkt) == ParseResult::OK) {
            std::fprintf(stderr,
                         "[gateway] recv from apphost id=%lu cmd=%s session=%u body=%u\n",
                         static_cast<unsigned long>(conn->id_),
                         cmd_type_name(pkt.header.cmd_type),
                         static_cast<unsigned>(pkt.header.session_id),
                         static_cast<unsigned>(pkt.header.body_length));

            if (pkt.header.session_id != 0)
                sessions_.mark_apphost_seen(pkt.header.session_id, GatewayClock::now());
            switch (pkt.header.cmd_type) {
            case CmdType::APPHOST_READY:
                handle_apphost_ready(conn, pkt.header.session_id);
                break;
            case CmdType::HEARTBEAT:
                enqueue_apphost(conn, make_packet(CmdType::HEARTBEAT, pkt.header.session_id));
                break;
            case CmdType::CLOSE_SESSION:
                close_session(pkt.header.session_id);
                break;
            default:
                forward_to_client(pkt);
                break;
            }
        }
    }

    void handle_spawn_app(ClientSession* conn, const Packet& pkt) {
        if (conn->session_id_ != 0 || sessions_.has_active_or_spawning_for_client(conn->id_)) {
            enqueue_client(conn, make_error(conn->session_id_));
            return;
        }

        if (!config_.auto_spawn_apphost) {
            conn->session_id_ = sessions_.create_for_client(conn->id_);
            enqueue_client(conn, make_packet(CmdType::SESSION_ACK, conn->session_id_));
            return;
        }

        std::string app_name;
        if (!parse_spawn_app_name(pkt, &app_name)) {
            enqueue_client(conn, make_error(0));
            return;
        }
        // check whether app_name is valid
        auto app_it = config_.app_allowlist.find(app_name);
        if (app_it == config_.app_allowlist.end() ||
            config_.apphost_bin.empty() ||
            ::access(config_.apphost_bin.c_str(), X_OK) != 0 ||
            ::access(app_it->second.plugin_path.c_str(), R_OK) != 0) {
            enqueue_client(conn, make_error(0));
            return;
        }

        const auto now = GatewayClock::now();
        const uint32_t session_id = sessions_.create_spawning_session(
            conn->id_, app_name, app_it->second.plugin_path, now,
            std::chrono::milliseconds(config_.spawn_timeout_ms));
        conn->session_id_ = session_id;

        AppHostLaunchConfig launch{};
        launch.session_id = session_id;
        launch.apphost_bin = config_.apphost_bin;
        launch.gateway_host = config_.apphost_bind_host;
        launch.gateway_port = config_.apphost_internal_port;
        launch.plugin_path = app_it->second.plugin_path;
        launch.width = app_it->second.width;
        launch.height = app_it->second.height;

        AppHostProcess proc{};
        if (!process_manager_.spawn_apphost(launch, &proc)) {
            sessions_.remove_session(session_id);
            conn->session_id_ = 0;
            enqueue_client(conn, make_error(session_id));
            return;
        }
        sessions_.set_apphost_pid(session_id, static_cast<int>(proc.pid));

        std::fprintf(stderr,
                     "[gateway] spawned apphost  session=%u app=%s pid=%d\n",
                     static_cast<unsigned>(session_id), app_name.c_str(),
                     static_cast<int>(proc.pid));

        // The client is not acknowledged here. The forked AppHost must first
        // connect back to the internal listener and send APPHOST_READY so the
        // session can be bound to a concrete AppHost socket.
    }

    void handle_apphost_ready(AppHostSession* conn, uint32_t session_id) {
        // APPHOST_READY is the handshake that completes an auto-spawned
        // session: it proves which accepted AppHost connection owns the
        // Gateway-assigned session id.
        if (!sessions_.bind_apphost(session_id, conn->id_))
            return;

        conn->session_id_ = session_id;
        const auto route = sessions_.find(session_id);
        conn->client_conn_ = route.client_conn;

        while (atomic_compare_exchange((int&)client_lock_, 0, 1)) {}
        ClientSession* client = find_client_locked(route.client_conn);
        if (client != nullptr)
            client->apphost_conn_ = conn->id_;
        atomic_compare_exchange((int&)client_lock_, 1, 0);

        if (config_.auto_spawn_apphost && route.client_conn != 0) {
            std::fprintf(stderr,
                         "[gateway] SESSION_ACK  session=%u -> client id=%lu\n",
                         static_cast<unsigned>(session_id),
                         static_cast<unsigned long>(route.client_conn));

            // Only after the AppHost socket is bound can the client safely send
            // INPUT_EVENT messages that Gateway can route to the correct peer.
            send_to_client(route.client_conn, make_packet(CmdType::SESSION_ACK, session_id));
        }
    }

    void forward_to_apphost(const Packet& pkt) {
        const uintptr target = sessions_.find_apphost(pkt.header.session_id);
        if (target != 0) {
            send_to_apphost(target, serialize_packet(pkt));
        } else {
            std::fprintf(stderr,
                         "[gateway] drop client->apphost (no route)  cmd=%s session=%u\n",
                         cmd_type_name(pkt.header.cmd_type),
                         static_cast<unsigned>(pkt.header.session_id));
        }
    }

    void forward_to_client(const Packet& pkt) {
        const uintptr target = sessions_.find_client(pkt.header.session_id);
        if (target != 0) {
            PERF_LOG("[perf] +%lld us  forward_to_client ENTRY cmd=%s body=%u\n",
                     perf_now_us(),
                     cmd_type_name(pkt.header.cmd_type),
                     static_cast<unsigned>(pkt.header.body_length));
            send_to_client(target, serialize_packet(pkt));
        } else {
            std::fprintf(stderr,
                         "[gateway] drop apphost->client (no route)  cmd=%s session=%u\n",
                         cmd_type_name(pkt.header.cmd_type),
                         static_cast<unsigned>(pkt.header.session_id));
        }
    }

    void close_session(uint32_t session_id) {
        std::fprintf(stderr, "[gateway] session close  session=%u\n",
                     static_cast<unsigned>(session_id));
        const auto route = sessions_.find(session_id);
        int pid = -1;
        sessions_.get_pid(session_id, &pid);
        sessions_.remove_session(session_id);
        if (route.client_conn != 0)
            send_to_client(route.client_conn, make_packet(CmdType::CLOSE_SESSION, session_id));
        if (route.apphost_conn != 0)
            send_to_apphost(route.apphost_conn, make_packet(CmdType::CLOSE_SESSION, session_id));
        if (pid > 0)
            process_manager_.terminate(static_cast<pid_t>(pid));
    }

    bool start(uint16_t public_port, uint16_t apphost_internal_port) {
        Gateway::Config cfg{};
        cfg.public_port = public_port;
        cfg.apphost_internal_port = apphost_internal_port;
        cfg.auto_spawn_apphost = false;
        return start(cfg);
    }

    bool start(const Gateway::Config& config) {
        config_ = config;
        // gateway maintains a IoPool 
        pool_ = _IoPool_Fetch();
        if (pool_.IsNull())
            return false;

        bool cancelled = false;
        // listen to the public_port : client_side
        client_listen_id_ = pool_.GetFunc()->StartListen(
            pool_.GetContext(), static_cast<uint>(config_.public_port),
            _IoFunc{on_client_listen_event}, this, cancelled);
        if (client_listen_id_ == 0 || cancelled) {
            _IoPool_Disable();
            return false;
        }

        cancelled = false;
        // listen to the apphost_internal_port : apphost_side
        apphost_listen_id_ = pool_.GetFunc()->StartListen(
            pool_.GetContext(), static_cast<uint>(config_.apphost_internal_port),
            _IoFunc{on_apphost_listen_event}, this, cancelled);
        if (apphost_listen_id_ == 0 || cancelled) {
            pool_.GetFunc()->DisableHandle(pool_.GetContext(), client_listen_id_);
            client_listen_id_ = 0;
            _IoPool_Disable();
            return false;
        }

        std::fprintf(stderr,
                     "[gateway] listening on public=:%u apphost_internal=:%u\n",
                     static_cast<unsigned>(config_.public_port),
                     static_cast<unsigned>(config_.apphost_internal_port));

        running_.store(true, std::memory_order_release);
        monitor_thread_ = std::thread([this] { monitor_loop(); });
        return true;
    }

    void monitor_loop() {
        while (running_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.monitor_interval_ms));
            // First handle failures that the OS can prove: an AppHost child
            // process has exited. waitpid() cannot detect a live-but-stuck
            // AppHost, but it is the authoritative source for crash/exit cases.
            for (const auto& exited : process_manager_.reap_exited()) {
                const uint32_t session_id = sessions_.find_by_pid(static_cast<int>(exited.pid));
                if (session_id != 0)
                    fail_session(session_id, true);
            }

            // Then handle protocol/session timeouts. These cover cases where no
            // child exit is visible yet: AppHost may still be alive but failed to
            // finish the startup handshake, or either side may have stopped
            // sending valid packets after the session became active.
            const auto now = GatewayClock::now();
            for (const auto& snap : sessions_.snapshot_sessions()) {
                if (snap.state == SessionState::SPAWNING && now > snap.spawn_deadline) {
                    // Gateway fork/exec'd an AppHost, but it did not connect back
                    // and send APPHOST_READY before the startup deadline.
                    fail_session(snap.session_id, true);
                    continue;
                }
                if (snap.state == SessionState::ACTIVE) {
                    if (now - snap.last_client_seen >
                        std::chrono::milliseconds(config_.client_timeout_ms)) {
                        // The client side is presumed gone or unresponsive. Do
                        // not send ERROR_RESP to the peer that already timed out.
                        std::fprintf(stderr,
                                     "[gateway] client timeout  session=%u timeout_ms=%d\n",
                                     static_cast<unsigned>(snap.session_id),
                                     config_.client_timeout_ms);
                        fail_session(snap.session_id, false);
                        continue;
                    }
                    if (now - snap.last_apphost_seen >
                        std::chrono::milliseconds(config_.apphost_timeout_ms)) {
                        // The AppHost process/socket may still exist, but the
                        // protocol side is stale; notify the client of backend
                        // failure before tearing down the session.
                        std::fprintf(stderr,
                                     "[gateway] apphost timeout  session=%u timeout_ms=%d\n",
                                     static_cast<unsigned>(snap.session_id),
                                     config_.apphost_timeout_ms);
                        fail_session(snap.session_id, true);
                        continue;
                    }
                }
            }
        }
    }

    void fail_session(uint32_t session_id, bool notify_client_error) {
        std::fprintf(stderr, "[gateway] session fail  session=%u notify_client=%d\n",
                     static_cast<unsigned>(session_id),
                     notify_client_error ? 1 : 0);

        SessionTable::Route route{};
        int pid = -1;
        if (!sessions_.transition_to_closing(session_id, &route, &pid))
            return;
        if (notify_client_error && route.client_conn != 0)
            send_to_client(route.client_conn, make_error(session_id));
        if (route.apphost_conn != 0)
            pool_.GetFunc()->DisableHandle(pool_.GetContext(), route.apphost_conn);
        if (route.client_conn != 0)
            pool_.GetFunc()->DisableHandle(pool_.GetContext(), route.client_conn);
        if (pid > 0)
            process_manager_.terminate(static_cast<pid_t>(pid));
        sessions_.remove_session(session_id);
    }

    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel))
            return;

        std::fprintf(stderr, "[gateway] shutting down...\n");

        if (monitor_thread_.joinable())
            monitor_thread_.join();

        for (const auto& snap : sessions_.snapshot_sessions())
            fail_session(snap.session_id, false);

        if (client_listen_id_ != 0) {
            pool_.GetFunc()->DisableHandle(pool_.GetContext(), client_listen_id_);
            client_listen_id_ = 0;
        }
        if (apphost_listen_id_ != 0) {
            pool_.GetFunc()->DisableHandle(pool_.GetContext(), apphost_listen_id_);
            apphost_listen_id_ = 0;
        }
        _IoPool_Disable();
        for (const auto& exited : process_manager_.reap_exited()) {
            (void)exited;
        }

        while (atomic_compare_exchange((int&)client_lock_, 0, 1)) {}
        ClientSession* c = client_head_;
        client_head_ = nullptr;
        while (c != nullptr) {
            ClientSession* next = c->next_active_;
            call_destructor(*c);
            client_pool_.PutFreeNode(c);
            c = next;
        }
        atomic_compare_exchange((int&)client_lock_, 1, 0);

        while (atomic_compare_exchange((int&)apphost_lock_, 0, 1)) {}
        AppHostSession* a = apphost_head_;
        apphost_head_ = nullptr;
        while (a != nullptr) {
            AppHostSession* next = a->next_active_;
            call_destructor(*a);
            apphost_pool_.PutFreeNode(a);
            a = next;
        }
        atomic_compare_exchange((int&)apphost_lock_, 1, 0);

        {
            std::unique_lock<std::mutex> lk(wait_mtx_);
            cv_.wait(lk, [this] {
                return drain_workers_.load(std::memory_order_acquire) == 0;
            });
        }
        cv_.notify_all();
    }

    void wait_for_drain_workers() noexcept {
        if (drain_workers_.load(std::memory_order_acquire) == 0)
            return;
        std::unique_lock<std::mutex> lk(wait_mtx_);
        cv_.wait(lk, [this] {
            return drain_workers_.load(std::memory_order_acquire) == 0;
        });
    }

    void wait() {
        std::unique_lock<std::mutex> lk(wait_mtx_);
        cv_.wait(lk, [this] {
            return !running_.load(std::memory_order_acquire);
        });
    }

    GKC::LcInterface<_IIoPool> pool_;
    uintptr client_listen_id_ = 0;
    uintptr apphost_listen_id_ = 0;

    free_list<ClientSession> client_pool_;
    ClientSession* client_head_ = nullptr;
    volatile int client_lock_ = 0;
    uint64_t next_client_generation_ = 0;

    free_list<AppHostSession> apphost_pool_;
    AppHostSession* apphost_head_ = nullptr;
    volatile int apphost_lock_ = 0;
    uint64_t next_apphost_generation_ = 0;

    SessionTable sessions_;
    ProcessManager process_manager_;
    Gateway::Config config_;
    std::atomic<bool> running_{false};
    std::atomic<int> drain_workers_{0};
    std::thread monitor_thread_;
    std::mutex wait_mtx_;
    std::condition_variable cv_;
};

Gateway::Gateway() : impl_(std::make_unique<Impl>()) {}

Gateway::~Gateway() {
    if (impl_ && impl_->running_.load(std::memory_order_acquire))
        impl_->stop();
}

bool Gateway::start(uint16_t public_port, uint16_t apphost_internal_port) {
    return impl_->start(public_port, apphost_internal_port);
}

bool Gateway::start(const Config& config) {
    return impl_->start(config);
}

void Gateway::stop() {
    impl_->stop();
}

void Gateway::wait() {
    impl_->wait();
}

bool Gateway::is_running() const {
    return impl_ && impl_->running_.load(std::memory_order_acquire);
}
