#pragma once

#include <chrono>
#include <memory>
#include <string_view>

#include "teamspeak_cli/domain/models.hpp"
#include "teamspeak_cli/domain/result.hpp"

namespace teamspeak_cli::sdk {

struct InitOptions {
    bool verbose = false;
    bool debug = false;
    std::string socket_path;
    std::chrono::milliseconds command_timeout = std::chrono::seconds(5);
};

struct ConnectRequest {
    std::string host;
    std::uint16_t port = 9987;
    std::string nickname;
    std::string identity;
    std::string server_password;
    std::string channel_password;
    std::string default_channel;
    std::string profile_name;
};

class Backend {
  public:
    virtual ~Backend() = default;

    [[nodiscard]] virtual auto kind() const -> std::string = 0;
    virtual auto initialize(const InitOptions& options) -> domain::Result<void> = 0;
    virtual auto shutdown() -> domain::Result<void> = 0;
    virtual auto connect(const ConnectRequest& request) -> domain::Result<void> = 0;
    virtual auto disconnect(std::string_view reason) -> domain::Result<void> = 0;

    [[nodiscard]] virtual auto plugin_info() const -> domain::Result<domain::PluginInfo> = 0;
    [[nodiscard]] virtual auto connection_state() const -> domain::Result<domain::ConnectionState> = 0;
    [[nodiscard]] virtual auto server_info() const -> domain::Result<domain::ServerInfo> = 0;
    [[nodiscard]] virtual auto list_channels() const -> domain::Result<std::vector<domain::Channel>> = 0;
    [[nodiscard]] virtual auto list_clients() const -> domain::Result<std::vector<domain::Client>> = 0;
    [[nodiscard]] virtual auto get_channel(const domain::Selector& selector) const
        -> domain::Result<domain::Channel> = 0;
    [[nodiscard]] virtual auto get_client(const domain::Selector& selector) const
        -> domain::Result<domain::Client> = 0;
    virtual auto join_channel(const domain::Selector& selector) -> domain::Result<void> = 0;
    virtual auto set_self_muted(bool muted) -> domain::Result<void> = 0;
    virtual auto set_self_away(bool away, std::string_view message) -> domain::Result<void> = 0;
    virtual auto send_message(const domain::MessageRequest& request) -> domain::Result<void> = 0;
    virtual auto next_event(std::chrono::milliseconds timeout)
        -> domain::Result<std::optional<domain::Event>> = 0;
};

}  // namespace teamspeak_cli::sdk
