#pragma once

#include <string>

#include "teamspeak_cli/bridge/protocol.hpp"
#include "teamspeak_cli/sdk/backend.hpp"

namespace teamspeak_cli::sdk {

class SocketBackend final : public Backend {
  public:
    [[nodiscard]] auto kind() const -> std::string override;

    auto initialize(const InitOptions& options) -> domain::Result<void> override;
    auto shutdown() -> domain::Result<void> override;
    auto connect(const ConnectRequest& request) -> domain::Result<void> override;
    auto disconnect(std::string_view reason) -> domain::Result<void> override;

    [[nodiscard]] auto plugin_info() const -> domain::Result<domain::PluginInfo> override;
    [[nodiscard]] auto connection_state() const -> domain::Result<domain::ConnectionState> override;
    [[nodiscard]] auto server_info() const -> domain::Result<domain::ServerInfo> override;
    [[nodiscard]] auto list_channels() const -> domain::Result<std::vector<domain::Channel>> override;
    [[nodiscard]] auto list_clients() const -> domain::Result<std::vector<domain::Client>> override;
    [[nodiscard]] auto get_channel(const domain::Selector& selector) const
        -> domain::Result<domain::Channel> override;
    [[nodiscard]] auto get_client(const domain::Selector& selector) const
        -> domain::Result<domain::Client> override;
    auto join_channel(const domain::Selector& selector) -> domain::Result<void> override;
    auto set_self_muted(bool muted) -> domain::Result<void> override;
    auto set_self_away(bool away, std::string_view message) -> domain::Result<void> override;
    auto send_message(const domain::MessageRequest& request) -> domain::Result<void> override;
    auto next_event(std::chrono::milliseconds timeout)
        -> domain::Result<std::optional<domain::Event>> override;

  private:
    auto exchange(
        std::string_view operation,
        const bridge::protocol::Fields& request,
        std::chrono::milliseconds timeout
    ) const
        -> domain::Result<bridge::protocol::Response>;
    auto require_initialized() const -> domain::Result<void>;
    [[nodiscard]] auto command_timeout() const -> std::chrono::milliseconds;

    InitOptions options_;
    std::string socket_path_;
    bool initialized_ = false;
};

}  // namespace teamspeak_cli::sdk
