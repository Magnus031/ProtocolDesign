// IoPool integration tests.
//
// _IoPool_Fetch() / _IoPool_Disable() operate on a process-global singleton.
// Once _IoPool_Disable() is called the pool cannot be re-initialised in the
// same process, so every scenario is sequenced inside a SINGLE TEST function.
// The POSIX socket side acts as the "other end" for all loopback tests.
//
// IO_TYPE_BEFORE_CLOSE is intentionally NOT asserted in these tests.
// disable_handle() shuts the socket down immediately (SHUT_RDWR) but adds the
// handle to a deferred close-list with a RECV_TIMEOUT (5 min) grace window
// before BEFORE_CLOSE fires.  Testing a 5-minute delay is impractical here.

#include <gtest/gtest.h>
#include "base/GkcDef.h"
#include "sys/_GkcSys.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// ── Semaphore ─────────────────────────────────────────────────────────────────
// Lightweight counting semaphore used to wait for async IoPool callbacks.

struct Sem {
    std::mutex              mtx;
    std::condition_variable cv;
    int                     count{0};

    void post() noexcept {
        std::lock_guard<std::mutex> lk(mtx);
        ++count;
        cv.notify_one();
    }

    // Returns true if the semaphore was decremented within timeout_ms.
    bool wait_ms(int timeout_ms) {
        std::unique_lock<std::mutex> lk(mtx);
        bool ok = cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                              [this] { return count > 0; });
        if (ok) --count;
        return ok;
    }
};

// ── POSIX socket helpers ──────────────────────────────────────────────────────

// Create a blocking TCP socket connected to 127.0.0.1:port.
// Returns -1 on failure.
static int posix_connect(uint16_t port) {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct timeval tv{5, 0};
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    ::inet_aton("127.0.0.1", &addr.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(s);
        return -1;
    }
    return s;
}

// Write all n bytes to sock.  Returns false on error.
static bool posix_send_all(int s, const uint8_t* buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = ::send(s, buf + sent, n - sent, 0);
        if (r <= 0) return false;
        sent += static_cast<size_t>(r);
    }
    return true;
}

// Read exactly n bytes from sock.
// Returns number of bytes read; < n means peer closed or timeout.
static ssize_t posix_recv_exact(int s, uint8_t* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::recv(s, buf + got, n - got, 0);
        if (r < 0) return -1;
        if (r == 0) return static_cast<ssize_t>(got);
        got += static_cast<size_t>(r);
    }
    return static_cast<ssize_t>(got);
}

// ── Per-connection state ──────────────────────────────────────────────────────
// Shared by server-side (accepted) and client-side (StartConnect) handles.

struct ConnState {
    GKC::LcInterface<_IIoPool>* pool{nullptr};
    uintptr id{0};

    Sem accepted;      // fires on IO_TYPE_ACCEPTED or IO_TYPE_CONNECTED
    Sem received;      // fires on IO_TYPE_RECEIVED
    Sem sent;          // fires on IO_TYPE_SENT
    Sem recv_error;    // fires on IO_TYPE_RECV_ERROR
    Sem connect_error; // fires on IO_TYPE_CONNECT_ERROR

    std::vector<uint8_t> recv_data;
    std::mutex           recv_mtx;
};

// Callback for accepted / connected handles.
static int conn_cb(void* ctx, int type, uintptr param) noexcept {
    auto* cs = static_cast<ConnState*>(ctx);
    switch (type) {
    case IO_TYPE_ACCEPTED:
    case IO_TYPE_CONNECTED:
        cs->accepted.post();
        break;
    case IO_TYPE_RECEIVED: {
        const auto* info = reinterpret_cast<const _IoRecvInfo*>(param);
        {
            std::lock_guard<std::mutex> lk(cs->recv_mtx);
            cs->recv_data.insert(cs->recv_data.end(),
                                 info->p, info->p + info->len);
        }
        cs->received.post();
        break;
    }
    case IO_TYPE_SENT:
        cs->sent.post();
        break;
    case IO_TYPE_RECV_ERROR:
        cs->recv_error.post();
        break;
    case IO_TYPE_CONNECT_ERROR:
        cs->connect_error.post();
        break;
    default:
        break;
    }
    return 0;
}

