#include <infra/ConnectionHealth.hpp>

#include <format>

namespace bot {

using Level = ConnectionHealth::Level;

std::string ConnectionHealth::duration(int64_t ms) {
    int64_t s = ms / 1000;
    if (s < 60) return std::format("{} s", s);
    if (s < 3600) return std::format("{} min", s / 60);
    return std::format("{} h {:02} min", s / 3600, s / 60 % 60);
}

std::optional<ConnectionHealth::Notice> ConnectionHealth::ibkrRestored(int64_t nowMs, const char* how) {
    // Caller holds mtx_.
    ibkr_ = true;
    if (!ibkrDownSince_) return std::nullopt;
    int64_t down = nowMs - *ibkrDownSince_;
    bool escalated = ibkrEscalatedAt_.has_value();
    ibkrDownSince_.reset();
    ibkrEscalatedAt_.reset();
    return Notice{escalated ? Level::Warn : Level::Info,
                  std::format("Gateway is connected to IBKR again after {} ({}).", duration(down), how)};
}

std::optional<ConnectionHealth::Notice> ConnectionHealth::socketUp(int64_t nowMs) {
    std::lock_guard<std::mutex> lock(mtx_);
    socket_ = true;
    auto ibkr = ibkrRestored(nowMs, "new Gateway session");
    if (!socketDownSince_) return ibkr;
    int64_t down = nowMs - *socketDownSince_;
    bool escalated = socketEscalatedAt_.has_value();
    socketDownSince_.reset();
    socketEscalatedAt_.reset();
    std::string text = std::format("Reconnected to the Gateway after {}.", duration(down));
    if (ibkr) text += " " + ibkr->text;
    return Notice{escalated || (ibkr && ibkr->level == Level::Warn) ? Level::Warn : Level::Info, text};
}

std::optional<ConnectionHealth::Notice> ConnectionHealth::socketDown(int64_t nowMs, const std::string& why) {
    std::lock_guard<std::mutex> lock(mtx_);
    socket_ = false;
    // Each failed attempt during an outage closes the socket again: only the first counts.
    if (socketDownSince_) return Notice{Level::Debug, std::format("Gateway connection still down ({}).", why)};
    socketDownSince_ = nowMs;
    return Notice{Level::Info, std::format("Lost the connection to the Gateway ({}). Reconnecting.", why)};
}

std::optional<ConnectionHealth::Notice> ConnectionHealth::retryFailed(int64_t nowMs) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!socketDownSince_) socketDownSince_ = nowMs;   // never connected yet (Gateway still starting)
    return Notice{Level::Debug, "Gateway not reachable. Retrying in 5 s."};
}

std::optional<ConnectionHealth::Notice> ConnectionHealth::ibkrDown(int64_t nowMs) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!ibkr_ && ibkrDownSince_) return Notice{Level::Debug, "Gateway still cut off from IBKR (1100)."};
    ibkr_ = false;
    ibkrDownSince_ = nowMs;
    ibkrEscalatedAt_.reset();
    return Notice{Level::Warn, "Gateway lost its connection to IBKR (1100). Orders and prices are paused "
                               "until it reconnects."};
}

std::optional<ConnectionHealth::Notice> ConnectionHealth::ibkrUp(int64_t nowMs, bool dataLost) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto n = ibkrRestored(nowMs, dataLost ? "1101: data lost, re-subscribing" : "1102: data kept");
    if (!n && dataLost) return Notice{Level::Info, "IBKR reports data lost (1101). Re-subscribing."};
    return n;
}

std::optional<ConnectionHealth::Notice> ConnectionHealth::escalate(int64_t nowMs, int64_t since, int64_t graceMs,
                                                                  std::optional<int64_t>& escalatedAt,
                                                                  const char* what) {
    // Caller holds mtx_.
    int64_t down = nowMs - since;
    if (down < graceMs) return std::nullopt;
    if (escalatedAt && nowMs - *escalatedAt < kReminderMs) return std::nullopt;
    escalatedAt = nowMs;
    std::string d = duration(down);
    return Notice{Level::Error, std::vformat(what, std::make_format_args(d))};
}

std::optional<ConnectionHealth::Notice> ConnectionHealth::tick(int64_t nowMs) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!socket_ && socketDownSince_)
        return escalate(nowMs, *socketDownSince_, kSocketGraceMs, socketEscalatedAt_,
                        "Can't reach the Gateway for {}: no orders or prices. Check that ib-gateway is "
                        "running and logged in.");
    if (socket_ && !ibkr_ && ibkrDownSince_)
        return escalate(nowMs, *ibkrDownSince_, kIbkrGraceMs, ibkrEscalatedAt_,
                        "Gateway cut off from IBKR for {}: no orders or prices. A failed re-login stays "
                        "stuck; restart ib-gateway if this lasts.");
    return std::nullopt;
}

ConnectionHealth::Snapshot ConnectionHealth::snapshot() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return Snapshot{socket_, socket_ && ibkr_, ibkrDownSince_};
}

} // namespace bot
