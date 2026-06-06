// MessageParser micro-benchmark.
//
// Measures the cost of feeding a complete Packet into MessageParser and
// extracting it via next_packet(). Targets the three Body sizes referenced in
// the thesis chapter 6: 280 B, 2072 B, 128024 B.
//
// Build:   bazel build -c opt //src/protocol:message_parser_bench
// Run:     bazel-bin/src/protocol/message_parser_bench
// Logs:    Per-iteration CSV is written to --log=<path> (default:
//          /tmp/message_parser_bench.csv). Summary is printed to stdout.

#include "src/protocol/message_parser.h"
#include "src/protocol/protocol.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

namespace {

constexpr int kWarmupIters = 50;
constexpr int kBenchIters  = 200;

// Builds a serialized PIXEL_DATA Packet of total wire size (HEADER_SIZE + body_len).
// The Body content is deterministic dummy bytes — parser only does length and magic
// checks, so the actual byte values do not affect parse cost.
std::vector<uint8_t> build_packet_bytes(uint32_t body_len) {
    std::vector<uint8_t> wire(HEADER_SIZE + body_len);

    Header h{};
    h.magic       = PROTOCOL_MAGIC;
    h.session_id  = 0x12345678;
    h.cmd_type    = CmdType::PIXEL_DATA;
    h.body_length = body_len;
    h.reserved    = 0;
    serialize_header(h, wire.data());

    for (uint32_t i = 0; i < body_len; ++i) {
        wire[HEADER_SIZE + i] = static_cast<uint8_t>(i & 0xFF);
    }
    return wire;
}

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
    s.avg_ns = sum / static_cast<long long>(samples.size());
    s.min_ns = samples.front();
    s.max_ns = samples.back();
    s.p50_ns = samples[samples.size() * 50 / 100];
    s.p95_ns = samples[samples.size() * 95 / 100];
    return s;
}

// Run kBenchIters parses for one body size. Writes per-iteration ns to csv (if non-null).
Stats bench_one(uint32_t body_len, FILE* csv) {
    const auto wire = build_packet_bytes(body_len);

    // Warm up: ensure caches and pages are touched, parser code path is hot.
    for (int i = 0; i < kWarmupIters; ++i) {
        MessageParser p;
        p.feed(wire.data(), wire.size());
        Packet out;
        (void)p.next_packet(out);
    }

    std::vector<long long> samples;
    samples.reserve(kBenchIters);

    for (int i = 0; i < kBenchIters; ++i) {
        MessageParser p;  // Fresh parser each iter — measures full feed+parse path.

        const auto t0 = std::chrono::steady_clock::now();
        p.feed(wire.data(), wire.size());
        Packet out;
        const ParseResult r = p.next_packet(out);
        const auto t1 = std::chrono::steady_clock::now();

        if (r != ParseResult::OK) {
            std::fprintf(stderr, "[bench] FATAL: parse failed at iter=%d body=%u\n",
                         i, body_len);
            std::exit(1);
        }
        // Touch out.body to keep the compiler from eliding the copy.
        asm volatile("" : : "r"(out.body.data()) : "memory");

        const long long ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        samples.push_back(ns);

        if (csv) {
            std::fprintf(csv, "MessageParser,%u,%d,%lld\n", body_len, i, ns);
        }
    }

    return summarize(std::move(samples));
}

void print_summary(uint32_t body_len, const Stats& s) {
    std::printf(
        "[bench] MessageParser  body=%-7u B  iters=%d  "
        "avg=%lld ns  min=%lld  p50=%lld  p95=%lld  max=%lld\n",
        body_len, kBenchIters, s.avg_ns, s.min_ns, s.p50_ns, s.p95_ns, s.max_ns);
}

const char* parse_log_path(int argc, char** argv, const char* def) {
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--log=", 6) == 0) return argv[i] + 6;
    }
    return def;
}

}  // namespace

int main(int argc, char** argv) {
    const char* log_path = parse_log_path(argc, argv, "/tmp/message_parser_bench.csv");

    FILE* csv = std::fopen(log_path, "w");
    if (!csv) {
        std::fprintf(stderr, "[bench] WARN: cannot open %s for writing; "
                             "per-iteration log will be dropped\n", log_path);
    } else {
        std::fprintf(csv, "test,body_len,iter,ns\n");
    }

    // Stamp the log header to stdout so the run is self-documenting.
    std::time_t now = std::time(nullptr);
    char ts[64];
    std::strftime(ts, sizeof(ts), "%F %T", std::localtime(&now));
    std::printf("[bench] ===== MessageParser benchmark @ %s =====\n", ts);
    std::printf("[bench] warmup=%d  iters=%d  log=%s\n",
                kWarmupIters, kBenchIters, csv ? log_path : "<disabled>");

    // Body sizes chosen to match Chapter 6 — chapter 6 表 6.x：
    //   280 B   → demo_app 8x8 dirty rect Body
    //   2072 B  → demo_app 32x16 full window Body
    //   128024 B→ app_2048 160x200 full canvas Body
    const uint32_t sizes[] = {280, 2072, 128024};
    for (uint32_t sz : sizes) {
        Stats s = bench_one(sz, csv);
        print_summary(sz, s);
    }

    if (csv) std::fclose(csv);
    std::printf("[bench] done\n");
    return 0;
}
