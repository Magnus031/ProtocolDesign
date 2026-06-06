// End-to-end response-latency benchmark via a headless fake client.
//
// Boots a real Gateway with auto-spawn AppHost, loads the real app_2048
// plugin, and connects a fake TCP client over loopback that:
//
//   1. sends SPAWN_APP=app_2048 and drains the SESSION_ACK plus initial
//      PIXEL_DATA frames produced by the plugin's first paint;
//   2. enters a measurement loop where each iteration:
//        - records t0,
//        - sends one INPUT_EVENT (arrow key or RESET, depending on case),
//        - drives MessageParser over the inbound TCP stream until it sees
//          the first PIXEL_DATA whose frame_seq is strictly greater than
//          the baseline frame_seq recorded before send,
//        - records t1, pushes (t1 - t0) as the end-to-end latency sample.
//
// What this measures:
//   INPUT_EVENT packing → kernel loopback TCP up → MessageParser on Gateway
//   → Gateway route lookup → serialize+chunk+enqueue → IoPool TX → kernel
//   loopback TCP down → MessageParser on AppHost → AppHost dispatch into
//   plugin → plugin redraws board → pixel_capture pack → IoPool TX back to
//   Gateway → Gateway re-serialize+chunk → IoPool TX to fake client →
//   client-side MessageParser.
//
// What this does NOT measure:
//   Cross-host network RTT (everything is loopback on one Linux box).
//   Real client-side GUI rendering (fake client never blits pixels).
//
// Build:  bazel build -c opt //src/gateway:fake_e2e_client_bench
// Run example:
//   bazel-bin/src/gateway/fake_e2e_client_bench
//       --apphost-bin=bazel-bin/src/apphost/apphost_bin
//       --app-2048=bazel-bin/plugins/app_2048/libapp_2048.so
//       [--iters=100] [--warmup=10]
//       [--log=docs/perf-logs/fake_e2e_client.csv]

#include "src/common/input_event.h"
#include "src/gateway/gateway.h"
#include "src/protocol/message_parser.h"
#include "src/protocol/protocol.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

constexpr int kDefaultBenchIters  = 100;
constexpr int kDefaultWarmupIters = 10;

// ── TEMP PERF DIAGNOSTIC ────────────────────────────────────────────────────
static const bool g_perf_trace = std::getenv("GATEWAY_PERF_TRACE") != nullptr;
static const auto g_perf_t0    = std::chrono::steady_clock::now();
static long long perf_now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - g_perf_t0).count();
}
#define PERF_LOG(...) do { if (g_perf_trace) std::fprintf(stderr, __VA_ARGS__); } while (0)
// ────────────────────────────────────────────────────────────────────────────

// Key codes (mirrored from third_party/GKC/public/include/base/system/ui_types.h
// and app_2048.cpp dispatch table).
constexpr uint16_t kKeyLeft  = 0x25;
constexpr uint16_t kKeyUp    = 0x26;
constexpr uint16_t kKeyRight = 0x27;
constexpr uint16_t kKeyDown  = 0x28;
constexpr uint16_t kKeyF2    = 0x71;  // RESET

// app_2048 canvas — must match plugins/app_2048/app_2048_view.h.
constexpr int kCanvasWidth  = 160;
constexpr int kCanvasHeight = 200;

// ─── socket helpers (adapted from gateway_m5_input_test.cpp) ────────────────

uint16_t pick_free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    (void)::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    (void)::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    ::close(fd);
    return ntohs(addr.sin_port);
}

bool send_all(int sock, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(sock, data + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) <<  8) |
            static_cast<uint32_t>(p[3]);
}

std::vector<uint8_t> make_packet(CmdType cmd, uint32_t session_id,
                                 const std::vector<uint8_t>& body = {}) {
    Header hdr{};
    hdr.magic       = PROTOCOL_MAGIC;
    hdr.session_id  = session_id;
    hdr.cmd_type    = cmd;
    hdr.body_length = static_cast<uint32_t>(body.size());
    std::vector<uint8_t> out(HEADER_SIZE + body.size());
    serialize_header(hdr, out.data());
    if (!body.empty())
        std::memcpy(out.data() + HEADER_SIZE, body.data(), body.size());
    return out;
}

