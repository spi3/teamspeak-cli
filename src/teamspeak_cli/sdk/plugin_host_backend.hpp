#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "ts3_functions.h"

#include "teamspeak_cli/bridge/media_bridge.hpp"
#include "teamspeak_cli/events/event_queue.hpp"
#include "teamspeak_cli/sdk/backend.hpp"

namespace teamspeak_cli::sdk {

class PluginHostBackend final
    : public Backend,
      public bridge::MediaBridgeHost,
      public bridge::MediaPlaybackControl {
  public:
    PluginHostBackend();

    void set_functions(const TS3Functions& functions);
    void set_plugin_id(std::string plugin_id);
    void on_current_server_connection_changed(std::uint64_t server_connection_handler_id);
    void on_connect_status_change(std::uint64_t server_connection_handler_id, int new_status, unsigned int error_number);
    void on_text_message(
        std::uint64_t server_connection_handler_id,
        unsigned int target_mode,
        std::uint16_t to_id,
        std::uint16_t from_id,
        const char* from_name,
        const char* from_unique_identifier,
        const char* message
    );
    void on_talk_status_change(std::uint64_t server_connection_handler_id, int status, std::uint16_t client_id);
    void on_playback_voice_data(
        std::uint64_t server_connection_handler_id,
        std::uint16_t client_id,
        short* samples,
        int sample_count,
        int channels
    );
    void on_captured_voice_data(
        std::uint64_t server_connection_handler_id,
        short* samples,
        int sample_count,
        int channels,
        int* edited
    );
    void on_client_move(
        std::uint64_t server_connection_handler_id,
        std::uint16_t client_id,
        std::uint64_t old_channel_id,
        std::uint64_t new_channel_id,
        const char* move_message
    );
    void on_server_error(
        std::uint64_t server_connection_handler_id,
        const char* error_message,
        unsigned int error,
        const char* return_code,
        const char* extra_message
    );

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

    void set_media_bridge(const std::shared_ptr<bridge::MediaBridge>& media_bridge) override;
    [[nodiscard]] auto activate_media_playback() -> domain::Result<void> override;
    [[nodiscard]] auto deactivate_media_playback() -> domain::Result<void> override;

  private:
    [[nodiscard]] auto resolve_media_speaker(
        std::uint64_t server_connection_handler_id,
        std::uint16_t client_id
    ) -> bridge::MediaSpeaker;
    [[nodiscard]] auto resolve_handler_id(bool require_connection) const -> domain::Result<std::uint64_t>;
    [[nodiscard]] auto translate_error(unsigned int error_code, std::string_view fallback) const -> domain::Error;
    [[nodiscard]] auto current_self_id(std::uint64_t server_connection_handler_id) const
        -> domain::Result<std::uint16_t>;
    [[nodiscard]] auto connection_phase(std::uint64_t server_connection_handler_id) const
        -> domain::Result<domain::ConnectionPhase>;
    [[nodiscard]] auto client_string(std::uint64_t server_connection_handler_id, std::uint16_t client_id, std::size_t flag) const
        -> domain::Result<std::string>;
    [[nodiscard]] auto client_bool(std::uint64_t server_connection_handler_id, std::uint16_t client_id, std::size_t flag) const
        -> domain::Result<bool>;
    [[nodiscard]] auto channel_string(std::uint64_t server_connection_handler_id, std::uint64_t channel_id, std::size_t flag) const
        -> domain::Result<std::string>;
    [[nodiscard]] auto channel_bool(std::uint64_t server_connection_handler_id, std::uint64_t channel_id, std::size_t flag) const
        -> domain::Result<bool>;
    [[nodiscard]] auto server_string(std::uint64_t server_connection_handler_id, std::size_t flag) const
        -> domain::Result<std::string>;
    [[nodiscard]] auto connection_host(std::uint64_t server_connection_handler_id, std::uint16_t client_id) const
        -> domain::Result<std::string>;
    [[nodiscard]] auto connection_port(std::uint64_t server_connection_handler_id, std::uint16_t client_id) const
        -> domain::Result<std::uint16_t>;
    [[nodiscard]] auto find_channel_by_selector(
        std::uint64_t server_connection_handler_id,
        const domain::Selector& selector
    ) const -> domain::Result<domain::Channel>;
    [[nodiscard]] auto find_client_by_selector(
        std::uint64_t server_connection_handler_id,
        const domain::Selector& selector
    ) const -> domain::Result<domain::Client>;

    mutable std::mutex mutex_;
    std::optional<TS3Functions> functions_;
    events::EventQueue events_;
    InitOptions options_;
    std::optional<ConnectRequest> last_connect_request_;
    std::string plugin_id_;
    std::uint64_t preferred_handler_id_ = 0;
    std::uint64_t managed_handler_id_ = 0;
    bool initialized_ = false;
    bool media_playback_active_ = false;
    std::shared_ptr<bridge::MediaBridge> media_bridge_;
    std::map<std::pair<std::uint64_t, std::uint16_t>, bridge::MediaSpeaker> media_speakers_;
};

}  // namespace teamspeak_cli::sdk
