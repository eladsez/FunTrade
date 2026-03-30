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
#include <blockingconcurrentqueue.h> // The moodycamel queue you already have
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

// A variant holding any of the possible messages
using UiMessage = std::variant<MsgInitBasket, MsgZTick, MsgPrice, MsgState, MsgFill>;

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
                for (const auto& [id, msg] : init_cache_) {
                    session->send(msg);
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
    // THE HOT PATH APIs: These are now wait-free, O(1), and do zero heap allocation.
    // ------------------------------------------------------------------------
    
    void broadcastNewBasket(std::string basket_id, std::vector<std::string> symbols, std::vector<double> weights,
                            double entry_long, double exit_long, double exit_short, double entry_short) {
        msg_queue_.enqueue(MsgInitBasket{
            std::move(basket_id), std::move(symbols), std::move(weights), 
            entry_long, exit_long, exit_short, entry_short
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

private:
    UiBroadcaster() : ioc_(), is_running_(false) {}
    ~UiBroadcaster() { stopServer(); }

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
            bool is_init = false;
            std::string current_basket;

            // Use std::visit to figure out what type of message we just pulled
            std::visit([&json_str, &is_init, &current_basket](auto&& arg) {
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
                        legs[arg.symbols[i]] = {{"px", 0.0}, {"wgt", arg.weights[i]}, {"tgt", 0}, {"fill", 0}};
                    }
                    j["legs"] = legs;
                    is_init = true;
                    current_basket = arg.basket_id;
                }
                else if constexpr (std::is_same_v<T, MsgZTick>) {
                    j = {{"type", "z_tick"}, {"basket", arg.basket_id}, {"z", arg.z_score}};
                }
                else if constexpr (std::is_same_v<T, MsgPrice>) {
                    j = {{"type", "price"}, {"basket", arg.basket_id}, {"sym", arg.symbol}, {"p", arg.price}};
                }
                else if constexpr (std::is_same_v<T, MsgState>) {
                    j = {{"type", "state"}, {"basket", arg.basket_id}, {"master", arg.master_state}, {"pos", arg.pos_state}};
                }
                else if constexpr (std::is_same_v<T, MsgFill>) {
                    j = {{"type", "fill"}, {"basket", arg.basket_id}, {"sym", arg.symbol}, {"tgt", arg.tgt_qty}, {"fill", arg.fill_qty}};
                }
                
                json_str = j.dump();
            }, msg);

            if (is_init) {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                init_cache_[current_basket] = json_str;
            }

            // Now that the heavy lifting is done, hand it off to the ASIO server
            if (server_) {
                server_->broadcast(json_str);
            }
        }
    }

    boost::asio::io_context ioc_;
    std::shared_ptr<UiWebSocketServer> server_;
    
    std::thread network_thread_;
    std::thread serializer_thread_; // NEW: Dedicated thread for JSON
    std::atomic<bool> is_running_;

    // NEW: Lock-free queue separating the hot path from the JSON construction
    moodycamel::BlockingConcurrentQueue<UiMessage> msg_queue_;

    std::unordered_map<std::string, std::string> init_cache_;
    std::mutex cache_mutex_;
};

} // namespace bot