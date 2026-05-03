#pragma once

#include "base/GkcDef.h"
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using GatewayClock = std::chrono::steady_clock;
using GatewayTimePoint = GatewayClock::time_point;

// Runtime lifecycle of one logical remote-application session.
//
// SPAWNING: Gateway accepted SPAWN_APP and fork/exec'd AppHost, but has not yet
//           received APPHOST_READY from the AppHost connection.
// ACTIVE:   Client and AppHost are both bound; Gateway may route packets.
// CLOSING:  Cleanup is in progress. This prevents duplicate timeout/disconnect
//           paths from cleaning the same session twice.
// DEAD:     Terminal state used by snapshots/default values; removed sessions
//           are no longer stored in SessionTable.
enum class SessionState {
    SPAWNING,
    ACTIVE,
    CLOSING,
    DEAD,
};

// Authoritative state for one Gateway session.
//
// This is deliberately more than a routing entry. M3b needs the same object to
// track route handles, AppHost process lifetime, and heartbeat/timeout data so
// Gateway does not maintain two maps that can drift out of sync.
struct SessionRuntime {
    uint32_t session_id = 0;
    SessionState state = SessionState::SPAWNING;
    // IoPool handle for the Client TCP connection. This is not a pointer to
    // ClientSession; it is the handle passed to BeginInput/DisableHandle.
    uintptr client_conn = 0;
    // IoPool handle for the AppHost TCP connection, filled after APPHOST_READY.
    uintptr apphost_conn = 0;
    // OS child process id for the fork/exec'd AppHost. -1 means none recorded.
    int apphost_pid = -1;
    // Logical app name from SPAWN_APP and resolved plugin path from allowlist.
    std::string app_name;
    std::string plugin_path;
    // steady_clock timestamps used by monitor thread. steady_clock avoids
    // system-clock jumps affecting heartbeat/spawn timeout decisions.
    GatewayTimePoint created_at;
    GatewayTimePoint spawn_deadline;
    GatewayTimePoint last_client_seen;
    GatewayTimePoint last_apphost_seen;
};

// Copy-only view of SessionRuntime for monitor scans.
//
// The monitor thread must not hold a SessionRuntime* after releasing
// SessionTable::mutex_, because an IoPool callback may concurrently remove the
// session. Instead it asks SessionTable for snapshots, evaluates timeouts
// without holding the lock, then re-enters SessionTable by session_id to close
// anything that still needs cleanup.
struct SessionSnapshot {
    uint32_t session_id = 0;
    SessionState state = SessionState::DEAD;
    uintptr client_conn = 0;
    uintptr apphost_conn = 0;
    int apphost_pid = -1;
    GatewayTimePoint spawn_deadline;
    GatewayTimePoint last_client_seen;
    GatewayTimePoint last_apphost_seen;
};

// Maintains Gateway routing state:
//   SessionID <-> (Client IoPool handle, AppHost IoPool handle)
//
// Values are IoPool handle ids, not ConnContext pointers. Gateway uses them
// with BeginInput/EndInput/DisableHandle when forwarding packets.
// In M3b this table is also the authoritative runtime registry for process
// state and heartbeat timestamps.
class SessionTable {
public:
    struct Route {
        uintptr client_conn = 0;
        uintptr apphost_conn = 0;
    };

    // Allocate a new manual-routing session for M3a tests.
    // The AppHost side will be bound later by APPHOST_READY from a TestAppHost.
    uint32_t create_for_client(uintptr client_conn);

    // Allocate a SPAWNING session with process/runtime metadata.
    uint32_t create_spawning_session(uintptr client_conn, const std::string& app_name,
                                     const std::string& plugin_path,
                                     GatewayTimePoint now,
                                     std::chrono::milliseconds spawn_timeout);

    // Attach the fork/exec'd AppHost process id after ProcessManager succeeds.
    // This also populates the reverse pid -> session_id index used by waitpid
    // reap handling.
    bool set_apphost_pid(uint32_t session_id, int pid);

    // Bind an AppHost TCP connection to an existing session and mark it ACTIVE.
    bool bind_apphost(uint32_t session_id, uintptr apphost_conn);

    // Return the route for session_id. Missing sessions return {0, 0}.
    Route find(uint32_t session_id) const;

    uintptr find_apphost(uint32_t session_id) const;
    uintptr find_client(uint32_t session_id) const;
    uint32_t find_by_pid(int pid) const;

    // M3b MVP allows one active/spawning session per Client TCP connection.
    bool has_active_or_spawning_for_client(uintptr client_conn) const;
    // Update heartbeat/activity timestamps. These are called when Gateway
    // receives any valid packet, not only HEARTBEAT.
    bool mark_client_seen(uint32_t session_id, GatewayTimePoint now);
    bool mark_apphost_seen(uint32_t session_id, GatewayTimePoint now);
    // Return stable copies for monitor scans; see SessionSnapshot comment.
    std::vector<SessionSnapshot> snapshot_sessions() const;
    // Atomically change a live session to CLOSING and copy the route/pid needed
    // for cleanup. Returns false if another path already started cleanup.
    bool transition_to_closing(uint32_t session_id, Route* route, int* pid);
    bool get_pid(uint32_t session_id, int* pid) const;

    // Remove a session by id or by one of its connection handles.
    void remove_session(uint32_t session_id);
    uint32_t remove_by_client(uintptr client_conn);
    uint32_t remove_by_apphost(uintptr apphost_conn);

private:
    // Protects every field below and every field inside SessionRuntime. Do not
    // read or mutate SessionRuntime without holding this mutex through a
    // SessionTable method.
    mutable std::mutex mutex_;
    // Monotonic source of Session IDs. 0 is reserved by the protocol for
    // pre-session packets such as SPAWN_APP, so the first real session is 1.
    // IDs are not reused during the Gateway process lifetime.
    uint32_t next_session_id_ = 1;
    // Primary table: session_id -> authoritative runtime state.
    std::unordered_map<uint32_t, SessionRuntime> by_session_;
    // Reverse indexes let Gateway find the session from whichever event arrives:
    // Client disconnect, AppHost disconnect, or waitpid(AppHost pid).
    std::unordered_map<uintptr, uint32_t> by_client_;
    std::unordered_map<uintptr, uint32_t> by_apphost_;
    std::unordered_map<int, uint32_t> by_pid_;
};
