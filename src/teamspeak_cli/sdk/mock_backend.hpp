#pragma once

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

#include "teamspeak_cli/bridge/media_bridge.hpp"
#include "teamspeak_cli/events/event_queue.hpp"
#include "teamspeak_cli/sdk/backend.hpp"

namespace teamspeak_cli::sdk {

class MockBackend final : public Backend, public bridge::MediaBridgeHost, public bridge::MediaPlaybackControl {
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
    auto rename_channel(const domain::Selector& selector, std::string_view name)
        -> domain::Result<domain::Channel> override;
    auto set_self_muted(bool muted) -> domain::Result<void> override;
    auto set_self_away(bool away, std::string_view message) -> domain::Result<void> override;
    auto send_message(const domain::MessageRequest& request) -> domain::Result<void> override;
    auto apply_server_group(const domain::ServerGroupApplicationRequest& request)
        -> domain::Result<domain::ServerGroupApplication> override;
    auto next_event(std::chrono::milliseconds timeout)
        -> domain::Result<std::optional<domain::Event>> override;

    void set_media_bridge(const std::shared_ptr<bridge::MediaBridge>& media_bridge) override;
    [[nodiscard]] auto activate_media_playback() -> domain::Result<void> override;
    [[nodiscard]] auto deactivate_media_playback() -> domain::Result<void> override;

  private:
    auto require_connected() const -> domain::Result<void>;
    auto find_channel(const domain::Selector& selector) const -> domain::Result<domain::Channel>;
    auto find_client(const domain::Selector& selector) const -> domain::Result<domain::Client>;
    auto find_server_group(const domain::Selector& selector) const -> domain::Result<domain::ServerGroup>;
    auto client_database_id_for(domain::ClientId client_id) const -> domain::Result<domain::ClientDatabaseId>;
    void clear_pending_events();
    void start_event_loop();
    void stop_event_loop();

    mutable std::mutex mutex_;
    events::EventQueue events_;
    std::vector<domain::Channel> channels_;
    std::vector<domain::Client> clients_;
    std::vector<domain::ServerGroup> server_groups_;
    std::map<domain::ClientId, domain::ClientDatabaseId> client_database_ids_;
    domain::ConnectionState state_;
    domain::ServerInfo server_;
    InitOptions options_;
    bool initialized_ = false;
    bool self_muted_ = false;
    bool self_away_ = false;
    std::string self_away_message_;
    std::jthread event_thread_;
    std::jthread media_thread_;
    std::shared_ptr<bridge::MediaBridge> media_bridge_;
};

}  // namespace teamspeak_cli::sdk
