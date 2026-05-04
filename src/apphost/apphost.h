#pragma once

#include <cstdint>
#include <memory>
#include <string>

// AppHost — outbound TCP client to Gateway that hosts a single business plugin.
//
// Lifecycle (M4):
//   1. connect() to Gateway internal port (with bounded retry)
//   2. send APPHOST_READY(header.session_id = cfg.session_id)
//   3. start a reader thread:
//        - HEARTBEAT  → echo back
//        - CLOSE_SESSION → ui_host_impl.request_quit()
//        - INPUT_EVENT  → ignored (M5 will inject via PostWork)
//   4. dlopen plugin .so, dlsym _SA_UIMain
//   5. install pixel sink: each finished PIXEL_DATA Body gets prefixed with
//      a 13-byte protocol Header and sent over the socket
//   6. _SA_UIMain(lcHost, args) — blocks on the plugin's GuiHelper::Loop()
//   7. on plugin return: close socket, join reader thread, dlclose plugin
//
// Single connection, no pool: AppHost talks to exactly one Gateway in M4.
class AppHost {
public:
    struct Config {
        std::string gateway_host;   // dotted IPv4 (e.g. "127.0.0.1")
        uint16_t    gateway_port = 0;
        uint32_t    session_id   = 0;
        std::string plugin_path;    // .so to dlopen
        int         width        = 800;
        int         height       = 600;
    };

    AppHost();
    ~AppHost();

    AppHost(const AppHost&)            = delete;
    AppHost& operator=(const AppHost&) = delete;

    // Runs the full lifecycle.  Returns:
    //   >=0  plugin exit code (0 on graceful CLOSE_SESSION)
    //   <0   setup failure (connect / dlopen / dlsym / write APPHOST_READY)
    int run(const Config& cfg);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
