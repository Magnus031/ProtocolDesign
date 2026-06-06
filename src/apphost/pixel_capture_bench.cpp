// pack_pixel_data_rect micro-benchmark.
//
// Measures the cost of packing a full window pixel buffer into a PIXEL_DATA
// Body — the AppHost-side hot path. Targets the three dirty-rectangle sizes
// referenced in the thesis chapter 6:
//   8x8     → demo_app local update
//   32x16   → demo_app full window update
//   160x200 → app_2048 full canvas update
//
// Build:  bazel build -c opt //src/apphost:pixel_capture_bench
// Run:    bazel-bin/src/apphost/pixel_capture_bench --log=<csv>
// Logs:   Per-iteration ns is written to --log (default
//         /tmp/pixel_capture_bench.csv). Summary is printed to stdout.

#include "src/apphost/pixel_capture.h"

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

// Builds a stride_pixels-wide buffer tall enough to hold the requested rect
// at (0,0). The values are deterministic — pack_pixel_data_rect performs
// byte-order conversion only, so actual pixel values do not affect timing.
std::vector<uint32_t> make_buffer(int stride_pixels, int height) {
    std::vector<uint32_t> buf(static_cast<size_t>(stride_pixels) * height);
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<uint32_t>(0x80000000u | (i & 0x00FFFFFFu));
    }
    return buf;
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
    int         stride;
    int         width;
    int         height;
};

Stats bench_one(const Case& c, FILE* csv) {
    auto buf = make_buffer(c.stride, c.height);

    // Warm up the code path and caches.
    for (int i = 0; i < kWarmupIters; ++i) {
        auto out = pack_pixel_data_rect(buf.data(), c.stride,
                                         0, 0, c.width, c.height,
                                         static_cast<uint32_t>(i));
        asm volatile("" : : "r"(out.data()) : "memory");
    }

    std::vector<long long> samples;
    samples.reserve(kBenchIters);

    for (int i = 0; i < kBenchIters; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        auto out = pack_pixel_data_rect(buf.data(), c.stride,
                                         0, 0, c.width, c.height,
                                         static_cast<uint32_t>(i));
        const auto t1 = std::chrono::steady_clock::now();
        asm volatile("" : : "r"(out.data()) : "memory");

        const long long ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        samples.push_back(ns);

        if (csv) {
            std::fprintf(csv, "pack_pixel_data_rect,%s,%dx%d,%d,%lld\n",
                         c.label, c.width, c.height, i, ns);
        }
    }
    return summarize(std::move(samples));
}

void print_summary(const Case& c, const Stats& s) {
    std::printf(
        "[bench] pack_pixel_data_rect  %-12s  rect=%dx%d  iters=%d  "
        "avg=%lld ns  min=%lld  p50=%lld  p95=%lld  max=%lld\n",
        c.label, c.width, c.height, kBenchIters,
        s.avg_ns, s.min_ns, s.p50_ns, s.p95_ns, s.max_ns);
}

const char* parse_log_path(int argc, char** argv, const char* def) {
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--log=", 6) == 0) return argv[i] + 6;
    }
    return def;
}

}  // namespace

int main(int argc, char** argv) {
    const char* log_path = parse_log_path(argc, argv, "/tmp/pixel_capture_bench.csv");

    FILE* csv = std::fopen(log_path, "w");
    if (!csv) {
        std::fprintf(stderr, "[bench] WARN: cannot open %s for writing; "
                             "per-iteration log will be dropped\n", log_path);
    } else {
        std::fprintf(csv, "test,case,rect,iter,ns\n");
    }

    std::time_t now = std::time(nullptr);
    char ts[64];
    std::strftime(ts, sizeof(ts), "%F %T", std::localtime(&now));
    std::printf("[bench] ===== pack_pixel_data_rect benchmark @ %s =====\n", ts);
    std::printf("[bench] warmup=%d  iters=%d  log=%s\n",
                kWarmupIters, kBenchIters, csv ? log_path : "<disabled>");

    // Cases chosen to match Chapter 6 测试场景：
    //   demo_app_small  → 8x8 dirty rect inside a 32x16 demo_app canvas
    //   demo_app_full   → 32x16 full demo_app window
    //   app_2048_full   → 160x200 full app_2048 canvas
    const Case cases[] = {
        {"demo_app_small", /*stride*/ 32,  /*w*/ 8,   /*h*/ 8},
        {"demo_app_full",  /*stride*/ 32,  /*w*/ 32,  /*h*/ 16},
        {"app_2048_full",  /*stride*/ 160, /*w*/ 160, /*h*/ 200},
    };
    for (const auto& c : cases) {
        Stats s = bench_one(c, csv);
        print_summary(c, s);
    }

    if (csv) std::fclose(csv);
    std::printf("[bench] done\n");
    return 0;
}
