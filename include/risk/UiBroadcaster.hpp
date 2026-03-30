#pragma once

#include <string>
#include <vector>
#include <thread>
#include <memory>
#include <core/Logger.hpp>
#include <nlohmann/json.hpp>
#include <boost/asio/io_context.hpp>
#include <infra/UiWebSocketServer.hpp>

namespace bot {

class UiBroadcaster {
public:
    static UiBroadcaster& getInstance() {
        static UiBroadcaster instance;
        return instance;
    }

    // Starts the server on a background thread. Call this once at startup.
    void startServer(unsigned short port = 9000) {
        if (is_running_) return;

        try {
            auto const address = boost::asio::ip::make_address("127.0.0.1");
            server_ = std::make_shared<UiWebSocketServer>(ioc_, boost::asio::ip::tcp::endpoint{address, port});
            server_->start();
            
            worker_thread_ = std::thread([this]() {
                LOG_INFO("UI WebSocket Server started on ws://127.0.0.1:9000");
                ioc_.run(); // This will block the worker thread, processing ASIO events
            });
            is_running_ = true;
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to start UI Server: {}", e.what());
        }
    }

    void stopServer() {
        if (is_running_) {
            ioc_.stop();
            if (worker_thread_.joinable()) {
                worker_thread_.join();
            }
            is_running_ = false;
        }
    }

    void broadcastNewBasket(const std::string& basket_id, 
                            const std::vector<std::string>& symbols, 
                            const std::vector<double>& weights) {
        nlohmann::json j;
        j["type"] = "init_basket";
        j["basket"] = basket_id;
        j["master_state"] = "FLAT";
        j["pos"] = "FLAT";
        j["z"] = 0.0;

        nlohmann::json legs = nlohmann::json::object();
        for (size_t i = 0; i < symbols.size(); ++i) {
            legs[symbols[i]] = {
                {"px", 0.0},
                {"wgt", weights[i]},
                {"tgt", 0},
                {"fill", 0}
            };
        }
        j["legs"] = legs;
        
        sendToWebSocket(j.dump());
    }

    void broadcastZTick(const std::string& basket_id, double z_score) {
        nlohmann::json j = {{"type", "z_tick"}, {"basket", basket_id}, {"z", z_score}};
        sendToWebSocket(j.dump());
    }

    void broadcastPrice(const std::string& basket_id, const std::string& symbol, double price) {
        nlohmann::json j = {{"type", "price"}, {"basket", basket_id}, {"sym", symbol}, {"p", price}};
        sendToWebSocket(j.dump());
    }

    void broadcastState(const std::string& basket_id, const std::string& master_state, const std::string& pos_state) {
        nlohmann::json j = {{"type", "state"}, {"basket", basket_id}, {"master", master_state}, {"pos", pos_state}};
        sendToWebSocket(j.dump());
    }

    void broadcastFill(const std::string& basket_id, const std::string& symbol, int tgt_qty, int fill_qty) {
        nlohmann::json j = {{"type", "fill"}, {"basket", basket_id}, {"sym", symbol}, {"tgt", tgt_qty}, {"fill", fill_qty}};
        sendToWebSocket(j.dump());
    }

private:
    UiBroadcaster() : ioc_(), is_running_(false) {}
    
    ~UiBroadcaster() {
        stopServer();
    }

    void sendToWebSocket(const std::string& json_str) {
        if (server_ && is_running_) {
            server_->broadcast(json_str);
        }
    }

    boost::asio::io_context ioc_;
    std::shared_ptr<UiWebSocketServer> server_;
    std::thread worker_thread_;
    bool is_running_;
};

} // namespace bot