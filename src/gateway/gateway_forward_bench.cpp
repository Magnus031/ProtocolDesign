// Gateway data-plane forwarding micro-benchmark.
//
// Measures the protocol-layer overhead Gateway incurs per Packet on the
// forward path, i.e. the work between "complete Packet parsed from input
// connection" and "byte chunks enqueued, ready for IoPool to drain":
//
//   serialize_packet(pkt)   →  rebuild 13 B Header + memcpy Body into one
//                              contiguous buffer for the outbound side.
//   push_chunked(queue, …)  →  split that buffer into ≤ kIoPoolMaxChunk
//                              pieces because GKC IoPool's send buffer is
//                              capped at 4000 B per BeginInput call.
//
// Out of scope (cannot benchmark in isolation):
//   • SessionManager lookup — O(1) hashmap; shared with other paths.
//   • IoPool BeginInput     — actual network I/O, not Gateway's CPU cost.
//   • MessageParser         — already covered by message_parser_bench.
//
// IMPORTANT — kIoPoolMaxChunk, push_chunked() and serialize_packet() below
// are hand-mirrored copies of the file-static helpers in
// src/gateway/gateway.cpp.  Keep them line-for-line aligned with production;
// any change in gateway.cpp must be reflected here, otherwise the bench
// stops measuring what Gateway actually does.
//
// Build:  bazel build -c opt //src/gateway:gateway_forward_bench
// Run:    bazel-bin/src/gateway/gateway_forward_bench --log=<csv>
// Logs:   Per-iteration ns is written to --log (default
//         /tmp/gateway_forward_bench.csv).  Summary is printed to stdout.

#include "src/protocol/protocol.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <queue>
#include <vector>

namespace {

constexpr int kWarmupIters = 50;
constexpr int kBenchIters  = 200;

// MIRRORED from gateway.cpp ─ kIoPoolMaxChunk
// IoPool's BeginInput crashes if uLen > 4000, so all outbound Packets larger
// than 4000 bytes must be split first.  TCP reassembles on the peer side, so
// chunking is invisible to MessageParser.
constexpr size_t kIoPoolMaxChunk = 4000;

// MIRRORED from gateway.cpp ─ push_chunked
// Splits an outbound buffer into ≤ kIoPoolMaxChunk pieces and pushes each one
// onto the connection's send queue.
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

// MIRRORED from gateway.cpp ─ serialize_packet
// Allocates HEADER_SIZE + body bytes, writes the 13-byte big-endian header
// in front, and memcpy's the body in.
std::vector<uint8_t> serialize_packet(const Packet& pkt) {
    std::vector<uint8_t> out(HEADER_SIZE + pkt.body.size());
    Header hdr = pkt.header;
    hdr.body_length = static_cast<uint32_t>(pkt.body.size());
    serialize_header(hdr, out.data());
    if (!pkt.body.empty())
        std::memcpy(out.data() + HEADER_SIZE, pkt.body.data(), pkt.body.size());
    return out;
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

struct Case {
    const char* label;
    CmdType     cmd;
    uint32_t    body_len;
};

// Returns ceil(packet_size / chunk_size) — the chunk count produced by
// push_chunked for a given total wire size.
size_t expected_chunks(size_t body_len) {
    const size_t packet = HEADER_SIZE + body_len;
    return (packet + kIoPoolMaxChunk - 1) / kIoPoolMaxChunk;
}

Packet build_packet(const Case& c) {
    Packet pkt{};
    pkt.header.magic       = PROTOCOL_MAGIC;
    pkt.header.session_id  = 1;
    pkt.header.cmd_type    = c.cmd;
    pkt.header.body_length = c.body_len;
    pkt.header.reserved    = 0;
    pkt.body.assign(c.body_len, 0xAB);   // Dummy body — content does not affect timing.
    return pkt;
}

Stats bench_one(const Case& c, FILE* csv, size_t& observed_chunks_out) {
    const Packet pkt = build_packet(c);
    std::queue<std::vector<uint8_t>> q;

    // Warm up: ensure caches and code path are hot.
    for (int i = 0; i < kWarmupIters; ++i) {
        push_chunked(q, serialize_packet(pkt));
        while (!q.empty()) q.pop();
    }

    std::vector<long long> samples;
    samples.reserve(kBenchIters);

    size_t observed_chunks = 0;

    for (int i = 0; i < kBenchIters; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        push_chunked(q, serialize_packet(pkt));
        const auto t1 = std::chrono::steady_clock::now();

        const size_t chunks = q.size();
        if (i == 0) observed_chunks = chunks;

        // Drain outside the timed window so the next iter starts with an
        // empty queue (identical baseline across iterations).
        while (!q.empty()) q.pop();

        const long long ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        samples.push_back(ns);

        if (csv) {
            std::fprintf(csv, "gateway_forward,%s,%u,%zu,%d,%lld\n",
                         c.label, c.body_len, chunks, i, ns);
        }
    }

    observed_chunks_out = observed_chunks;
    return summarize(std::move(samples));
}

void print_summary(const Case& c, const Stats& s, size_t chunks) {
    const size_t expected = expected_chunks(c.body_len);
    std::printf(
        "[bench] gateway_forward  %-18s  body=%-7u B  chunks=%zu (expected %zu)  "
        "iters=%d  avg=%lld ns  min=%lld  p50=%lld  p95=%lld  max=%lld\n",
        c.label, c.body_len, chunks, expected, kBenchIters,
        s.avg_ns, s.min_ns, s.p50_ns, s.p95_ns, s.max_ns);
    if (chunks != expected) {
        std::fprintf(stderr, "[bench] WARN: chunk count mismatch for %s "
                             "(got %zu, expected %zu)\n",
                     c.label, chunks, expected);
    }
}

const char* parse_log_path(int argc, char** argv, const char* def) {
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--log=", 6) == 0) return argv[i] + 6;
    }
    return def;
}

}  // namespace

