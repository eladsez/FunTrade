#pragma once

#include <string>
#include <vector>
#include <thread>
#include <memory>
#include <variant>
#include <unordered_map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <blockingconcurrentqueue.h>
#include <chrono>
#include <infra/UiWebSocketServer.hpp>
#include <core/Logger.hpp>

namespace bot {

// 1. Define lightweight, POD-like structs for the queue (No JSON here!)
struct MsgInitBasket {
    std::string basket_id;
    std::vector<std::string> symbols;
    std::vector<double> weights;
    double entry_long;
    double exit_long;
    double exit_short;
    double entry_short;
    std::vector<double> prices;  // startup seed (last close) per symbol; 0 = none
};

struct MsgZTick {
    std::string basket_id;
    double z_score;
};

struct MsgPrice {
    std::string basket_id;
    std::string symbol;
    double price;
};

struct MsgState {
    std::string basket_id;
    std::string master_state;
    std::string pos_state;
};

struct MsgFill {
    std::string basket_id;
    std::string symbol;
    int tgt_qty;
    int fill_qty;
};

struct MsgPnL {
    std::string pos_virtual_id;
    double realized_pnl;
    double unrealized_pnl;
};

// A variant holding any of the possible messages
using UiMessage = std::variant<MsgInitBasket, MsgZTick, MsgPrice, MsgState, MsgFill, MsgPnL>;

class UiBroadcaster {
public:
    static UiBroadcaster& getInstance() {
        static UiBroadcaster instance;
        return instance;
    }

    void startServer(unsigned short port = 9000) {
        if (is_running_) return;
        is_running_ = true;

        try {
            auto const address = boost::asio::ip::make_address("0.0.0.0");
            
            auto on_connect = [this](std::shared_ptr<WsSession> session) {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                for (const auto& [id, cached_json] : init_cache_) {
                    // Dump the LIVE, fully-updated JSON state to the new connection
                    session->send(cached_json.dump());
                }
            };

            server_ = std::make_shared<UiWebSocketServer>(ioc_, boost::asio::ip::tcp::endpoint{address, port}, on_connect);
            server_->start();
            
            // Start the ASIO network thread
            network_thread_ = std::thread([this]() { ioc_.run(); });
            
            // Start the background serialization thread
            serializer_thread_ = std::thread([this]() { processQueue(); });
            
            LOG_INFO("[UiBroadcaster] UI WebSocket Server started on ws://0.0.0.0:9000");
        } catch (const std::exception& e) {
            LOG_ERROR("[UiBroadcaster] Failed to start UI Server: {}", e.what());
        }

        startThrottleTimer();
    }

    void startThrottleTimer() {
        throttle_timer_.expires_after(throttle_interval_);
        throttle_timer_.async_wait([this](const boost::system::error_code& ec) {
            if (!ec && is_running_) {
                flushPendingUpdates();
                startThrottleTimer(); // Re-arm the timer
            }
        });
    }

    void stopServer() {
        if (is_running_) {
            is_running_ = false;
            
            // Push a dummy message to wake up the blocking queue
            msg_queue_.enqueue(MsgZTick{"SHUTDOWN", 0.0}); 
            
            if (serializer_thread_.joinable()) serializer_thread_.join();
            
            ioc_.stop();
            if (network_thread_.joinable()) network_thread_.join();
        }
    }

    // ------------------------------------------------------------------------
    // THE HOT PATH APIs: Wait-free, O(1), zero heap allocation.
    // ------------------------------------------------------------------------
    
    void broadcastNewBasket(std::string basket_id, std::vector<std::string> symbols, std::vector<double> weights,
                            double entry_long, double exit_long, double exit_short, double entry_short,
                            std::vector<double> prices = {}) {
        msg_queue_.enqueue(MsgInitBasket{
            std::move(basket_id), std::move(symbols), std::move(weights), 
            entry_long, exit_long, exit_short, entry_short, std::move(prices)
        });
    }

    void broadcastZTick(std::string basket_id, double z_score) {
        msg_queue_.enqueue(MsgZTick{std::move(basket_id), z_score});
    }