std::vector<uint8_t> make_spawn_app(const std::string& name) {
    const uint32_t len = static_cast<uint32_t>(name.size());
    std::vector<uint8_t> body(4 + name.size());
    body[0] = static_cast<uint8_t>((len >> 24) & 0xFF);
    body[1] = static_cast<uint8_t>((len >> 16) & 0xFF);
    body[2] = static_cast<uint8_t>((len >>  8) & 0xFF);
    body[3] = static_cast<uint8_t>( len        & 0xFF);
    std::memcpy(body.data() + 4, name.data(), name.size());
    return make_packet(CmdType::SPAWN_APP, 0, body);
}

int connect_to(uint16_t port) {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    timeval tv{2, 0};   // 2-second recv timeout: should never fire on loopback.
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    ::inet_aton("127.0.0.1", &addr.sin_addr);
    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(sock);
        return -1;
    }
    // Disable Nagle: we want every send() to hit the wire immediately so the
    // measured latency reflects protocol/processing time, not coalescing.
    int one = 1;
    ::setsockopt(sock, IPPROTO_TCP, /*TCP_NODELAY*/ 1, &one, sizeof(one));
    return sock;
}

class FakeClient {
public:
    enum class RecvResult { Ok, Timeout, Error };

    explicit FakeClient(uint16_t port) : sock_(connect_to(port)) {}
    ~FakeClient() { if (sock_ >= 0) ::close(sock_); }

    int  sock() const { return sock_; }
    bool ok()   const { return sock_ >= 0; }

    bool send_packet(const std::vector<uint8_t>& packet) {
        return send_all(sock_, packet.data(), packet.size());
    }

    // Adjusts the socket's recv timeout.  Used to switch between a generous
    // window for startup (Gateway fork+exec can take ~1s) and a tight window
    // for the bench loop (so a no-op move on app_2048 fails fast).
    void set_recv_timeout_ms(int ms) {
        timeval tv;
        tv.tv_sec  = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        ::setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    // Reads from the socket until one complete Packet is available.
    // Returns Timeout when SO_RCVTIMEO fires before any bytes arrive (and the
    // parser has no pending data), or Error on EOF / hard error.
    RecvResult recv_one_packet(Packet* out) {
        uint8_t buf[8192];
        size_t total_bytes_received = 0;
        int recv_calls = 0;
        const long long t_start = perf_now_us();
        while (true) {
            if (parser_.next_packet(*out) == ParseResult::OK) {
                PERF_LOG("[perf] +%lld us  CLIENT recv_one_packet OK "
                         "cmd=%u body=%u  bytes_total=%zu recv_calls=%d "
                         "elapsed=%lld us\n",
                         perf_now_us(), static_cast<unsigned>(out->header.cmd_type),
                         static_cast<unsigned>(out->header.body_length),
                         total_bytes_received, recv_calls,
                         perf_now_us() - t_start);
                return RecvResult::Ok;
            }
            ssize_t n = ::recv(sock_, buf, sizeof(buf), 0);
            ++recv_calls;
            if (n > 0) {
                PERF_LOG("[perf] +%lld us  CLIENT recv n=%zd (cumul=%zu)\n",
                         perf_now_us(), n, total_bytes_received + n);
                total_bytes_received += static_cast<size_t>(n);
                parser_.feed(buf, static_cast<size_t>(n));
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return RecvResult::Timeout;
            return RecvResult::Error;
        }
    }

private:
    int           sock_;
    MessageParser parser_;
};

// ─── stats ────────────────────────────────────────────────────────────────

struct Stats {
    long long avg_ns;
    long long min_ns;
    long long max_ns;
    long long p95_ns;
    long long p50_ns;
};

Stats summarize(std::vector<long long> samples) {
    std::sort(samples.begin(), samples.end());
    long long sum = 0;
    for (long long v : samples) sum += v;
    Stats s{};
    const size_t n = samples.size();
    s.avg_ns = sum / static_cast<long long>(n);
    s.min_ns = samples.front();
    s.max_ns = samples.back();
    s.p50_ns = samples[n * 50 / 100];
    s.p95_ns = samples[n * 95 / 100];
    return s;
}

// ─── argument parsing ─────────────────────────────────────────────────────

struct Args {
    std::string apphost_bin  = "bazel-bin/src/apphost/apphost_bin";
    std::string plugin_path  = "bazel-bin/plugins/app_2048/libapp_2048.so";
    std::string log_path     = "/tmp/fake_e2e_client_bench.csv";
    int         iters        = kDefaultBenchIters;
    int         warmup       = kDefaultWarmupIters;
};

bool parse_kv(const char* arg, const char* key, std::string* dst) {
    const size_t klen = std::strlen(key);
    if (std::strncmp(arg, key, klen) == 0 && arg[klen] == '=') {
        *dst = arg + klen + 1;
        return true;
    }
    return false;
}

bool parse_int(const char* arg, const char* key, int* dst) {
    std::string s;
    if (!parse_kv(arg, key, &s)) return false;
    *dst = std::atoi(s.c_str());
    return true;
}

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        if (parse_kv(argv[i], "--apphost-bin", &a.apphost_bin)) continue;
        if (parse_kv(argv[i], "--app-2048",    &a.plugin_path)) continue;
        if (parse_kv(argv[i], "--log",         &a.log_path))    continue;
        if (parse_int(argv[i], "--iters",  &a.iters))           continue;
        if (parse_int(argv[i], "--warmup", &a.warmup))          continue;
        std::fprintf(stderr, "[bench] unknown arg: %s\n", argv[i]);
        std::exit(2);
    }
    return a;
}

