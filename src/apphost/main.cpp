// AppHost main: parses --gateway-host/--gateway-port/--session-id/
// --plugin/--width/--height, then runs AppHost::run() until the plugin
// returns.

#include "src/apphost/apphost.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

const char* find_arg(int argc, char* argv[], const char* name) {
    const size_t n = std::strlen(name);
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], name, n) == 0 && argv[i][n] == '=')
            return argv[i] + n + 1;
        if (std::strcmp(argv[i], name) == 0 && i + 1 < argc)
            return argv[i + 1];
    }
    return nullptr;
}

bool parse_u32(const char* s, uint32_t* out) {
    if (s == nullptr) return false;
    char* end = nullptr;
    unsigned long v = std::strtoul(s, &end, 0);
    if (end == s || *end != '\0') return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

bool parse_int(const char* s, int* out) {
    if (s == nullptr) return false;
    char* end = nullptr;
    long v = std::strtol(s, &end, 0);
    if (end == s || *end != '\0') return false;
    *out = static_cast<int>(v);
    return true;
}

void usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s --gateway-host=<ip> --gateway-port=<n> --session-id=<n> "
        "--plugin=<path> [--width=<n>] [--height=<n>]\n", argv0);
}

}  // namespace

int main(int argc, char* argv[]) {
    AppHost::Config cfg;
    const char* host    = find_arg(argc, argv, "--gateway-host");
    const char* port_s  = find_arg(argc, argv, "--gateway-port");
    const char* sess_s  = find_arg(argc, argv, "--session-id");
    const char* plugin  = find_arg(argc, argv, "--plugin");
    const char* width_s = find_arg(argc, argv, "--width");
    const char* height_s= find_arg(argc, argv, "--height");

    if (host == nullptr || port_s == nullptr || sess_s == nullptr ||
        plugin == nullptr) {
        usage(argv[0]);
        return 1;
    }

    uint32_t port = 0;
    if (!parse_u32(port_s, &port) || port == 0 || port > 65535) {
        usage(argv[0]);
        return 1;
    }
    if (!parse_u32(sess_s, &cfg.session_id)) {
        usage(argv[0]);
        return 1;
    }

    cfg.gateway_host = host;
    cfg.gateway_port = static_cast<uint16_t>(port);
    cfg.plugin_path  = plugin;
    if (width_s  != nullptr && !parse_int(width_s,  &cfg.width))  cfg.width  = 800;
    if (height_s != nullptr && !parse_int(height_s, &cfg.height)) cfg.height = 600;

    AppHost host_runner;
    const int rc = host_runner.run(cfg);
    return rc < 0 ? -rc + 10 : rc;   // surface setup failures as nonzero exit
}
