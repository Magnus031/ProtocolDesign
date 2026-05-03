#include "src/gateway/process_manager.h"

#include <csignal>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

std::string to_string_u32(uint32_t v) {
    return std::to_string(static_cast<unsigned long long>(v));
}

std::string to_string_i32(int v) {
    return std::to_string(static_cast<long long>(v));
}

ExitedProcess make_exited(pid_t pid, int status) {
    ExitedProcess out{};
    out.pid = pid;
    out.status = status;
    out.exited = WIFEXITED(status);
    out.exit_code = out.exited ? WEXITSTATUS(status) : -1;
    out.signaled = WIFSIGNALED(status);
    out.signal = out.signaled ? WTERMSIG(status) : 0;
    return out;
}

} // namespace

bool ProcessManager::spawn_apphost(const AppHostLaunchConfig& cfg, AppHostProcess* out) {
    // fork() gives Gateway a child process that can be replaced with the
    // AppHost executable while the parent keeps serving existing sessions.
    pid_t pid = ::fork();
    if (pid < 0)
        return false;

    if (pid == 0) {
        // Pass the Gateway-assigned session and AppHost launch parameters as
        // argv entries. execv() does not invoke a shell, so paths are not
        // interpreted as shell commands.
        std::string session = "--session-id=" + to_string_u32(cfg.session_id);
        std::string host = "--gateway-host=" + cfg.gateway_host;
        std::string port = "--gateway-port=" + to_string_i32(cfg.gateway_port);
        std::string plugin = "--plugin=" + cfg.plugin_path;
        std::string width = "--width=" + to_string_i32(cfg.width);
        std::string height = "--height=" + to_string_i32(cfg.height);

        // The strings above stay alive until execv() replaces this process, so
        // argv may point directly at their internal buffers.
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(cfg.apphost_bin.c_str()));
        argv.push_back(const_cast<char*>(session.c_str()));
        argv.push_back(const_cast<char*>(host.c_str()));
        argv.push_back(const_cast<char*>(port.c_str()));
        argv.push_back(const_cast<char*>(plugin.c_str()));
        argv.push_back(const_cast<char*>(width.c_str()));
        argv.push_back(const_cast<char*>(height.c_str()));
        argv.push_back(nullptr);

        // On success execv() never returns. If it does return, use _exit() so
        // the child does not run parent-process cleanup handlers after fork().
        ::execv(cfg.apphost_bin.c_str(), argv.data());
        ::_exit(127);
    }

    // Reaching this point only means fork() succeeded. The monitor loop later
    // observes AppHost readiness or process exit.
    if (out)
        out->pid = pid;
    return true;
}

bool ProcessManager::terminate(pid_t pid) {
    return pid > 0 && ::kill(pid, SIGTERM) == 0;
}

bool ProcessManager::kill_force(pid_t pid) {
    return pid > 0 && ::kill(pid, SIGKILL) == 0;
}

std::vector<ExitedProcess> ProcessManager::reap_exited() {
    std::vector<ExitedProcess> out;
    while (true) {
        int status = 0;
        // waitpid(-1, ...) checks any child process owned by Gateway. WNOHANG
        // makes the call non-blocking so the monitor loop can poll for AppHost
        // exits without stalling session timeout checks.
        pid_t pid = ::waitpid(-1, &status, WNOHANG);
        if (pid > 0) {
            // Collect all children that have already exited; otherwise a burst
            // of AppHost exits could leave zombies until the next monitor pass.
            out.push_back(make_exited(pid, status));
            continue;
        }
        break;
    }
    return out;
}