// ─── per-iteration helpers ────────────────────────────────────────────────

// Result of a single send-key attempt.
enum class KeyResult {
    Ok,        // PIXEL_DATA returned within the recv timeout; elapsed_ns set.
    NoOp,      // Move was silently dropped by the plugin (e.g. app_2048's
               // no-op direction); caller should retry with a different key.
    Fatal,     // Hard socket error; caller should abort.
};

// Sends one keyboard INPUT_EVENT and waits for the next PIXEL_DATA with a
// strictly higher frame_seq.  On Ok, updates *last_seq and writes elapsed
// nanoseconds to *elapsed_ns_out.
KeyResult send_key_and_time(FakeClient& client, uint32_t session_id,
                            uint16_t key_code, uint32_t* last_seq,
                            long long* elapsed_ns_out) {
    auto body   = pack_keyboard_input_event(InputEventType::KEY_DOWN,
                                            key_code, /*ts*/ 0);
    auto packet = make_packet(CmdType::INPUT_EVENT, session_id, body);

    const auto t0 = std::chrono::steady_clock::now();
    if (!client.send_packet(packet)) return KeyResult::Fatal;

    while (true) {
        Packet pkt;
        const auto r = client.recv_one_packet(&pkt);
        if (r == FakeClient::RecvResult::Timeout) return KeyResult::NoOp;
        if (r == FakeClient::RecvResult::Error)   return KeyResult::Fatal;
        if (pkt.header.cmd_type != CmdType::PIXEL_DATA) continue;  // ignore HEARTBEAT etc.
        if (pkt.body.size() < 24) return KeyResult::Fatal;
        const uint32_t seq = read_be32(pkt.body.data());
        if (seq <= *last_seq) continue;  // stale frame queued before send.
        const auto t1 = std::chrono::steady_clock::now();
        *last_seq      = seq;
        *elapsed_ns_out = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        return KeyResult::Ok;
    }
}

// Sends one INPUT_EVENT untimed and drains its PIXEL_DATA.  Used to bring
// the app_2048 board to a known fresh state between measurement iters.  The
// helper aborts on Fatal but treats NoOp as caller's responsibility.
bool drain_one_key(FakeClient& client, uint32_t session_id,
                   uint16_t key_code, uint32_t* last_seq) {
    long long ignored = 0;
    const auto r = send_key_and_time(client, session_id, key_code,
                                      last_seq, &ignored);
    return r == KeyResult::Ok;
}

// Drives the per-case warmup + measurement loop.
//
// `keys` is the rotation of key codes whose round-trip latency is timed.
//
// `reset_before_each_iter` controls whether an untimed F2 (RESET) is sent
// before every timed key press.  Arrow cases must enable this because
// app_2048 silently swallows a move that doesn't shift any tile (e.g. LEFT
// on a board whose two random tiles already sit in column 0).  Even with a
// fresh board, some directions remain no-ops with ~1/4 probability, so the
// arrow-case loop retries with successive RESETs until the chosen direction
// actually shifts at least one tile.  The RESET case itself does not need
// this — every F2 triggers a full redraw.
constexpr int kMaxResetRetries = 8;

