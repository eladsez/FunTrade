#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace bot {

// Two links fail independently: our API socket to the Gateway, and the Gateway's own link to
// IBKR (error 1100). While the second is down the socket stays up, so "connected" alone
// overstated health: on 2026-10-21 a failed re-login left the Gateway cut off from 00:16 to
// 09:40 ET while status.json said connected.
//
// Pure bookkeeping (no I/O, time passed in) so it can be unit-tested: IbkrInfra feeds it
// events and logs the notices it returns. Short outages are routine (the Gateway restarts at
// 23:45 ET and IBKR resets its servers around midnight), so a reconnect logs one line when
// the link drops and one when it's back; only an outage past a grace period is an error.
class ConnectionHealth {
public:
    enum class Level { Debug, Info, Warn, Error };
    struct Notice {
        Level level;
        std::string text;
    };
    struct Snapshot {
        bool socket = false;                    // our API connection to the Gateway
        bool ibkr = false;                      // the Gateway's connection to IBKR
        std::optional<int64_t> ibkr_down_since; // epoch ms, while the Gateway is cut off
    };

    static constexpr int64_t kSocketGraceMs = 3 * 60'000;   // a Gateway restart takes ~1 min
    static constexpr int64_t kIbkrGraceMs = 5 * 60'000;
    static constexpr int64_t kReminderMs = 30 * 60'000;

    // A fresh API session (nextValidId). The Gateway only accepts it once logged in, so the
    // IBKR link counts as up.
    std::optional<Notice> socketUp(int64_t nowMs);
    std::optional<Notice> socketDown(int64_t nowMs, const std::string& why);
    std::optional<Notice> retryFailed(int64_t nowMs);              // a connect attempt failed
    std::optional<Notice> ibkrDown(int64_t nowMs);                 // 1100
    std::optional<Notice> ibkrUp(int64_t nowMs, bool dataLost);    // 1101 (lost) / 1102
    // Escalates an outage that outlasts its grace period, then reminds every kReminderMs.
    // Call periodically: a stuck Gateway sends nothing more.
    std::optional<Notice> tick(int64_t nowMs);
    Snapshot snapshot() const;

    static std::string duration(int64_t ms);   // "46 s", "12 min", "2 h 05 min"

private:
    std::optional<Notice> ibkrRestored(int64_t nowMs, const char* how);
    std::optional<Notice> escalate(int64_t nowMs, int64_t since, int64_t graceMs,
                                   std::optional<int64_t>& escalatedAt, const char* what);

    mutable std::mutex mtx_;
    bool socket_ = false;
    bool ibkr_ = false;
    std::optional<int64_t> ibkrDownSince_;
    std::optional<int64_t> ibkrEscalatedAt_;
    std::optional<int64_t> socketDownSince_;
    std::optional<int64_t> socketEscalatedAt_;
};

} // namespace bot