    void broadcastPrice(std::string basket_id, std::string symbol, double price) {
        msg_queue_.enqueue(MsgPrice{std::move(basket_id), std::move(symbol), price});
    }

    void broadcastState(std::string basket_id, std::string master_state, std::string pos_state) {
        msg_queue_.enqueue(MsgState{std::move(basket_id), std::move(master_state), std::move(pos_state)});
    }

    void broadcastFill(std::string basket_id, std::string symbol, int tgt_qty, int fill_qty) {
        msg_queue_.enqueue(MsgFill{std::move(basket_id), std::move(symbol), tgt_qty, fill_qty});
    }

    void broadcastBasketPnL(std::string pos_virtual_id, double realized_pnl, double unrealized_pnl) {
        msg_queue_.enqueue(MsgPnL{std::move(pos_virtual_id), realized_pnl, unrealized_pnl});
    }

private:
    UiBroadcaster() : ioc_(), is_running_(false), throttle_timer_(ioc_) {}
    ~UiBroadcaster() { stopServer(); }

    void flushPendingUpdates() {

        std::unordered_map<std::string, MsgPnL> local_pnl;
        std::unordered_map<std::string, MsgPrice> local_prices;
        std::unordered_map<std::string, MsgZTick> local_z;

        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            local_pnl.swap(pending_pnl_);
            local_prices.swap(pending_prices_);
            local_z.swap(pending_z_);
        }

        nlohmann::json j;

        // 1. Flush PnL
        for (const auto& [v_id, pnl_msg] : local_pnl) {
            // 1. Extract basket_id and symbol from "basketid_symbol" on the background thread
            std::string basket_id = "";
            std::string symbol = "";
            size_t last_underscore = pnl_msg.pos_virtual_id.find_last_of('_');

            if (last_underscore != std::string::npos) {
                basket_id = pnl_msg.pos_virtual_id.substr(0, last_underscore);
                symbol = pnl_msg.pos_virtual_id.substr(last_underscore + 1);
            }
            else {
                continue;
            }

            // Update the initial cache for new connections
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                
                auto it = init_cache_.find(basket_id);
                if (it != init_cache_.end() && it->second["legs"].contains(symbol)) {
                    it->second["legs"][symbol]["rpnl"] = pnl_msg.realized_pnl;
                    it->second["legs"][symbol]["upnl"] = pnl_msg.unrealized_pnl;
                }
            }
                    
            j = {{"type", "pnl"}, {"basket", basket_id}, {"sym", symbol}, {"rpnl", pnl_msg.realized_pnl}, {"upnl", pnl_msg.unrealized_pnl}};
            server_->broadcast(j.dump());
        }

        // 2. Flush Prices
        for (const auto& [symbol, price_msg] : local_prices) {
            j = {{"type", "price"}, {"basket", price_msg.basket_id}, {"sym", symbol}, {"p", price_msg.price}};
            server_->broadcast(j.dump());
        }

