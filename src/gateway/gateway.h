#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

// IoPool-based Gateway for M3a.
//
// Gateway listens on two TCP ports:
//   - public_port: externally exposed Client-side connections
//   - apphost_internal_port: internal AppHost-side connections
//
// It parses protocol frames with MessageParser, maintains SessionID routing,
// and forwards complete packets without parsing business payload bodies.
class Gateway {
public:
    struct AppEntry {
        std::string plugin_path;
        int width = 800;
        int height = 600;
    };

    struct Config {
        uint16_t public_port = 19000;
        std::string apphost_bind_host = "127.0.0.1";
        uint16_t apphost_internal_port = 19001;
        std::string apphost_bin;
        bool auto_spawn_apphost = false;
        int spawn_timeout_ms = 5000;
        int client_timeout_ms = 90000;
        int apphost_timeout_ms = 90000;
        int monitor_interval_ms = 1000;
        std::unordered_map<std::string, AppEntry> app_allowlist;
    };

    Gateway();
    ~Gateway();

    bool start(uint16_t public_port, uint16_t apphost_internal_port);
    bool start(const Config& config);
    void stop();
    void wait();
    bool is_running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
