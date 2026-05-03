#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace teamspeak_cli::domain {

struct ConnectionHandle {
    std::uint64_t value = 0;
    auto operator<=>(const ConnectionHandle&) const = default;
};

struct ChannelId {
    std::uint64_t value = 0;
    auto operator<=>(const ChannelId&) const = default;
};

struct ClientId {
    std::uint64_t value = 0;
    auto operator<=>(const ClientId&) const = default;
};

struct ClientDatabaseId {
    std::uint64_t value = 0;
    auto operator<=>(const ClientDatabaseId&) const = default;
};

struct ServerGroupId {
    std::uint64_t value = 0;
    auto operator<=>(const ServerGroupId&) const = default;
};

enum class ConnectionPhase {
    disconnected,
    connecting,
    connected,
};

enum class MessageTargetKind {
    client,
    channel,
};

struct Profile {
    std::string name = "default";
    std::string backend = "mock";
    std::string host = "127.0.0.1";
    std::uint16_t port = 9987;
    std::string nickname = "terminal";
    std::string identity;
    std::string server_password;
    std::string channel_password;
    std::string default_channel = "Lobby";
    std::string control_socket_path;
};

struct AppConfig {
    int version = 1;
    std::string active_profile = "plugin-local";
    std::vector<Profile> profiles;
};

struct AudioDeviceBinding {
    bool known = false;
    std::string mode;
    std::string device;
    bool is_default = false;
};

struct MediaDiagnostics {
    AudioDeviceBinding capture;
    AudioDeviceBinding playback;
    std::string pulse_sink;
    std::string pulse_source;
    bool pulse_source_is_monitor = false;
    bool consumer_connected = false;
    bool playback_active = false;
    std::size_t queued_playback_samples = 0;
    std::size_t active_speaker_count = 0;
    std::size_t dropped_audio_chunks = 0;
    std::size_t dropped_playback_chunks = 0;
    std::string last_error;
    bool custom_capture_device_registered = false;
    std::string custom_capture_device_id;
    std::string custom_capture_device_name;
    bool custom_capture_path_available = false;
    bool injected_playback_attached_to_capture = false;
    bool captured_voice_edit_attached = false;
    bool transmit_path_ready = false;
    std::string transmit_path;
};

struct PluginInfo {
    std::string backend;
    std::string transport;
    std::string plugin_name;
    std::string plugin_version;
    bool plugin_available = false;
    std::string socket_path;
    std::string media_transport;
    std::string media_socket_path;
    std::string media_format;
    MediaDiagnostics media_diagnostics;
    std::string note;
};

struct ConnectionState {
    ConnectionPhase phase = ConnectionPhase::disconnected;
    std::string backend;
    ConnectionHandle connection;
    std::string server;
    std::uint16_t port = 0;
    std::string nickname;
    std::string identity;
    std::string profile;
    std::string mode = "one-shot";
};

struct Channel {
    ChannelId id;
    std::string name;
    std::optional<ChannelId> parent_id;
    std::size_t client_count = 0;
    bool is_default = false;
    bool subscribed = true;
};

struct Client {
    ClientId id;
    std::string nickname;
    std::string unique_identity;
    std::optional<ChannelId> channel_id;
    bool self = false;
    bool talking = false;
};

struct ServerGroup {
    ServerGroupId id;
    std::string name;
};

struct ServerInfo {
    std::string name;
    std::string host;
    std::uint16_t port = 0;
    std::string backend;
    std::optional<ChannelId> current_channel;
    std::size_t channel_count = 0;
    std::size_t client_count = 0;
};

struct Selector {
    std::string raw;
};

struct MessageRequest {
    MessageTargetKind target_kind = MessageTargetKind::channel;
    std::string target;
    std::string text;
};

struct ServerGroupApplicationRequest {
    std::string group;
    std::optional<std::string> client;
    std::optional<ClientDatabaseId> client_database_id;
};

struct ServerGroupApplication {
    ServerGroup server_group;
    std::optional<Client> client;
    ClientDatabaseId client_database_id;
};

struct Event {
    std::string type;
    std::string summary;
    std::chrono::system_clock::time_point at;
    std::map<std::string, std::string> fields;
};

inline auto to_string(ConnectionHandle handle) -> std::string {
    return std::to_string(handle.value);
}

inline auto to_string(ChannelId id) -> std::string {
    return std::to_string(id.value);
}

inline auto to_string(ClientId id) -> std::string {
    return std::to_string(id.value);
}

inline auto to_string(ClientDatabaseId id) -> std::string {
    return std::to_string(id.value);
}

inline auto to_string(ServerGroupId id) -> std::string {
    return std::to_string(id.value);
}

inline auto to_string(ConnectionPhase phase) -> std::string {
    switch (phase) {
        case ConnectionPhase::disconnected:
            return "disconnected";
        case ConnectionPhase::connecting:
            return "connecting";
        case ConnectionPhase::connected:
            return "connected";
    }
    return "unknown";
}

inline auto to_string(MessageTargetKind kind) -> std::string {
    switch (kind) {
        case MessageTargetKind::client:
            return "client";
        case MessageTargetKind::channel:
            return "channel";
    }
    return "unknown";
}

}  // namespace teamspeak_cli::domain
