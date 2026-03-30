#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <core/Logger.hpp>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <queue>

namespace bot {

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

// -----------------------------------------------------------------------------
// Session: Represents a single active WebSocket connection to a UI client
// -----------------------------------------------------------------------------
class WsSession : public std::enable_shared_from_this<WsSession> {
public:
    explicit WsSession(tcp::socket&& socket, std::function<void(std::shared_ptr<WsSession>)> on_close)
        : ws_(std::move(socket)), on_close_callback_(std::move(on_close)) {}

    void run() {
        // Set suggested timeout settings for the websocket
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));

        // Accept the websocket handshake
        ws_.async_accept(
            beast::bind_front_handler(&WsSession::on_accept, shared_from_this()));
    }

    void send(const std::string& msg) {
        // Post the write to the strand to ensure thread safety
        net::post(ws_.get_executor(),
            beast::bind_front_handler(&WsSession::queue_write, shared_from_this(), msg));
    }

private:
    void on_accept(beast::error_code ec) {
        if (ec) {
            LOG_ERROR("[UiWebSocketServer] WebSocket Accept error: {}", ec.message());
            return;
        }
        LOG_INFO("[UiWebSocketServer] UI Terminal Connected.");
        do_read(); // Must start reading to detect disconnects
    }

    void do_read() {
        ws_.async_read(buffer_,
            beast::bind_front_handler(&WsSession::on_read, shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);
        
        // This indicates the client closed the connection
        if (ec == websocket::error::closed || ec == net::error::eof || ec == net::error::connection_reset) {
            LOG_INFO("[UiWebSocketServer] UI Terminal Disconnected.");
            on_close_callback_(shared_from_this());
            return;
        }

        if (ec) LOG_ERROR("[UiWebSocketServer] WebSocket Read error: {}", ec.message());
        
        buffer_.consume(buffer_.size()); // We ignore incoming data for now
        do_read(); // Continue reading
    }

    void queue_write(std::string msg) {
        queue_.push(std::move(msg));
        if (queue_.size() == 1) { // If it was empty, start writing
            do_write();
        }
    }

    void do_write() {
        ws_.async_write(
            net::buffer(queue_.front()),
            beast::bind_front_handler(&WsSession::on_write, shared_from_this()));
    }

    void on_write(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);
        if (ec) {
            LOG_ERROR("[UiWebSocketServer] WebSocket Write error: {}", ec.message());
            return;
        }

        queue_.pop();
        if (!queue_.empty()) {
            do_write();
        }
    }

    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    std::queue<std::string> queue_;
    std::function<void(std::shared_ptr<WsSession>)> on_close_callback_;
};

// -----------------------------------------------------------------------------
// Server: Listens for connections and manages broadcasting
// -----------------------------------------------------------------------------
class UiWebSocketServer : public std::enable_shared_from_this<UiWebSocketServer> {
public:
    UiWebSocketServer(net::io_context& ioc, tcp::endpoint endpoint,
                      std::function<void(std::shared_ptr<WsSession>)> on_connect)
        : ioc_(ioc), acceptor_(ioc), on_connect_callback_(std::move(on_connect)){
        
        beast::error_code ec;
        acceptor_.open(endpoint.protocol(), ec);
        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        acceptor_.bind(endpoint, ec);
        acceptor_.listen(net::socket_base::max_listen_connections, ec);

        if (ec) {
            LOG_ERROR("[UiWebSocketServer] WebSocket Server Bind Error: {}", ec.message());
            return;
        }
    }

    void start() {
        do_accept();
    }

    void broadcast(const std::string& message) {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (auto const& session : sessions_) {
            session->send(message);
        }
    }

private:
    void do_accept() {
        acceptor_.async_accept(
            net::make_strand(ioc_),
            beast::bind_front_handler(&UiWebSocketServer::on_accept, shared_from_this()));
    }

    void on_accept(beast::error_code ec, tcp::socket socket) {
        if (!ec) {
            // Create a session and add it to our active list
            auto session = std::make_shared<WsSession>(
                std::move(socket),
                [this](std::shared_ptr<WsSession> s) { remove_session(s); }
            );
            
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                sessions_.insert(session);
            }
            session->run();

            // Trigger the callback to send cached data to this specific late-joiner
            if (on_connect_callback_) {
                on_connect_callback_(session);
            }

        } else {
            LOG_ERROR("[UiWebSocketServer] WebSocket Accept error: {}", ec.message());
        }

        do_accept(); // Listen for next connection
    }

    void remove_session(std::shared_ptr<WsSession> session) {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.erase(session);
    }

    net::io_context& ioc_;
    tcp::acceptor acceptor_;
    
    std::mutex sessions_mutex_;
    std::unordered_set<std::shared_ptr<WsSession>> sessions_;
    std::function<void(std::shared_ptr<WsSession>)> on_connect_callback_;
};

} // namespace bot
