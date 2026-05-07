#pragma once

#include <cstdio>
#include <string>

#if defined(_WIN32)
#include <process.h>
#define CLIENT_LOG_GETPID() ::_getpid()
#else
#include <unistd.h>
#define CLIENT_LOG_GETPID() ::getpid()
#endif

// Appends a short diagnostic line to the client-side M6 log.  The client is a
// GUI subsystem binary on Windows, so stderr is normally invisible during
// manual testing.  The log path is unique per process (`client_m6_pid<PID>.log`)
// so multiple client.exe instances launched from the same working directory do
// not clobber each other's output.
inline void client_log_line(const std::string& line) {
    static const std::string log_path = [] {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "client_m6_pid%d.log",
                      CLIENT_LOG_GETPID());
        return std::string(buf);
    }();
    FILE* f = std::fopen(log_path.c_str(), "ab");
    if (f == nullptr) return;
    std::fwrite(line.data(), 1, line.size(), f);
    std::fwrite("\n", 1, 1, f);
    std::fclose(f);
}