// ── Per-listener state ────────────────────────────────────────────────────────

struct ListenState {
    GKC::LcInterface<_IIoPool>* pool{nullptr};
    ConnState*                  conn_st{nullptr};
    int                         accept_return{1}; // 1 = accept, 0 = reject
    Sem                         accept_error;
};

// Callback for the listen handle.
// On ACCEPT_INIT: calls SetHandleFunc to assign conn_cb + ConnState to the
// new connection, then returns accept_return to the pool.
static int listen_cb(void* ctx, int type, uintptr param) noexcept {
    auto* ls = static_cast<ListenState*>(ctx);
    switch (type) {
    case IO_TYPE_ACCEPT_INIT: {
        if (ls->accept_return == 0)
            return 0; // reject the connection
        ls->conn_st->id = param;
        bool cancelled  = false;
        ls->pool->GetFunc()->SetHandleFunc(
            ls->pool->GetContext(), param,
            _IoFunc{conn_cb}, ls->conn_st, cancelled);
        return 1;
    }
    case IO_TYPE_ACCEPT_ERROR:
        ls->accept_error.post();
        break;
    default:
        break;
    }
    return 0;
}

// ── Multi-connection listener (S8) ────────────────────────────────────────────
// Accepts up to MAX_CONNS connections and routes each to its own ConnState.

static constexpr int MAX_MULTI_CONNS = 4;

struct MultiListenState {
    GKC::LcInterface<_IIoPool>* pool{nullptr};
    std::atomic<int>            next_idx{0};
    ConnState                   cs[MAX_MULTI_CONNS];
};

static int multi_listen_cb(void* ctx, int type, uintptr param) noexcept {
    auto* mls = static_cast<MultiListenState*>(ctx);
    if (type == IO_TYPE_ACCEPT_INIT) {
        int idx = mls->next_idx.fetch_add(1, std::memory_order_relaxed);
        if (idx >= MAX_MULTI_CONNS)
            return 0; // pool is full, reject
        mls->cs[idx].id = param;
        bool cancelled  = false;
        mls->pool->GetFunc()->SetHandleFunc(
            mls->pool->GetContext(), param,
            _IoFunc{conn_cb}, &mls->cs[idx], cancelled);
        return 1;
    }
    return 0;
}

// ── Test ──────────────────────────────────────────────────────────────────────

// Ports are intentionally spread out to avoid TIME_WAIT overlap between
// scenarios; none collide with apphost_test.cpp (which uses 19001).
static constexpr uint16_t PORT_S2 = 19201; // listen + accept
static constexpr uint16_t PORT_S3 = 19202; // receive data
static constexpr uint16_t PORT_S4 = 19203; // send data
static constexpr uint16_t PORT_S5 = 19204; // accept rejection
static constexpr uint16_t PORT_S6 = 19205; // DisableHandle
static constexpr uint16_t PORT_S7 = 19206; // BeginInput double-call guard
static constexpr uint16_t PORT_S8 = 19207; // multiple concurrent connections
static constexpr uint16_t PORT_S9 = 19208; // StartConnect (outbound)

