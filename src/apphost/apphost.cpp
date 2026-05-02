#include "src/apphost/apphost.h"
#include "src/apphost/pixel_buffer.h"
#include "src/apphost/pixel_capture.h"
#include "src/common/message_handler.h"
#include "src/protocol/protocol.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// GKC headers — available via //third_party:GkcSys include paths.
#include "base/GkcDef.h"
#include "sys/_GkcSys.h"

static void write_be32(byte* dst, uint32_t v) noexcept {
    dst[0] = static_cast<byte>((v >> 24) & 0xFF);
    dst[1] = static_cast<byte>((v >> 16) & 0xFF);
    dst[2] = static_cast<byte>((v >>  8) & 0xFF);
    dst[3] = static_cast<byte>( v        & 0xFF);
}

// ── AppHost::Impl ─────────────────────────────────────────────────────────────

struct AppHost::Impl {

    // ── Per-connection context ─────────────────────────────────────────────
    //
    // Inherits node_base so free_list<ConnContext> can chain free nodes
    // through m_pNext.  The io_func_ member holds the per-connection IoPool
    // callback; SetHandleFunc passes `this` as pIoContext so every event
    // arrives directly at the owning ConnContext.
    struct ConnContext : public node_base {
        uintptr        id_          = 0;      // IoPool handle; needed for BeginInput etc.
        // Connection identity for this pooled slot. A deferred worker may still
        // hold a ConnContext* after the old connection closed and the same
        // free_list slot was reused by a new connection; generation_ prevents
        // that stale worker from operating on the new connection.
        uint64_t       generation_  = 0;
        ConnContext*   next_active_ = nullptr; // intrusive singly-linked active list
        _IoFunc        io_func_;              // Exec = on_io_event; set in constructor
        MessageHandler handler_;             // per-connection CmdType dispatcher
        // Overflow queue: used when BeginInput returns null because a prior send
        // is still in flight. Drained one item at a time after IO_TYPE_SENT returns.
        std::queue<std::vector<uint8_t>> send_queue_;
        Impl*          server_      = nullptr; // Back-pointer to AppHost-wide state from this connection callback.

        ConnContext() noexcept { io_func_.Exec = on_io_event; }

        // Per-connection IoPool callback.
        // ConnContext passes itself as pIoContext, so ctx == this.
        static int on_io_event(void* ctx, int type, uintptr uparam) noexcept {
            auto* conn = static_cast<ConnContext*>(ctx);
            switch (type) {
            case IO_TYPE_RECEIVED: {
                auto* info = reinterpret_cast<_IoRecvInfo*>(uparam);
                conn->handler_.feed(
                    reinterpret_cast<const uint8_t*>(info->p),
                    static_cast<size_t>(info->len));
                break;
            }
            case IO_TYPE_SENT:
                conn->server_->on_sent(conn);
                break;
            case IO_TYPE_BEFORE_CLOSE:
                conn->server_->release_conn(conn);
                break;
            default:
                break;
            }
            return 0;
        }
    };

    // ── Pool helpers ───────────────────────────────────────────────────────

    // Allocate a ConnContext from pool (spinlock-protected), construct it,
    // and insert it into the active tracking list.
    // Returns nullptr if the pool is exhausted (OOM).
    ConnContext* alloc_conn() noexcept {
        ConnContext* conn = nullptr;
        while (atomic_compare_exchange((int&)conn_lock_, 0, 1)) {}
        {
            call_result cr = conn_pool_.FetchFreeNode(conn);
            if (cr.IsSucceeded()) {
                // Construct BEFORE PickFreeNode: if the constructor were to
                // fail, PickFreeNode would not be called and the node stays
                // at the head of the free list — never lost.
                // ConnContext() is noexcept, but the ordering is canonical.
                call_constructor(*conn);
                conn_pool_.PickFreeNode();
                conn->generation_  = ++next_generation_;
                conn->next_active_ = active_head_;
                active_head_       = conn;
            }
        }
        atomic_compare_exchange((int&)conn_lock_, 1, 0);
        return conn;
    }