int main(int argc, char** argv) {
    const char* log_path = parse_log_path(argc, argv, "/tmp/gateway_forward_bench.csv");

    FILE* csv = std::fopen(log_path, "w");
    if (!csv) {
        std::fprintf(stderr, "[bench] WARN: cannot open %s for writing; "
                             "per-iteration log will be dropped\n", log_path);
    } else {
        std::fprintf(csv, "test,case,body_len,chunks,iter,ns\n");
    }

    std::time_t now = std::time(nullptr);
    char ts[64];
    std::strftime(ts, sizeof(ts), "%F %T", std::localtime(&now));
    std::printf("[bench] ===== Gateway forward benchmark @ %s =====\n", ts);
    std::printf("[bench] warmup=%d  iters=%d  log=%s\n",
                kWarmupIters, kBenchIters, csv ? log_path : "<disabled>");
    std::printf("[bench] measuring: serialize_packet() + push_chunked() per Packet\n");
    std::printf("[bench] out of scope: SessionManager lookup, IoPool BeginInput (network I/O)\n");

    // Cases chosen to match Chapter 6 表 "Gateway 转发开销" 三行：
    //   input_event    → keyboard INPUT_EVENT body 11 B → Packet 24 B → 1 chunk
    //   demo_app_full  → PIXEL_DATA body 2072 B → Packet 2085 B → 1 chunk
    //   app_2048_full  → PIXEL_DATA body 128024 B → Packet 128037 B → 33 chunks
    const Case cases[] = {
        {"input_event",   CmdType::INPUT_EVENT,    11u},
        {"demo_app_full", CmdType::PIXEL_DATA,   2072u},
        {"app_2048_full", CmdType::PIXEL_DATA, 128024u},
    };
    for (const auto& c : cases) {
        size_t chunks = 0;
        Stats s = bench_one(c, csv, chunks);
        print_summary(c, s, chunks);
    }

    if (csv) std::fclose(csv);
    std::printf("[bench] done\n");
    return 0;
}
