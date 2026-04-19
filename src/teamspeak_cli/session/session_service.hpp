#pragma once

#include <memory>

#include "teamspeak_cli/sdk/backend.hpp"

namespace teamspeak_cli::session {

class SessionService {
  public:
    explicit SessionService(std::unique_ptr<sdk::Backend> backend);

    auto initialize(const sdk::InitOptions& options) -> domain::Result<void>;
    auto shutdown() -> domain::Result<void>;
    auto connect(const sdk::ConnectRequest& request) -> domain::Result<void>;
    auto disconnect(std::string_view reason) -> domain::Result<void>;

    [[nodiscard]] auto plugin_info() const -> domain::Result<domain::PluginInfo>;
    [[nodiscard]] auto connection_state() const -> domain::Result<domain::ConnectionState>;
    [[nodiscard]] auto server_info() const -> domain::Result<domain::ServerInfo>;
    [[nodiscard]] auto list_channels() const -> domain::Result<std::vector<domain::Channel>>;
    [[nodiscard]] auto list_clients() const -> domain::Result<std::vector<domain::Client>>;
    [[nodiscard]] auto get_channel(const domain::Selector& selector) const -> domain::Result<domain::Channel>;
    [[nodiscard]] auto get_client(const domain::Selector& selector) const -> domain::Result<domain::Client>;
    auto join_channel(const domain::Selector& selector) -> domain::Result<void>;
    auto send_message(const domain::MessageRequest& request) -> domain::Result<void>;
    auto watch_events(std::size_t count, std::chrono::milliseconds timeout)
        -> domain::Result<std::vector<domain::Event>>;

  private:
    std::unique_ptr<sdk::Backend> backend_;
};

}  // namespace teamspeak_cli::session