    // Destruct a ConnContext and return its slot to the pool.
    // Safe against concurrent calls (stop() vs BEFORE_CLOSE): the node is
    // removed from active_head_ under the spinlock first; if it is not found
    // the caller bail-outs, so the destructor runs exactly once.
    void release_conn(ConnContext* conn) noexcept {
        // Step 1: remove from active list under lock.
        // If not found, another code path already released it — do nothing.
        while (atomic_compare_exchange((int&)conn_lock_, 0, 1)) {}
        ConnContext** pp = &active_head_;
        while (*pp && *pp != conn)
            pp = &(*pp)->next_active_;
        const bool was_active = (*pp == conn);
        if (was_active)
            *pp = conn->next_active_;
        atomic_compare_exchange((int&)conn_lock_, 1, 0);

        if (!was_active) return;

        // Step 2: destruct outside lock (destructor may allocate).
        call_destructor(*conn);

        // Step 3: return slot to pool.
        while (atomic_compare_exchange((int&)conn_lock_, 0, 1)) {}
        conn_pool_.PutFreeNode(conn);
        atomic_compare_exchange((int&)conn_lock_, 1, 0);
    }

    // ── IoPool listener callback ───────────────────────────────────────────

    // Listener-level callback; only handles ACCEPT_INIT.
    // Allocates a per-connection context, binds conn->io_func_ via SetHandleFunc,
    // and passes conn itself as pIoContext.
    static int on_listen_event(void* ctx, int type, uintptr uparam) noexcept {
        if (type != IO_TYPE_ACCEPT_INIT) return 0;

        auto*    self    = static_cast<Impl*>(ctx);
        uintptr  conn_id = uparam;

        ConnContext* conn = self->alloc_conn();
        if (conn == nullptr) return 0;  // reject: pool exhausted

        conn->id_     = conn_id;
        conn->server_ = self;

        // Register per-CmdType handlers before SetHandleFunc so they are
        // in place when the first IO_TYPE_RECEIVED event arrives.
        conn->handler_.register_handler(CmdType::SPAWN_APP,
            [self, conn](const Packet& p) {
                self->send_pixel_data(conn, p.header.session_id);
            });
        conn->handler_.register_handler(CmdType::HEARTBEAT,
            [self, conn](const Packet& p) {
                self->send_heartbeat(conn, p.header.session_id);
            });
        conn->handler_.register_handler(CmdType::CLOSE_SESSION,
            [self, conn](const Packet&) {
                self->pool_.GetFunc()->DisableHandle(
                    self->pool_.GetContext(), conn->id_);
            });

        bool cancelled = false;
        // Set new handle Function to the new TCP connection session
        self->pool_.GetFunc()->SetHandleFunc(
            self->pool_.GetContext(), conn_id, conn->io_func_, conn, cancelled);

        if (cancelled) {
            self->release_conn(conn);
            return 0;
        }
        return 1;  // accept
    }

    // ── Send helpers ───────────────────────────────────────────────────────
    // is_active_locked's helper function
    // we are in LOCKED state already
    uint64_t active_generation_locked(ConnContext* conn) const noexcept {
        for (ConnContext* p = active_head_; p != nullptr; p = p->next_active_) {
            if (p == conn)
                return p->generation_;
        }
        return 0;
    }

    bool is_active_locked(ConnContext* conn, uint64_t generation) const noexcept {
        if (generation == 0)
            return false;
        return active_generation_locked(conn) == generation;
    }

    // in UN-LOCKED state
    uint64_t active_generation(ConnContext* conn) noexcept {
        while (atomic_compare_exchange((int&)conn_lock_, 0, 1)) {}
        const uint64_t generation = active_generation_locked(conn);
        atomic_compare_exchange((int&)conn_lock_, 1, 0);
        return generation;
    }

    // Try to fire one serialized buffer via BeginInput/EndInput. If a send is
    // already in flight, preserve ordering by appending to the per-connection
    // queue. conn_lock_ also prevents deferred drain from racing with release.
    void enqueue_send(ConnContext* conn, std::vector<uint8_t> data) {
        while (atomic_compare_exchange((int&)conn_lock_, 0, 1)) {}
        if (!is_active_locked(conn, conn->generation_)) {
            atomic_compare_exchange((int&)conn_lock_, 1, 0);
            return;
        }

        if (!conn->send_queue_.empty()) {
            conn->send_queue_.push(std::move(data));
            atomic_compare_exchange((int&)conn_lock_, 1, 0);
            return;
        }
        // conn->send_queue_.empty()
        // I/O state
        bool cancelled = false;
        // alloc a GKC buffer which waiting for writing.
        byte* raw = pool_.GetFunc()->BeginInput(
            pool_.GetContext(), conn->id_,
            static_cast<uint>(data.size()), cancelled);
        if (raw && !cancelled) {
            // alloc succeed;
            std::memcpy(raw, data.data(), data.size());
            pool_.GetFunc()->EndInput(pool_.GetContext(), conn->id_);
        } else if (!cancelled) {
            // BeginInput busy: defer until IO_TYPE_SENT.
            conn->send_queue_.push(std::move(data));
        }
        atomic_compare_exchange((int&)conn_lock_, 1, 0);
    }