        // 2. Flush Z-Score
        for (const auto& [baske_id, z_score_msg] : local_z){
            j = {{"type", "z_tick"}, {"basket", baske_id}, {"z", z_score_msg.z_score}};
            server_->broadcast(j.dump());
        }
        
    }

    // ------------------------------------------------------------------------
    // BACKGROUND THREAD: Takes the hit for JSON serialization and Mutex locks
    // ------------------------------------------------------------------------
    void processQueue() {
        UiMessage msg;
        while (is_running_) {
            // Blocks efficiently until a message arrives
            msg_queue_.wait_dequeue(msg); 
            if (!is_running_) break;

            std::string json_str;

            // Capture 'this' so we can access init_cache_ and cache_mutex_
            std::visit([this, &json_str](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                nlohmann::json j;

                if constexpr (std::is_same_v<T, MsgInitBasket>) {
                    j["type"] = "init_basket";
                    j["basket"] = arg.basket_id;
                    j["master_state"] = "FLAT";
                    j["pos"] = "FLAT";
                    j["z"] = 0.0;
                    j["entry_long"] = arg.entry_long;
                    j["exit_long"] = arg.exit_long;
                    j["exit_short"] = arg.exit_short;
                    j["entry_short"] = arg.entry_short;
                    
                    nlohmann::json legs = nlohmann::json::object();
                    for (size_t i = 0; i < arg.symbols.size(); ++i) {
                        legs[arg.symbols[i]] = {
                            {"px", i < arg.prices.size() ? arg.prices[i] : 0.0}, {"wgt", arg.weights[i]}, {"tgt", 0}, {"fill", 0}, {"rpnl", 0.0}, {"upnl", 0.0}
                        };
                    }
                    j["legs"] = legs;
                    
                    json_str = j.dump();

                    std::lock_guard<std::mutex> lock(cache_mutex_);
                    init_cache_[arg.basket_id] = j; 
                }
                else if constexpr (std::is_same_v<T, MsgZTick>) {
                    {
                        std::lock_guard<std::mutex> lock(pending_mutex_);
                        pending_z_[arg.basket_id] = arg;
                    }
                    // Update the cached Z-score
                    std::lock_guard<std::mutex> lock(cache_mutex_);
                    if (init_cache_.count(arg.basket_id)) {
                        init_cache_[arg.basket_id]["z"] = arg.z_score;
                    }
                }
                else if constexpr (std::is_same_v<T, MsgPrice>) {
                    {
                        std::lock_guard<std::mutex> lock(pending_mutex_);
                        pending_prices_[arg.symbol] = arg;
                    }

                    // Update the cached Price
                    std::lock_guard<std::mutex> lock(cache_mutex_);
                    if (init_cache_.count(arg.basket_id)) {
                        init_cache_[arg.basket_id]["legs"][arg.symbol]["px"] = arg.price;
                    }
                }
                else if constexpr (std::is_same_v<T, MsgState>) {
                    j = {{"type", "state"}, {"basket", arg.basket_id}, {"master", arg.master_state}, {"pos", arg.pos_state}};
                    json_str = j.dump();

                    // Update the cached Master State and Position
                    std::lock_guard<std::mutex> lock(cache_mutex_);
                    if (init_cache_.count(arg.basket_id)) {
                        init_cache_[arg.basket_id]["master_state"] = arg.master_state;
                        init_cache_[arg.basket_id]["pos"] = arg.pos_state;
                    }
                }
                else if constexpr (std::is_same_v<T, MsgFill>) {
                    j = {{"type", "fill"}, {"basket", arg.basket_id}, {"sym", arg.symbol}, {"tgt", arg.tgt_qty}, {"fill", arg.fill_qty}};
                    json_str = j.dump();

                    // Update the cached Target and Fill Quantities
                    std::lock_guard<std::mutex> lock(cache_mutex_);
                    if (init_cache_.count(arg.basket_id)) {
                        init_cache_[arg.basket_id]["legs"][arg.symbol]["tgt"] = arg.tgt_qty;
                        init_cache_[arg.basket_id]["legs"][arg.symbol]["fill"] = arg.fill_qty;
                    }
                }
                else if constexpr (std::is_same_v<T, MsgPnL>) {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    pending_pnl_[arg.pos_virtual_id] = arg;
                }
            }, msg);

            // Now that the heavy lifting is done, hand it off to the ASIO server
            if (server_ && !json_str.empty()) {
                server_->broadcast(json_str);
            }
        }
    }

    boost::asio::io_context ioc_;
    std::shared_ptr<UiWebSocketServer> server_;
    
    std::thread network_thread_;
    std::thread serializer_thread_; 
    std::atomic<bool> is_running_;

    moodycamel::BlockingConcurrentQueue<UiMessage> msg_queue_;

    std::unordered_map<std::string, nlohmann::json> init_cache_;
    std::mutex cache_mutex_;

    // Throttling mechanisms
    boost::asio::steady_timer throttle_timer_;
    std::chrono::milliseconds throttle_interval_{50}; // 20 updates per second
    
    // Caches to hold the latest state before flushing
    std::unordered_map<std::string, MsgPnL> pending_pnl_;
    std::unordered_map<std::string, MsgPrice> pending_prices_;
    std::unordered_map<std::string, MsgZTick> pending_z_;
    std::mutex pending_mutex_; // Protects the pending_* maps
};

} // namespace bot