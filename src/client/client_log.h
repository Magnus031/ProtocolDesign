#pragma once

#include <cstdio>
#include <string>

// Appends a short diagnostic line to the client-side M6 log.  The client is a
// GUI subsystem binary on Windows, so stderr is normally invisible during
// manual testing.
inline void client_log_line(const std::string& line) {
    FILE* f = std::fopen("client_m6.log", "ab");
    if (f == nullptr) return;
    std::fwrite(line.data(), 1, line.size(), f);
    std::fwrite("\n", 1, 1, f);
    std::fclose(f);
}
