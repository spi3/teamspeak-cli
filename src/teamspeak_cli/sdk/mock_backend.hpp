#pragma once

#include <atomic>
#include <mutex>
#include <thread>

#include "teamspeak_cli/events/event_queue.hpp"
#include "teamspeak_cli/sdk/backend.hpp"

namespace teamspeak_cli::sdk {

class MockBackend final : public Backend {
  public:
    MockBackend();
    ~MockBackend() override;

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
    auto send_message(const domain::MessageRequest& request) -> domain::Result<void> override;
    auto next_event(std::chrono::milliseconds timeout)
        -> domain::Result<std::optional<domain::Event>> override;

  private:
    auto require_connected() const -> domain::Result<void>;
    auto find_channel(const domain::Selector& selector) const -> domain::Result<domain::Channel>;
    auto find_client(const domain::Selector& selector) const -> domain::Result<domain::Client>;
    void start_event_loop();
    void stop_event_loop();

    mutable std::mutex mutex_;
    events::EventQueue events_;
    std::vector<domain::Channel> channels_;
    std::vector<domain::Client> clients_;
    domain::ConnectionState state_;
    domain::ServerInfo server_;
    InitOptions options_;
    bool initialized_ = false;
    std::jthread event_thread_;
};

}  // namespace teamspeak_cli::sdk
