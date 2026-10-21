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
    return ibkrRestored(nowMs, "new Gateway session");
}

std::optional<ConnectionHealth::Notice> ConnectionHealth::socketDown(int64_t) {
    std::lock_guard<std::mutex> lock(mtx_);
    socket_ = false;
    return std::nullopt;
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

std::optional<ConnectionHealth::Notice> ConnectionHealth::tick(int64_t nowMs) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!socket_ || ibkr_ || !ibkrDownSince_) return std::nullopt;
    int64_t down = nowMs - *ibkrDownSince_;
    if (down < kIbkrGraceMs) return std::nullopt;
    if (ibkrEscalatedAt_ && nowMs - *ibkrEscalatedAt_ < kReminderMs) return std::nullopt;
    ibkrEscalatedAt_ = nowMs;
    return Notice{Level::Error, std::format(
        "Gateway cut off from IBKR for {}: no orders or prices. A failed re-login stays stuck; "
        "restart ib-gateway if this lasts.", duration(down))};
}

ConnectionHealth::Snapshot ConnectionHealth::snapshot() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return Snapshot{socket_, socket_ && ibkr_, ibkrDownSince_};
}

} // namespace bot
