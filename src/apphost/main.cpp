#include "src/apphost/apphost.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>

static AppHost* g_server = nullptr;

static void on_signal(int) {
    if (g_server) g_server->stop();
}

// Parse --port=<n> or --port <n> from argv.  Returns 0 on parse error.
static uint16_t parse_port(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        // --port=19001
        if (std::strncmp(arg, "--port=", 7) == 0) {
            long v = std::strtol(arg + 7, nullptr, 10);
            return (v > 0 && v < 65536) ? static_cast<uint16_t>(v) : 0;
        }
        // --port 19001
        if (std::strcmp(arg, "--port") == 0 && i + 1 < argc) {
            long v = std::strtol(argv[++i], nullptr, 10);
            return (v > 0 && v < 65536) ? static_cast<uint16_t>(v) : 0;
        }
    }
    return 19001;  // default
}

int main(int argc, char* argv[]) {
    uint16_t port = parse_port(argc, argv);
    if (port == 0) {
        std::fprintf(stderr, "Usage: apphost [--port <1-65535>]\n");
        return 1;
    }

    AppHost server;
    g_server = &server;

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    if (!server.start(port)) {
        std::fprintf(stderr, "AppHost: failed to start on port %u\n", port);
        return 1;
    }

    std::fprintf(stdout, "AppHost: listening on port %u\n", port);

    // Block until stop() is called from a signal handler.
    server.wait();

    return 0;
}