    void schedule_deferred_drain(ConnContext* conn, uint64_t generation) {
        // drain_workers_ tracks detached drain threads that still may access
        // this Impl, pool_, conn_lock_, or ConnContext pointers. Because these
        // threads are detached, stop() cannot join them directly; it waits for
        // this counter to return to zero before allowing Impl shutdown to
        // finish. This is a lifetime guard, not a send-ordering mechanism.
        drain_workers_.fetch_add(1, std::memory_order_acq_rel);
        std::thread([this, conn, generation] {
            // GKC clears its iSend flag after IO_TYPE_SENT returns to it.
            // Deferring avoids re-entering BeginInput while that flag is still set.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            drain_send_queue(conn, generation);
            if (drain_workers_.fetch_sub(1, std::memory_order_acq_rel) == 1)
                cv_.notify_all();
        }).detach();
    }

    // Called after IO_TYPE_SENT returns to GKC. Flush at most one queued buffer;
    // the next IO_TYPE_SENT will schedule the next drain and preserve ordering.
    void drain_send_queue(ConnContext* conn, uint64_t generation) {
        while (atomic_compare_exchange((int&)conn_lock_, 0, 1)) {}
        if (!is_active_locked(conn, generation) || conn->send_queue_.empty()) {
            atomic_compare_exchange((int&)conn_lock_, 1, 0);
            return;
        }
        // from conn->send_queue_ get the data() and memcpy in the GKC buffer
        const auto& data = conn->send_queue_.front();
        bool cancelled = false;
        byte* raw = pool_.GetFunc()->BeginInput(
            pool_.GetContext(), conn->id_,
            static_cast<uint>(data.size()), cancelled);
        if (raw && !cancelled) {
            std::memcpy(raw, data.data(), data.size());
            conn->send_queue_.pop();
            pool_.GetFunc()->EndInput(pool_.GetContext(), conn->id_);
            atomic_compare_exchange((int&)conn_lock_, 1, 0);
            return;
        }

        const bool retry = !cancelled;
        atomic_compare_exchange((int&)conn_lock_, 1, 0);
        if (retry)
            schedule_deferred_drain(conn, generation);
    }

    // IO_TYPE_SENT fires before GKC clears iSend. Schedule an out-of-callback
    // drain instead of calling BeginInput immediately.
    void on_sent(ConnContext* conn) {
        const uint64_t generation = active_generation(conn);
        if (generation != 0)
            schedule_deferred_drain(conn, generation);
    }

    // ── Packet handling ────────────────────────────────────────────────────

    // Sends one 16x16 solid-red PIXEL_DATA packet.
    void send_pixel_data(ConnContext* conn, uint32_t session_id) {
        const int      w        = frame_buf_.width();
        const int      h        = frame_buf_.height();
        const auto     pixels   = capture_rect(frame_buf_.data(), w, 0, 0, w, h);
        const uint32_t rect_r   = static_cast<uint32_t>(w);
        const uint32_t rect_b   = static_cast<uint32_t>(h);
        const uint32_t data_len = static_cast<uint32_t>(pixels.size());
        const uint32_t body_len = 24 + data_len;  // 6 × 4-byte fields + pixels

        Header hdr{};
        hdr.magic       = PROTOCOL_MAGIC;
        hdr.session_id  = session_id;
        hdr.cmd_type    = CmdType::PIXEL_DATA;
        hdr.body_length = body_len;

        std::vector<uint8_t> buf(HEADER_SIZE + body_len);
        serialize_header(hdr, buf.data());
        byte* p = reinterpret_cast<byte*>(buf.data()) + HEADER_SIZE;
        write_be32(p,       1);        // frameSeq
        write_be32(p +  4,  0);        // rectLeft
        write_be32(p +  8,  0);        // rectTop
        write_be32(p + 12,  rect_r);   // rectRight
        write_be32(p + 16,  rect_b);   // rectBottom
        write_be32(p + 20,  data_len); // dataLen
        std::memcpy(p + 24, pixels.data(), data_len);

        enqueue_send(conn, std::move(buf));
    }