TEST(IoPoolTest, AllScenarios) {

    // ── S1: Pool fetch ────────────────────────────────────────────────────────
    GKC::LcInterface<_IIoPool> pool = _IoPool_Fetch();
    ASSERT_FALSE(pool.IsNull()) << "S1: _IoPool_Fetch() returned null";

    // A second Fetch must return the same live singleton.
    GKC::LcInterface<_IIoPool> pool2 = _IoPool_Fetch();
    EXPECT_FALSE(pool2.IsNull())           << "S1: second Fetch returned null";
    EXPECT_EQ(pool.GetContext(), pool2.GetContext())
        << "S1: second Fetch returned a different context (not a singleton)";

    auto* fn   = pool.GetFunc().Get();  // _IIoPool function table
    void* pctx = pool.GetContext();     // IoPool instance pointer

    // ── S2: Listen + Accept lifecycle ─────────────────────────────────────────
    // Verifies: ACCEPT_INIT fires with a valid id, SetHandleFunc assigns conn_cb,
    // and ACCEPTED fires on the new connection handle.
    {
        ConnState   cs;  cs.pool = &pool;
        ListenState ls;  ls.pool = &pool;  ls.conn_st = &cs;

        bool    cancelled = false;
        uintptr listen_id = fn->StartListen(pctx, PORT_S2,
                                            _IoFunc{listen_cb}, &ls, cancelled);
        ASSERT_NE(listen_id, 0u)  << "S2: StartListen failed";
        ASSERT_FALSE(cancelled)   << "S2: StartListen was cancelled";

        int sock = posix_connect(PORT_S2);
        ASSERT_GE(sock, 0) << "S2: POSIX connect failed";

        EXPECT_TRUE(cs.accepted.wait_ms(2000)) << "S2: ACCEPTED did not fire";
        EXPECT_NE(cs.id, 0u) << "S2: connection id was not assigned in ACCEPT_INIT";

        fn->DisableHandle(pctx, listen_id);
        ::close(sock);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ── S3: Receive data ──────────────────────────────────────────────────────
    // POSIX client sends a known byte sequence; verifies that IO_TYPE_RECEIVED
    // fires and the received bytes match exactly.
    {
        ConnState   cs;  cs.pool = &pool;
        ListenState ls;  ls.pool = &pool;  ls.conn_st = &cs;

        bool    cancelled = false;
        uintptr listen_id = fn->StartListen(pctx, PORT_S3,
                                            _IoFunc{listen_cb}, &ls, cancelled);
        ASSERT_NE(listen_id, 0u) << "S3: StartListen failed";

        int sock = posix_connect(PORT_S3);
        ASSERT_GE(sock, 0) << "S3: POSIX connect failed";
        ASSERT_TRUE(cs.accepted.wait_ms(2000)) << "S3: ACCEPTED not received";

        const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03};
        ASSERT_TRUE(posix_send_all(sock, payload, sizeof(payload)));

        EXPECT_TRUE(cs.received.wait_ms(2000)) << "S3: IO_TYPE_RECEIVED did not fire";
        {
            std::lock_guard<std::mutex> lk(cs.recv_mtx);
            ASSERT_EQ(cs.recv_data.size(), sizeof(payload))
                << "S3: received byte count mismatch";
            EXPECT_EQ(std::memcmp(cs.recv_data.data(), payload, sizeof(payload)), 0)
                << "S3: received bytes do not match sent payload";
        }

        fn->DisableHandle(pctx, listen_id);
        ::close(sock);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ── S4: Send data (BeginInput / EndInput) ─────────────────────────────────
    // Server calls BeginInput to obtain the send buffer, writes a payload,
    // then EndInput to submit.  Verifies IO_TYPE_SENT fires and the POSIX
    // client receives the exact bytes that were written.
    {
        ConnState   cs;  cs.pool = &pool;
        ListenState ls;  ls.pool = &pool;  ls.conn_st = &cs;

        bool    cancelled = false;
        uintptr listen_id = fn->StartListen(pctx, PORT_S4,
                                            _IoFunc{listen_cb}, &ls, cancelled);
        ASSERT_NE(listen_id, 0u) << "S4: StartListen failed";

        int sock = posix_connect(PORT_S4);
        ASSERT_GE(sock, 0) << "S4: POSIX connect failed";
        ASSERT_TRUE(cs.accepted.wait_ms(2000)) << "S4: ACCEPTED not received";

        const uint8_t payload[] = {0xCA, 0xFE, 0xBA, 0xBE};
        bool send_cancelled = false;
        byte* buf = fn->BeginInput(pctx, cs.id,
                                   static_cast<uint>(sizeof(payload)),
                                   send_cancelled);
        ASSERT_NE(buf, nullptr)      << "S4: BeginInput returned null";
        ASSERT_FALSE(send_cancelled) << "S4: BeginInput was cancelled";
        std::memcpy(buf, payload, sizeof(payload));
        ASSERT_TRUE(fn->EndInput(pctx, cs.id)) << "S4: EndInput failed";

        EXPECT_TRUE(cs.sent.wait_ms(2000)) << "S4: IO_TYPE_SENT did not fire";

        uint8_t recv_buf[sizeof(payload)];
        ssize_t got = posix_recv_exact(sock, recv_buf, sizeof(payload));
        EXPECT_EQ(got, static_cast<ssize_t>(sizeof(payload)))
            << "S4: POSIX client did not receive all bytes";
        if (got == static_cast<ssize_t>(sizeof(payload))) {
            EXPECT_EQ(std::memcmp(recv_buf, payload, sizeof(payload)), 0)
                << "S4: POSIX client received wrong data";
        }

        fn->DisableHandle(pctx, listen_id);
        ::close(sock);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ── S5: ACCEPT_INIT rejection ─────────────────────────────────────────────
    // When the listen callback returns 0 from ACCEPT_INIT, the pool must close
    // the new socket.  The POSIX client should receive EOF (recv returns 0).
    {
        ConnState   cs;  cs.pool = &pool;
        ListenState ls;  ls.pool = &pool;  ls.conn_st = &cs;
        ls.accept_return = 0;

        bool    cancelled = false;
        uintptr listen_id = fn->StartListen(pctx, PORT_S5,
                                            _IoFunc{listen_cb}, &ls, cancelled);
        ASSERT_NE(listen_id, 0u) << "S5: StartListen failed";

        int sock = posix_connect(PORT_S5);
        ASSERT_GE(sock, 0) << "S5: POSIX connect failed";

        // Pool frees the handle immediately; client must see EOF or error.
        uint8_t tmp;
        ssize_t r = ::recv(sock, &tmp, 1, 0);
        EXPECT_LE(r, 0) << "S5: expected EOF after rejection but got " << r << " bytes";

        fn->DisableHandle(pctx, listen_id);
        ::close(sock);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ── S6: DisableHandle closes connection ───────────────────────────────────
    // After accepting a connection, calling DisableHandle shuts down the socket
    // (SHUT_RDWR) immediately.  The POSIX client should therefore see EOF.
    // IO_TYPE_BEFORE_CLOSE is NOT asserted here: the pool defers it by
    // RECV_TIMEOUT (5 minutes) after the close-list entry is created.
    {
        ConnState   cs;  cs.pool = &pool;
        ListenState ls;  ls.pool = &pool;  ls.conn_st = &cs;

        bool    cancelled = false;
        uintptr listen_id = fn->StartListen(pctx, PORT_S6,
                                            _IoFunc{listen_cb}, &ls, cancelled);
        ASSERT_NE(listen_id, 0u) << "S6: StartListen failed";

        int sock = posix_connect(PORT_S6);
        ASSERT_GE(sock, 0) << "S6: POSIX connect failed";
        ASSERT_TRUE(cs.accepted.wait_ms(2000)) << "S6: ACCEPTED not received";

        fn->DisableHandle(pctx, cs.id);

        // Socket was shut down; client must see EOF within a short timeout.
        struct timeval tv{3, 0};
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        uint8_t tmp;
        ssize_t r = ::recv(sock, &tmp, 1, 0);
        EXPECT_LE(r, 0) << "S6: expected EOF after DisableHandle, got " << r;

        fn->DisableHandle(pctx, listen_id);
        ::close(sock);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ── S7: BeginInput send-lock guard ────────────────────────────────────────
    // The pool uses an iSend flag to prevent overlapping sends.
    // A second BeginInput before the first EndInput must return NULL.
    // After the SENT callback, BeginInput must succeed again.
    {
        ConnState   cs;  cs.pool = &pool;
        ListenState ls;  ls.pool = &pool;  ls.conn_st = &cs;

        bool    cancelled = false;
        uintptr listen_id = fn->StartListen(pctx, PORT_S7,
                                            _IoFunc{listen_cb}, &ls, cancelled);
        ASSERT_NE(listen_id, 0u) << "S7: StartListen failed";

        int sock = posix_connect(PORT_S7);
        ASSERT_GE(sock, 0) << "S7: POSIX connect failed";
        ASSERT_TRUE(cs.accepted.wait_ms(2000)) << "S7: ACCEPTED not received";

        // First BeginInput acquires the send lock (iSend 0 → 1).
        bool c1  = false;
        byte* b1 = fn->BeginInput(pctx, cs.id, 4, c1);
        ASSERT_NE(b1, nullptr) << "S7: first BeginInput failed";
        ASSERT_FALSE(c1);

        // Second BeginInput must fail while the lock is held.
        bool c2  = false;
        byte* b2 = fn->BeginInput(pctx, cs.id, 4, c2);
        EXPECT_EQ(b2, nullptr)
            << "S7: second BeginInput should return NULL (send in progress)";

        // Complete the pending send.
        std::memset(b1, 0xAB, 4);
        ASSERT_TRUE(fn->EndInput(pctx, cs.id));
        EXPECT_TRUE(cs.sent.wait_ms(2000)) << "S7: SENT did not fire";

        // After SENT the lock is released; BeginInput must succeed again.
        bool c3  = false;
        byte* b3 = fn->BeginInput(pctx, cs.id, 4, c3);
        EXPECT_NE(b3, nullptr)
            << "S7: BeginInput after SENT callback should succeed";
        if (b3) {
            std::memset(b3, 0xCD, 4);
            fn->EndInput(pctx, cs.id);
            cs.sent.wait_ms(2000); // drain SENT before cleanup
        }

        fn->DisableHandle(pctx, listen_id);
        ::close(sock);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ── S8: Multiple concurrent connections ───────────────────────────────────
    // Two POSIX clients connect simultaneously.  Each must get its own
    // ACCEPTED callback and independent receive/send state.
    {
        MultiListenState mls;
        mls.pool = &pool;
        for (auto& cs : mls.cs) cs.pool = &pool;

        bool    cancelled = false;
        uintptr listen_id = fn->StartListen(pctx, PORT_S8,
                                            _IoFunc{multi_listen_cb}, &mls,
                                            cancelled);
        ASSERT_NE(listen_id, 0u) << "S8: StartListen failed";

        int sock0 = posix_connect(PORT_S8);
        int sock1 = posix_connect(PORT_S8);
        ASSERT_GE(sock0, 0) << "S8: first POSIX connect failed";
        ASSERT_GE(sock1, 0) << "S8: second POSIX connect failed";

        EXPECT_TRUE(mls.cs[0].accepted.wait_ms(2000)) << "S8: first ACCEPTED missing";
        EXPECT_TRUE(mls.cs[1].accepted.wait_ms(2000)) << "S8: second ACCEPTED missing";

        // Send distinct payloads so we can verify independent routing.
        const uint8_t payload0[] = {0x11, 0x22, 0x33};
        const uint8_t payload1[] = {0xAA, 0xBB, 0xCC};
        ASSERT_TRUE(posix_send_all(sock0, payload0, sizeof(payload0)));
        ASSERT_TRUE(posix_send_all(sock1, payload1, sizeof(payload1)));

        EXPECT_TRUE(mls.cs[0].received.wait_ms(2000)) << "S8: first RECEIVED missing";
        EXPECT_TRUE(mls.cs[1].received.wait_ms(2000)) << "S8: second RECEIVED missing";

        {
            std::lock_guard<std::mutex> lk0(mls.cs[0].recv_mtx);
            EXPECT_EQ(std::memcmp(mls.cs[0].recv_data.data(),
                                  payload0, sizeof(payload0)), 0)
                << "S8: first connection received wrong data";
        }
        {
            std::lock_guard<std::mutex> lk1(mls.cs[1].recv_mtx);
            EXPECT_EQ(std::memcmp(mls.cs[1].recv_data.data(),
                                  payload1, sizeof(payload1)), 0)
                << "S8: second connection received wrong data";
        }

        fn->DisableHandle(pctx, listen_id);
        ::close(sock0);
        ::close(sock1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ── S9: StartConnect (outbound connection) ────────────────────────────────
    // IoPool initiates the TCP connection to a local POSIX server.
    // Verifies IO_TYPE_CONNECTED fires and bidirectional data exchange works.
    {
        // Create a POSIX listen socket first.
        int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(server_fd, 0) << "S9: failed to create POSIX server socket";
        int opt = 1;
        ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(PORT_S9);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        ASSERT_EQ(::bind(server_fd,
                         reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0)
            << "S9: POSIX bind failed";
        ASSERT_EQ(::listen(server_fd, 5), 0) << "S9: POSIX listen failed";

        // Accept the IoPool connection in a background thread.
        int client_fd = -1;
        std::thread accept_thread([&] {
            struct timeval tv{5, 0};
            ::setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            client_fd = ::accept(server_fd, nullptr, nullptr);
        });

        // IoPool starts an outbound connection.
        ConnState cs;  cs.pool = &pool;
        bool    cancelled = false;
        uintptr conn_id   = fn->StartConnect(pctx, "127.0.0.1", PORT_S9,
                                             _IoFunc{conn_cb}, &cs, cancelled);
        ASSERT_NE(conn_id, 0u) << "S9: StartConnect returned 0";
        ASSERT_FALSE(cancelled) << "S9: StartConnect was cancelled";

        EXPECT_TRUE(cs.accepted.wait_ms(3000)) << "S9: IO_TYPE_CONNECTED did not fire";

        accept_thread.join();
        ASSERT_GE(client_fd, 0) << "S9: POSIX accept failed";

        // POSIX server sends data; IoPool client should receive it.
        const uint8_t server_payload[] = {0x55, 0x66, 0x77, 0x88};
        ASSERT_TRUE(posix_send_all(client_fd, server_payload,
                                   sizeof(server_payload)));
        EXPECT_TRUE(cs.received.wait_ms(2000)) << "S9: IO_TYPE_RECEIVED did not fire";
        {
            std::lock_guard<std::mutex> lk(cs.recv_mtx);
            ASSERT_EQ(cs.recv_data.size(), sizeof(server_payload));
            EXPECT_EQ(std::memcmp(cs.recv_data.data(), server_payload,
                                  sizeof(server_payload)), 0)
                << "S9: received data mismatch";
        }

        // IoPool client sends data back; POSIX server verifies.
        const uint8_t client_payload[] = {0x11, 0x22};
        bool sc = false;
        byte* buf = fn->BeginInput(pctx, conn_id,
                                   static_cast<uint>(sizeof(client_payload)), sc);
        ASSERT_NE(buf, nullptr) << "S9: BeginInput failed";
        std::memcpy(buf, client_payload, sizeof(client_payload));
        fn->EndInput(pctx, conn_id);
        EXPECT_TRUE(cs.sent.wait_ms(2000)) << "S9: IO_TYPE_SENT did not fire";

        uint8_t rbuf[sizeof(client_payload)];
        ssize_t got = posix_recv_exact(client_fd, rbuf, sizeof(client_payload));
        EXPECT_EQ(got, static_cast<ssize_t>(sizeof(client_payload)));
        if (got == static_cast<ssize_t>(sizeof(client_payload))) {
            EXPECT_EQ(std::memcmp(rbuf, client_payload,
                                  sizeof(client_payload)), 0)
                << "S9: POSIX server received wrong data";
        }

        fn->DisableHandle(pctx, conn_id);
        ::close(client_fd);
        ::close(server_fd);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ── Final teardown ────────────────────────────────────────────────────────
    // _IoPool_Disable() is irreversible in this process; call it exactly once
    // at the very end so all preceding scenarios share the same live pool.
    _IoPool_Disable();
}
