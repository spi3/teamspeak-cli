#pragma once

#include <functional>
#include <memory>

#include "teamspeak_cli/sdk/backend.hpp"

namespace teamspeak_cli::session {

struct ConnectResult {
    domain::ConnectionState state;
    std::vector<domain::Event> lifecycle;
    bool connected = false;
    bool timed_out = false;
    std::chrono::milliseconds timeout = std::chrono::milliseconds::zero();
};

struct DisconnectResult {
    domain::ConnectionState state;
    std::vector<domain::Event> lifecycle;
    bool disconnected = false;
    bool timed_out = false;
    std::chrono::milliseconds timeout = std::chrono::milliseconds::zero();
};

using ConnectEventCallback = std::function<void(const domain::Event&)>;

class SessionService {
  public:
    explicit SessionService(std::unique_ptr<sdk::Backend> backend);

    auto initialize(const sdk::InitOptions& options) -> domain::Result<void>;
    auto shutdown() -> domain::Result<void>;
    auto connect(const sdk::ConnectRequest& request) -> domain::Result<void>;
    auto connect_and_wait(
        const sdk::ConnectRequest& request,
        std::chrono::milliseconds timeout,
        ConnectEventCallback on_event = {}
    )
        -> domain::Result<ConnectResult>;
    auto disconnect(std::string_view reason) -> domain::Result<void>;
    auto disconnect_and_wait(
        std::string_view reason,
        std::chrono::milliseconds timeout,
        ConnectEventCallback on_event = {}
    ) -> domain::Result<DisconnectResult>;

    [[nodiscard]] auto plugin_info() const -> domain::Result<domain::PluginInfo>;
    [[nodiscard]] auto connection_state() const -> domain::Result<domain::ConnectionState>;
    [[nodiscard]] auto server_info() const -> domain::Result<domain::ServerInfo>;
    [[nodiscard]] auto list_channels() const -> domain::Result<std::vector<domain::Channel>>;
    [[nodiscard]] auto list_clients() const -> domain::Result<std::vector<domain::Client>>;
    [[nodiscard]] auto get_channel(const domain::Selector& selector) const -> domain::Result<domain::Channel>;
    [[nodiscard]] auto get_client(const domain::Selector& selector) const -> domain::Result<domain::Client>;
    auto join_channel(const domain::Selector& selector) -> domain::Result<void>;
    auto rename_channel(const domain::Selector& selector, std::string_view name)
        -> domain::Result<domain::Channel>;
    auto set_self_muted(bool muted) -> domain::Result<void>;
    auto set_self_away(bool away, std::string_view message) -> domain::Result<void>;
    auto send_message(const domain::MessageRequest& request) -> domain::Result<void>;
    auto apply_server_group(const domain::ServerGroupApplicationRequest& request)
        -> domain::Result<domain::ServerGroupApplication>;
    auto watch_events(std::size_t count, std::chrono::milliseconds timeout)
        -> domain::Result<std::vector<domain::Event>>;

  private:
    std::unique_ptr<sdk::Backend> backend_;
};

}  // namespace teamspeak_cli::session
