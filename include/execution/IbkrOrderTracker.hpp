#pragma once

#include <core/DomainModels.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace bot {

// Translates IBKR's raw order callbacks into clean ExecutionReports. Pure logic, no TWS
// connection, so it can be unit tested with IBKR-shaped inputs.
//
// IBKR delivers order progress through three overlapping channels:
//   * execDetails  - one per execution, with a unique execId. The only source of FILLS here.
//   * orderStatus  - status + cumulative filled, often repeated, sometimes before the
//                    matching execDetails arrives. Used for STATUS only (no fill quantity).
//   * error        - some rejections/cancellations arrive only as error codes.
//
// Guarantees for the reports it returns:
//   * every fill is reported exactly once (deduplicated by execId);
//   * a terminal status (FILLED/CANCELED/REJECTED) is reported once, and only after the
//     executions it implies have been reported, so positions are complete by the time a
//     strategy sees the leg as done;
//   * nothing is reported after a terminal status except late executions (as fills);
//   * every field of every report is initialised.
// Thread-safe.
class IbkrOrderTracker {
public:
    // An order this process is about to send.
    void onPlaced(long orderId, const OrderRequest& request);

    // An order learned from TWS (openOrder), e.g. one placed by a previous run of the bot.
    void onOpenOrder(long orderId, const std::string& orderRef, const std::string& symbol,
                     Side side, double quantity);

    std::optional<long> orderIdFor(const std::string& clientOrderId) const;
    bool isTracked(long orderId) const;

    std::optional<ExecutionReport> onOrderStatus(long orderId, const std::string& status,
                                                 double filled, double avgFillPrice);

    // ibSide is IBKR's execution side: "BOT" or "SLD".
    std::optional<ExecutionReport> onExecution(long orderId, const std::string& orderRef,
                                               const std::string& execId, const std::string& symbol,
                                               const std::string& ibSide, double shares,
                                               double price, double avgPrice);

    std::optional<ExecutionReport> onError(long id, int errorCode);

    // Maps an IBKR status string. Unknown/pending statuses map to NEW (non-terminal).
    static OrderStatus parseStatus(const std::string& ibkrStatus);

    // Error codes that end an order. Others (warnings, informational) leave it working;
    // IBKR follows real rejections with orderStatus "Inactive"/"Cancelled" anyway.
    static bool isTerminalErrorCode(int errorCode);

private:
    struct Tracked {
        std::string client_order_id;
        std::string symbol;
        Side side = Side::UNKNOWN;
        double quantity = 0.0;
        double exec_filled = 0.0;    // sum of execution shares reported so far
        double avg_price = 0.0;
        OrderStatus reported = OrderStatus::NEW;
        double reported_cum = -1.0;  // cumulative qty of the last status report (dedupe)
        // A terminal status from orderStatus that is waiting for its executions.
        std::optional<OrderStatus> held_terminal;
        double held_filled = 0.0;
    };

    ExecutionReport baseReport(long orderId, const Tracked& t) const;
    Tracked* find(long orderId);

    mutable std::mutex mtx_;
    std::unordered_map<long, Tracked> orders_;
    std::unordered_map<std::string, long> idByClient_;
    std::unordered_set<std::string> seenExecIds_;
};

} // namespace bot