    // Sends a zero-body HEARTBEAT reply.
    void send_heartbeat(ConnContext* conn, uint32_t session_id) {
        Header hdr{};
        hdr.magic       = PROTOCOL_MAGIC;
        hdr.session_id  = session_id;
        hdr.cmd_type    = CmdType::HEARTBEAT;
        hdr.body_length = 0;

        std::vector<uint8_t> buf(HEADER_SIZE);
        serialize_header(hdr, buf.data());
        enqueue_send(conn, std::move(buf));
    }

    // ── Lifecycle ──────────────────────────────────────────────────────────

    bool start(uint16_t port) {
        pool_ = _IoPool_Fetch();
        if (pool_.IsNull()) return false;
        // on_listen_event is the callback function for listen event
        _IoFunc lf{on_listen_event};
        bool cancelled = false;
        // pContext : pool_.GetContext get the context of the IoPool
        // pIoContext : this gets the context of the AppHost::Impl's context
        listen_id_ = pool_.GetFunc()->StartListen(
            pool_.GetContext(), static_cast<uint>(port), lf, this, cancelled);

        if (listen_id_ == 0 || cancelled) {
            _IoPool_Disable();
            return false;
        }
        running_.store(true, std::memory_order_release);
        return true;
    }

    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;

        if (listen_id_ != 0) {
            pool_.GetFunc()->DisableHandle(pool_.GetContext(), listen_id_);
            listen_id_ = 0;
        }
        _IoPool_Disable();

        // Destroy any ConnContexts not freed via BEFORE_CLOSE.
        // IoPool's DisableAll() does not guarantee BEFORE_CLOSE fires for
        // connections still in the close-list at shutdown time.
        while (atomic_compare_exchange((int&)conn_lock_, 0, 1)) {}
        ConnContext* p = active_head_;
        active_head_ = nullptr;
        while (p != nullptr) {
            ConnContext* next = p->next_active_;
            call_destructor(*p);
            conn_pool_.PutFreeNode(p);
            p = next;
        }
        atomic_compare_exchange((int&)conn_lock_, 1, 0);

        {
            std::unique_lock<std::mutex> lk(wait_mtx_);
            cv_.wait(lk, [this] {
                return drain_workers_.load(std::memory_order_acquire) == 0;
            });
        }

        cv_.notify_all();
    }

    void wait() {
        std::unique_lock<std::mutex> lk(wait_mtx_);
        cv_.wait(lk, [this] {
            return !running_.load(std::memory_order_acquire);
        });
    }

    // ── State ──────────────────────────────────────────────────────────────
    
    GKC::LcInterface<_IIoPool> pool_;
    uintptr                    listen_id_ = 0;
    // A AppHost maintains a variable
    PixelBuffer                frame_buf_{16, 16};

    // Connection pool: pre-allocated ConnContext nodes, no per-connection
    // heap allocation.  conn_lock_ is an atomic spinlock (0=free, 1=held)
    // protecting both conn_pool_ and active_head_.
    free_list<ConnContext>     conn_pool_;
    // active_head_ : records every active ConnContext Node
    ConnContext*               active_head_ = nullptr;
    volatile int               conn_lock_   = 0;
    // Monotonic source for ConnContext::generation_; this is not a session id
    // or connection count, only a per-allocation identity guard for reused slots.
    uint64_t                   next_generation_ = 0;
    std::atomic<bool>          running_{false};
    // Number of detached deferred-drain threads currently running. stop()
    // waits until this reaches zero so no background thread can access Impl
    // state after shutdown begins to tear it down.
    std::atomic<int>           drain_workers_{0};
    std::mutex                 wait_mtx_;
    std::condition_variable    cv_;
};

// ── AppHost public API ───────────────────────────────────────────────────────

AppHost::AppHost() : impl_(std::make_unique<Impl>()) {
    impl_->frame_buf_.fill_solid_color(0xFF, 0x00, 0x00, 0xFF);  // solid red
}

AppHost::~AppHost() {
    if (impl_ && impl_->running_.load(std::memory_order_acquire))
        impl_->stop();
}

bool AppHost::start(uint16_t port)  { return impl_->start(port); }
void AppHost::stop()                { impl_->stop(); }
void AppHost::wait()                { impl_->wait(); }
bool AppHost::is_running() const {
    return impl_ && impl_->running_.load(std::memory_order_acquire);
}
