#include "src/gateway/gateway.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static Gateway* g_gateway = nullptr;

static void on_signal(int) {
    if (g_gateway)
        g_gateway->stop();
}

static uint16_t parse_port_arg(const char* value) {
    char* end = nullptr;
    long v = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || v <= 0 || v >= 65536)
        return 0;
    return static_cast<uint16_t>(v);
}

int main(int argc, char* argv[]) {
    Gateway::Config config{};

    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--public-port=", 14) == 0) {
            config.public_port = parse_port_arg(argv[i] + 14);
        } else if (std::strcmp(argv[i], "--public-port") == 0 && i + 1 < argc) {
            config.public_port = parse_port_arg(argv[++i]);
        } else if (std::strncmp(argv[i], "--apphost-internal-port=", 24) == 0) {
            config.apphost_internal_port = parse_port_arg(argv[i] + 24);
        } else if (std::strcmp(argv[i], "--apphost-internal-port") == 0 && i + 1 < argc) {
            config.apphost_internal_port = parse_port_arg(argv[++i]);
        } else if (std::strncmp(argv[i], "--apphost-bin=", 14) == 0) {
            config.apphost_bin = argv[i] + 14;
            config.auto_spawn_apphost = true;
        } else if (std::strcmp(argv[i], "--apphost-bin") == 0 && i + 1 < argc) {
            config.apphost_bin = argv[++i];
            config.auto_spawn_apphost = true;
        } else if (std::strncmp(argv[i], "--app=", 6) == 0) {
            std::string spec = argv[i] + 6;
            const size_t eq = spec.find('=');
            if (eq == std::string::npos || eq == 0 || eq + 1 >= spec.size()) {
                std::fprintf(stderr, "Invalid --app. Expected --app=name=plugin_path\n");
                return 1;
            }
            // register app_allowlist
            config.app_allowlist[spec.substr(0, eq)] =
                Gateway::AppEntry{spec.substr(eq + 1), 800, 600};
        } else {
            std::fprintf(stderr,
                         "Usage: gateway [--public-port <1-65535>] "
                         "[--apphost-internal-port <1-65535>] "
                         "[--apphost-bin <path>] [--app=name=plugin_path]\n");
            return 1;
        }
    }

    if (config.public_port == 0 || config.apphost_internal_port == 0) {
        std::fprintf(stderr,
                     "Usage: gateway [--public-port <1-65535>] "
                     "[--apphost-internal-port <1-65535>] "
                     "[--apphost-bin <path>] [--app=name=plugin_path]\n");
        return 1;
    }

    if (config.auto_spawn_apphost && config.app_allowlist.empty()) {
        std::fprintf(stderr, "Gateway: --apphost-bin requires at least one --app=name=plugin_path\n");
        return 1;
    }

    Gateway gateway;
    g_gateway = &gateway;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    if (!gateway.start(config)) {
        std::fprintf(stderr, "Gateway: failed to start\n");
        return 1;
    }

    std::fprintf(stdout, "Gateway: public=%u apphost_internal=%u\n",
                 config.public_port, config.apphost_internal_port);
    gateway.wait();
    return 0;
}
