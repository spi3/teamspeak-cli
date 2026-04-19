#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "teamspeak_cli/sdk/backend.hpp"

namespace teamspeak_cli::bridge {

class SocketBridgeServer {
  public:
    explicit SocketBridgeServer(std::unique_ptr<sdk::Backend> backend);
    ~SocketBridgeServer();

    auto start(const sdk::InitOptions& options) -> domain::Result<void>;
    auto stop() -> domain::Result<void>;

    [[nodiscard]] auto socket_path() const -> std::string;

  private:
    void accept_loop(std::stop_token stop_token);
    void handle_client(int client_fd);

    std::unique_ptr<sdk::Backend> backend_;
    mutable std::mutex mutex_;
    std::string socket_path_;
    int listen_fd_ = -1;
    bool running_ = false;
    std::jthread accept_thread_;
};

}  // namespace teamspeak_cli::bridge
