#include "src/gateway/session_manager.h"

namespace {

SessionTable::Route route_from(const SessionRuntime& rt) {
    return SessionTable::Route{rt.client_conn, rt.apphost_conn};
}

SessionSnapshot snapshot_from(const SessionRuntime& rt) {
    SessionSnapshot snap{};
    snap.session_id = rt.session_id;
    snap.state = rt.state;
    snap.client_conn = rt.client_conn;
    snap.apphost_conn = rt.apphost_conn;
    snap.apphost_pid = rt.apphost_pid;
    snap.spawn_deadline = rt.spawn_deadline;
    snap.last_client_seen = rt.last_client_seen;
    snap.last_apphost_seen = rt.last_apphost_seen;
    return snap;
}

} // namespace

uint32_t SessionTable::create_for_client(uintptr client_conn) {
    const auto now = GatewayClock::now();
    return create_spawning_session(client_conn, "", "", now, std::chrono::milliseconds(0));
}

uint32_t SessionTable::create_spawning_session(uintptr client_conn,
                                               const std::string& app_name,
                                               const std::string& plugin_path,
                                               GatewayTimePoint now,
                                               std::chrono::milliseconds spawn_timeout) {
    std::lock_guard<std::mutex> lk(mutex_);
    const uint32_t session_id = next_session_id_++;

    SessionRuntime rt{};
    rt.session_id = session_id;
    rt.state = SessionState::SPAWNING;
    rt.client_conn = client_conn;
    rt.app_name = app_name;
    rt.plugin_path = plugin_path;
    rt.created_at = now;
    rt.spawn_deadline = now + spawn_timeout;
    rt.last_client_seen = now;
    rt.last_apphost_seen = now;

    by_session_[session_id] = std::move(rt);
    by_client_[client_conn] = session_id;
    return session_id;
}

bool SessionTable::set_apphost_pid(uint32_t session_id, int pid) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto session_iter = by_session_.find(session_id);
    if (session_iter == by_session_.end())
        return false;
    if (session_iter->second.apphost_pid > 0)
        by_pid_.erase(session_iter->second.apphost_pid);
    session_iter->second.apphost_pid = pid;
    if (pid > 0)
        by_pid_[pid] = session_id;
    return true;
}

bool SessionTable::bind_apphost(uint32_t session_id, uintptr apphost_conn) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto session_iter = by_session_.find(session_id);
    if (session_iter == by_session_.end() || session_iter->second.state == SessionState::DEAD)
        return false;

    if (session_iter->second.apphost_conn != 0)
        by_apphost_.erase(session_iter->second.apphost_conn);
    session_iter->second.apphost_conn = apphost_conn;
    session_iter->second.state = SessionState::ACTIVE;
    session_iter->second.last_apphost_seen = GatewayClock::now();
    by_apphost_[apphost_conn] = session_id;
    return true;
}

SessionTable::Route SessionTable::find(uint32_t session_id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto session_iter = by_session_.find(session_id);
    if (session_iter == by_session_.end())
        return Route{};
    return route_from(session_iter->second);
}

uintptr SessionTable::find_apphost(uint32_t session_id) const {
    return find(session_id).apphost_conn;
}

uintptr SessionTable::find_client(uint32_t session_id) const {
    return find(session_id).client_conn;
}

uint32_t SessionTable::find_by_pid(int pid) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto pid_index = by_pid_.find(pid);
    return pid_index == by_pid_.end() ? 0 : pid_index->second;
}

bool SessionTable::has_active_or_spawning_for_client(uintptr client_conn) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto client_index = by_client_.find(client_conn);
    if (client_index == by_client_.end())
        return false;
    auto session_iter = by_session_.find(client_index->second);
    if (session_iter == by_session_.end())
        return false;
    return session_iter->second.state == SessionState::SPAWNING ||
           session_iter->second.state == SessionState::ACTIVE;
}

bool SessionTable::mark_client_seen(uint32_t session_id, GatewayTimePoint now) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto session_iter = by_session_.find(session_id);
    if (session_iter == by_session_.end())
        return false;
    session_iter->second.last_client_seen = now;
    return true;
}

bool SessionTable::mark_apphost_seen(uint32_t session_id, GatewayTimePoint now) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto session_iter = by_session_.find(session_id);
    if (session_iter == by_session_.end())
        return false;
    session_iter->second.last_apphost_seen = now;
    return true;
}

std::vector<SessionSnapshot> SessionTable::snapshot_sessions() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<SessionSnapshot> out;
    out.reserve(by_session_.size());
    for (const auto& item : by_session_)
        out.push_back(snapshot_from(item.second));
    return out;
}

bool SessionTable::transition_to_closing(uint32_t session_id, Route* route, int* pid) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto session_iter = by_session_.find(session_id);
    if (session_iter == by_session_.end() || session_iter->second.state == SessionState::DEAD ||
        session_iter->second.state == SessionState::CLOSING)
        return false;
    session_iter->second.state = SessionState::CLOSING;
    if (route)
        *route = route_from(session_iter->second);
    if (pid)
        *pid = session_iter->second.apphost_pid;
    return true;
}

bool SessionTable::get_pid(uint32_t session_id, int* pid) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto session_iter = by_session_.find(session_id);
    if (session_iter == by_session_.end())
        return false;
    if (pid)
        *pid = session_iter->second.apphost_pid;
    return true;
}

void SessionTable::remove_session(uint32_t session_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto session_iter = by_session_.find(session_id);
    if (session_iter == by_session_.end())
        return;

    if (session_iter->second.client_conn != 0)
        by_client_.erase(session_iter->second.client_conn);
    if (session_iter->second.apphost_conn != 0)
        by_apphost_.erase(session_iter->second.apphost_conn);
    if (session_iter->second.apphost_pid > 0)
        by_pid_.erase(session_iter->second.apphost_pid);
    by_session_.erase(session_iter);
}

uint32_t SessionTable::remove_by_client(uintptr client_conn) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto client_index = by_client_.find(client_conn);
    if (client_index == by_client_.end())
        return 0;

    const uint32_t session_id = client_index->second;
    auto session_iter = by_session_.find(session_id);
    if (session_iter != by_session_.end()) {
        if (session_iter->second.apphost_conn != 0)
            by_apphost_.erase(session_iter->second.apphost_conn);
        if (session_iter->second.apphost_pid > 0)
            by_pid_.erase(session_iter->second.apphost_pid);
        by_session_.erase(session_iter);
    }
    by_client_.erase(client_index);
    return session_id;
}

uint32_t SessionTable::remove_by_apphost(uintptr apphost_conn) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto apphost_index = by_apphost_.find(apphost_conn);
    if (apphost_index == by_apphost_.end())
        return 0;

    const uint32_t session_id = apphost_index->second;
    auto session_iter = by_session_.find(session_id);
    if (session_iter != by_session_.end()) {
        if (session_iter->second.client_conn != 0)
            by_client_.erase(session_iter->second.client_conn);
        if (session_iter->second.apphost_pid > 0)
            by_pid_.erase(session_iter->second.apphost_pid);
        by_session_.erase(session_iter);
    }
    by_apphost_.erase(apphost_index);
    return session_id;
}