Stats bench_case(FakeClient& client, uint32_t session_id, uint32_t& last_seq,
                 const char* label, const std::vector<uint16_t>& keys,
                 bool reset_before_each_iter,
                 int warmup, int iters, FILE* csv) {
    auto one_iter = [&](int i, bool record) -> long long {
        const uint16_t key = keys[i % keys.size()];
        long long ns = 0;

        for (int attempt = 0; attempt < kMaxResetRetries; ++attempt) {
            if (reset_before_each_iter) {
                if (!drain_one_key(client, session_id, kKeyF2, &last_seq)) {
                    std::fprintf(stderr,
                                 "[bench] FATAL: pre-reset failed "
                                 "(case=%s, iter=%d, record=%d, last_seq=%u)\n",
                                 label, i, static_cast<int>(record), last_seq);
                    std::exit(1);
                }
            }
            const auto r = send_key_and_time(client, session_id, key,
                                              &last_seq, &ns);
            if (r == KeyResult::Ok)    break;
            if (r == KeyResult::Fatal) {
                std::fprintf(stderr,
                             "[bench] FATAL: socket error during timed send "
                             "(case=%s, iter=%d, record=%d, key=0x%02X)\n",
                             label, i, static_cast<int>(record), key);
                std::exit(1);
            }
            // NoOp: same direction didn't shift any tile against this fresh
            // board.  Retry with another RESET (which re-randomises tile
            // positions) — keep the same `key` to honour the rotation.
            if (!reset_before_each_iter) {
                std::fprintf(stderr,
                             "[bench] FATAL: NoOp from a case that does not "
                             "reset between iters (case=%s, iter=%d, key=0x%02X) — "
                             "this should be impossible\n",
                             label, i, key);
                std::exit(1);
            }
        }

        if (ns == 0) {
            std::fprintf(stderr,
                         "[bench] FATAL: exhausted %d RESET retries "
                         "(case=%s, iter=%d, key=0x%02X) — board may be stuck "
                         "in a degenerate state\n",
                         kMaxResetRetries, label, i, key);
            std::exit(1);
        }

        if (record && csv) {
            std::fprintf(csv, "fake_e2e,%s,%d,0x%02X,%lld\n",
                         label, i, key, ns);
        }
        return ns;
    };

    for (int i = 0; i < warmup; ++i) (void)one_iter(i, /*record=*/false);

    std::vector<long long> samples;
    samples.reserve(iters);
    for (int i = 0; i < iters; ++i) samples.push_back(one_iter(i, /*record=*/true));
    return summarize(std::move(samples));
}

void print_summary(const char* label, const Stats& s, int iters) {
    std::printf(
        "[bench] e2e_latency  %-20s  iters=%d  "
        "avg=%lld ns (%.1f us)  min=%lld  p50=%lld  p95=%lld  max=%lld\n",
        label, iters, s.avg_ns, s.avg_ns / 1000.0,
        s.min_ns, s.p50_ns, s.p95_ns, s.max_ns);
}

// ─── Gateway boot ─────────────────────────────────────────────────────────

Gateway::Config make_gateway_config(const Args& a) {
    Gateway::Config cfg{};
    cfg.public_port            = pick_free_port();
    cfg.apphost_internal_port  = pick_free_port();
    cfg.apphost_bind_host      = "127.0.0.1";
    cfg.apphost_bin            = a.apphost_bin;
    cfg.auto_spawn_apphost     = true;
    cfg.spawn_timeout_ms       = 2000;
    cfg.monitor_interval_ms    = 50;
    cfg.client_timeout_ms      = 30000;
    cfg.apphost_timeout_ms     = 30000;
    cfg.app_allowlist["app_2048"] =
        Gateway::AppEntry{a.plugin_path, kCanvasWidth, kCanvasHeight};
    return cfg;
}

