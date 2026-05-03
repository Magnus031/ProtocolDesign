#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <sys/types.h>

struct AppHostLaunchConfig {
    uint32_t session_id = 0;
    std::string apphost_bin;
    std::string gateway_host;
    uint16_t gateway_port = 0;
    std::string plugin_path;
    int width = 800;
    int height = 600;
};

struct AppHostProcess {
    pid_t pid = -1;
};

struct ExitedProcess {
    pid_t pid = -1;
    int status = 0;
    bool exited = false;
    int exit_code = -1;
    bool signaled = false;
    int signal = 0;
};

class ProcessManager {
public:
    bool spawn_apphost(const AppHostLaunchConfig& cfg, AppHostProcess* out);
    bool terminate(pid_t pid);
    bool kill_force(pid_t pid);
    std::vector<ExitedProcess> reap_exited();
};
