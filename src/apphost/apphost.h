#pragma once
#include <cstdint>
#include <memory>

// IoPool-based TCP server for AppHost.
// Listens for client connections, processes protocol packets, and responds
// to SPAWN_APP with a static 16x16 PIXEL_DATA frame.
//
// IoPool is a process-global singleton. Only one AppHost may be started at a
// time per process. Calling start() initialises the pool; stop() disables it.
class AppHost {
public:
    AppHost();
    ~AppHost();

    // Binds to port, initialises IoPool, and starts accepting connections.
    // Returns false if the port cannot be bound or IoPool initialisation fails.
    bool start(uint16_t port);

    // Stops accepting new connections and disables the global IoPool.
    // Blocks until all I/O threads have stopped.
    void stop();

    // Blocks until stop() is called (useful in main()).
    void wait();

    bool is_running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