// Drains SESSION_ACK plus enough PIXEL_DATA frames that the plugin has
// reached steady state.  Returns the (session_id, last_frame_seq) pair.
struct Session { uint32_t id; uint32_t last_seq; };
Session drain_startup(FakeClient& client) {
    Session out{0, 0};
    int pixel_seen = 0;
    for (int i = 0; i < 20 && (out.id == 0 || pixel_seen < 1); ++i) {
        Packet pkt;
        const auto r = client.recv_one_packet(&pkt);
        if (r != FakeClient::RecvResult::Ok) {
            std::fprintf(stderr,
                         "[bench] FATAL: startup recv failed at i=%d (r=%d)\n",
                         i, static_cast<int>(r));
            std::exit(1);
        }
        if (pkt.header.cmd_type == CmdType::SESSION_ACK) {
            out.id = pkt.header.session_id;
            continue;
        }
        if (pkt.header.cmd_type == CmdType::PIXEL_DATA) {
            if (pkt.body.size() >= 4)
                out.last_seq = read_be32(pkt.body.data());
            ++pixel_seen;
            continue;
        }
        // ignore HEARTBEAT etc.
    }
    if (out.id == 0) {
        std::fprintf(stderr, "[bench] FATAL: no SESSION_ACK during startup\n");
        std::exit(1);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);

    FILE* csv = std::fopen(a.log_path.c_str(), "w");
    if (!csv) {
        std::fprintf(stderr,
                     "[bench] WARN: cannot open %s for writing; "
                     "per-iteration log will be dropped\n", a.log_path.c_str());
    } else {
        std::fprintf(csv, "test,case,iter,key,ns\n");
    }

    std::time_t now = std::time(nullptr);
    char ts[64];
    std::strftime(ts, sizeof(ts), "%F %T", std::localtime(&now));
    std::printf("[bench] ===== fake_e2e_client benchmark @ %s =====\n", ts);
    std::printf("[bench] apphost_bin = %s\n", a.apphost_bin.c_str());
    std::printf("[bench] app_2048    = %s\n", a.plugin_path.c_str());
    std::printf("[bench] warmup=%d  iters=%d  log=%s\n",
                a.warmup, a.iters, csv ? a.log_path.c_str() : "<disabled>");
    std::printf("[bench] scope: INPUT_EVENT → PIXEL_DATA round-trip over "
                "loopback (no cross-host RTT, no GUI render)\n");

    // 1. Start the real Gateway with app_2048 in its allowlist.
    Gateway::Config cfg = make_gateway_config(a);
    Gateway gateway;
    if (!gateway.start(cfg)) {
        std::fprintf(stderr,
                     "[bench] FATAL: Gateway::start() failed.  "
                     "Check --apphost-bin and --app-2048 paths.\n");
        return 1;
    }

    // 2. Connect a fake client and complete startup handshake.
    FakeClient client(cfg.public_port);
    if (!client.ok()) {
        std::fprintf(stderr, "[bench] FATAL: connect to Gateway failed\n");
        gateway.stop();
        return 1;
    }
    if (!client.send_packet(make_spawn_app("app_2048"))) {
        std::fprintf(stderr, "[bench] FATAL: SPAWN_APP send failed\n");
        gateway.stop();
        return 1;
    }
    Session session = drain_startup(client);
    std::printf("[bench] session_id=%u  startup_last_seq=%u — ready\n",
                session.id, session.last_seq);

    // After startup, switch to a tight recv timeout so a no-op move on the
    // app_2048 board fails fast (≈100 ms) instead of blocking on the
    // generous 2-second startup budget.  On loopback the legitimate response
    // arrives in milliseconds, so 100 ms leaves wide headroom for jitter.
    client.set_recv_timeout_ms(100);

    // 3. Run benches.  Each call mutates session.last_seq as it observes
    //    frames, so the two cases stay synchronised on the running counter.
    const std::vector<uint16_t> arrow_keys = {kKeyLeft, kKeyUp, kKeyRight, kKeyDown};
    Stats arrow_stats = bench_case(client, session.id, session.last_seq,
                                    "app_2048_arrow", arrow_keys,
                                    /*reset_before_each_iter=*/true,
                                    a.warmup, a.iters, csv);
    print_summary("app_2048_arrow", arrow_stats, a.iters);

    const std::vector<uint16_t> reset_keys = {kKeyF2};
    Stats reset_stats = bench_case(client, session.id, session.last_seq,
                                    "app_2048_reset", reset_keys,
                                    /*reset_before_each_iter=*/false,
                                    a.warmup, a.iters, csv);
    print_summary("app_2048_reset", reset_stats, a.iters);

    // 4. Teardown.
    client.send_packet(make_packet(CmdType::CLOSE_SESSION, session.id));
    gateway.stop();
    if (csv) std::fclose(csv);
    std::printf("[bench] done\n");
    return 0;
}
