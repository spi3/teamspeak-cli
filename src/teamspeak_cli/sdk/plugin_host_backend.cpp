#include "teamspeak_cli/sdk/plugin_host_backend.hpp"

#include <chrono>
#include <thread>
#include <vector>

#include "teamspeak/public_definitions.h"
#include "teamspeak/public_errors.h"
#include "teamspeak/public_rare_definitions.h"
#include "ts3_functions.h"

#include "teamspeak_cli/build/version.hpp"
#include "teamspeak_cli/bridge/socket_paths.hpp"
#include "teamspeak_cli/util/strings.hpp"

namespace teamspeak_cli::sdk {
namespace {

constexpr std::string_view kCustomCaptureMode = "custom";
constexpr std::string_view kCustomCaptureDeviceId = "ts3cli_media_capture";
constexpr std::string_view kCustomCaptureDeviceName = "ts3cli Media Bridge";
constexpr int kMediaCaptureChunkFrames = bridge::kMediaSampleRate / 100;
constexpr auto kMediaCaptureChunkDuration = std::chrono::milliseconds(10);

auto backend_error(std::string code, std::string message, domain::ExitCode exit_code = domain::ExitCode::sdk)
    -> domain::Error {
    return domain::make_error("plugin", std::move(code), std::move(message), exit_code);
}

auto now_event(std::string type, std::string summary, std::map<std::string, std::string> fields = {})
    -> domain::Event {
    return domain::Event{
        .type = std::move(type),
        .summary = std::move(summary),
        .at = std::chrono::system_clock::now(),
        .fields = std::move(fields),
    };
}

template <typename T>
class Ts3Owned {
  public:
    Ts3Owned() = default;
    Ts3Owned(const TS3Functions* functions, T* pointer) : functions_(functions), pointer_(pointer) {}
    Ts3Owned(const Ts3Owned&) = delete;
    auto operator=(const Ts3Owned&) -> Ts3Owned& = delete;

    Ts3Owned(Ts3Owned&& other) noexcept : functions_(other.functions_), pointer_(other.pointer_) {
        other.pointer_ = nullptr;
    }

    auto operator=(Ts3Owned&& other) noexcept -> Ts3Owned& {
        if (this != &other) {
            reset();
            functions_ = other.functions_;
            pointer_ = other.pointer_;
            other.pointer_ = nullptr;
        }
        return *this;
    }

    ~Ts3Owned() { reset(); }

    [[nodiscard]] auto get() const -> T* { return pointer_; }

  private:
    void reset() {
        if (pointer_ != nullptr && functions_ != nullptr && functions_->freeMemory != nullptr) {
            functions_->freeMemory(pointer_);
            pointer_ = nullptr;
        }
    }

    const TS3Functions* functions_ = nullptr;
    T* pointer_ = nullptr;
};

template <typename T>
auto terminated_array_size(T* items) -> std::size_t {
    std::size_t size = 0;
    while (items != nullptr && items[size] != 0) {
        ++size;
    }
    return size;
}

}  // namespace

PluginHostBackend::PluginHostBackend() = default;

void PluginHostBackend::set_functions(const TS3Functions& functions) {
    std::lock_guard<std::mutex> lock(mutex_);
    functions_ = functions;
}

void PluginHostBackend::set_plugin_id(std::string plugin_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    plugin_id_ = std::move(plugin_id);
}

void PluginHostBackend::on_current_server_connection_changed(std::uint64_t server_connection_handler_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    preferred_handler_id_ = server_connection_handler_id;
}

void PluginHostBackend::on_connect_status_change(
    std::uint64_t server_connection_handler_id,
    int new_status,
    unsigned int error_number
) {
    bool should_stop_media_playback = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        preferred_handler_id_ = server_connection_handler_id;
    }

    if (error_number != ERROR_ok) {
        events_.push(now_event(
            "connection.error",
            "TeamSpeak client reported a connection error",
            {
                {"handler", std::to_string(server_connection_handler_id)},
                {"error", std::to_string(error_number)},
            }
        ));
        return;
    }

    switch (new_status) {
        case STATUS_CONNECTING:
            events_.push(now_event(
                "connection.connecting",
                "connection is starting",
                {{"handler", std::to_string(server_connection_handler_id)}}
            ));
            break;
        case STATUS_CONNECTION_ESTABLISHING:
        case STATUS_CONNECTION_ESTABLISHED:
        case STATUS_CONNECTED:
            events_.push(now_event(
                "connection.connected",
                "connection established",
                {{"handler", std::to_string(server_connection_handler_id)}}
            ));
            break;
        case STATUS_DISCONNECTED:
            {
                std::lock_guard<std::mutex> lock(mutex_);
                should_stop_media_playback =
                    media_playback_active_ && media_playback_session_.handler_id == server_connection_handler_id;
                for (auto it = media_speakers_.begin(); it != media_speakers_.end();) {
                    if (it->first.first == server_connection_handler_id) {
                        it = media_speakers_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            if (should_stop_media_playback) {
                const auto ignored = deactivate_media_playback();
                (void)ignored;
            }
            events_.push(now_event(
                "connection.disconnected",
                "connection closed",
                {{"handler", std::to_string(server_connection_handler_id)}}
            ));
            break;
        default:
            events_.push(now_event(
                "connection.status",
                "connection status changed",
                {
                    {"handler", std::to_string(server_connection_handler_id)},
                    {"status", std::to_string(new_status)},
                }
            ));
            break;
    }
}

void PluginHostBackend::on_text_message(
    std::uint64_t server_connection_handler_id,
    unsigned int target_mode,
    std::uint16_t to_id,
    std::uint16_t from_id,
    const char* from_name,
    const char* from_unique_identifier,
    const char* message
) {
    events_.push(now_event(
        "message.received",
        "received TeamSpeak text message",
        {
            {"handler", std::to_string(server_connection_handler_id)},
            {"target_mode", std::to_string(target_mode)},
            {"to_id", std::to_string(to_id)},
            {"from_id", std::to_string(from_id)},
            {"from_name", from_name == nullptr ? "" : from_name},
            {"from_unique_identifier", from_unique_identifier == nullptr ? "" : from_unique_identifier},
            {"text", message == nullptr ? "" : message},
        }
    ));
}

void PluginHostBackend::on_talk_status_change(
    std::uint64_t server_connection_handler_id,
    int status,
    std::uint16_t client_id
) {
    std::shared_ptr<bridge::MediaBridge> media_bridge;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        media_bridge = media_bridge_;
    }
    auto speaker = resolve_media_speaker(server_connection_handler_id, client_id);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status == STATUS_NOT_TALKING) {
            media_speakers_.erase({server_connection_handler_id, client_id});
        } else {
            media_speakers_[{server_connection_handler_id, client_id}] = speaker;
        }
    }
    if (media_bridge != nullptr) {
        if (status == STATUS_NOT_TALKING) {
            media_bridge->publish_speaker_stop(speaker);
        } else {
            media_bridge->publish_speaker_start(speaker);
        }
    }
    events_.push(now_event(
        "client.talking",
        status == STATUS_NOT_TALKING ? "client stopped talking" : "client is talking",
        {
            {"handler", std::to_string(server_connection_handler_id)},
            {"client_id", std::to_string(client_id)},
            {"status", std::to_string(status)},
        }
    ));
}

void PluginHostBackend::on_playback_voice_data(
    std::uint64_t server_connection_handler_id,
    std::uint16_t client_id,
    short* samples,
    int sample_count,
    int channels
) {
    std::shared_ptr<bridge::MediaBridge> media_bridge;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        media_bridge = media_bridge_;
    }
    if (media_bridge == nullptr) {
        return;
    }
    const auto speaker = resolve_media_speaker(server_connection_handler_id, client_id);
    media_bridge->publish_audio_chunk(
        speaker,
        bridge::kMediaSampleRate,
        channels,
        samples,
        sample_count
    );
}

void PluginHostBackend::on_captured_voice_data(
    std::uint64_t server_connection_handler_id,
    short* samples,
    int sample_count,
    int channels,
    int* edited
) {
    (void)server_connection_handler_id;
    (void)samples;
    (void)sample_count;
    (void)channels;
    (void)edited;
}

void PluginHostBackend::on_client_move(
    std::uint64_t server_connection_handler_id,
    std::uint16_t client_id,
    std::uint64_t old_channel_id,
    std::uint64_t new_channel_id,
    const char* move_message
) {
    events_.push(now_event(
        "client.moved",
        "client changed channels",
        {
            {"handler", std::to_string(server_connection_handler_id)},
            {"client_id", std::to_string(client_id)},
            {"old_channel_id", std::to_string(old_channel_id)},
            {"new_channel_id", std::to_string(new_channel_id)},
            {"message", move_message == nullptr ? "" : move_message},
        }
    ));
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto it = media_speakers_.find({server_connection_handler_id, client_id}); it != media_speakers_.end()) {
        it->second.channel_id =
            new_channel_id == 0 ? std::optional<domain::ChannelId>{}
                                : std::optional<domain::ChannelId>{domain::ChannelId{new_channel_id}};
    }
}

void PluginHostBackend::on_server_error(
    std::uint64_t server_connection_handler_id,
    const char* error_message,
    unsigned int error,
    const char* return_code,
    const char* extra_message
) {
    events_.push(now_event(
        "server.error",
        error_message == nullptr ? "server error" : error_message,
        {
            {"handler", std::to_string(server_connection_handler_id)},
            {"error", std::to_string(error)},
            {"return_code", return_code == nullptr ? "" : return_code},
            {"extra_message", extra_message == nullptr ? "" : extra_message},
        }
    ));
}

auto PluginHostBackend::kind() const -> std::string {
    return "plugin";
}

auto PluginHostBackend::initialize(const InitOptions& options) -> domain::Result<void> {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!functions_.has_value()) {
        return domain::fail(backend_error(
            "functions_unavailable", "TeamSpeak plugin host functions are not available"
        ));
    }
    options_ = options;
    initialized_ = true;
    return domain::ok();
}

auto PluginHostBackend::shutdown() -> domain::Result<void> {
    const auto stopped = deactivate_media_playback();
    if (!stopped) {
        return stopped;
    }

    TS3Functions functions{};
    std::jthread media_capture_thread;
    bool custom_capture_device_registered = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        initialized_ = false;
        if (functions_.has_value()) {
            functions = *functions_;
        }
        custom_capture_device_registered = custom_capture_device_registered_;
        custom_capture_device_registered_ = false;
        if (media_capture_thread_.joinable()) {
            media_capture_thread = std::move(media_capture_thread_);
        }
    }

    if (media_capture_thread.joinable()) {
        media_capture_thread.join();
    }

    if (custom_capture_device_registered && functions.unregisterCustomDevice != nullptr) {
        const unsigned int error = functions.unregisterCustomDevice(kCustomCaptureDeviceId.data());
        if (error != ERROR_ok && error != ERROR_sound_unknown_device) {
            return domain::fail(translate_error(error, "failed to unregister TeamSpeak custom capture device"));
        }
    }
    return domain::ok();
}

auto PluginHostBackend::connect(const ConnectRequest& request) -> domain::Result<void> {
    TS3Functions functions{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) {
            return domain::fail(backend_error("not_initialized", "plugin host backend is not initialized"));
        }
        functions = *functions_;
    }

    unsigned int connect_error = ERROR_ok;
    std::uint64_t handler_id = 0;
    if (functions.guiConnect != nullptr) {
        const std::string server_address = request.host + ":" + std::to_string(request.port);
        const char* user_identity = request.identity.empty() ? nullptr : request.identity.c_str();
        const char* nickname = request.nickname.empty() ? nullptr : request.nickname.c_str();
        const char* server_password = request.server_password.empty() ? nullptr : request.server_password.c_str();
        const char* default_channel = request.default_channel.empty() ? nullptr : request.default_channel.c_str();
        const char* channel_password = request.channel_password.empty() ? nullptr : request.channel_password.c_str();

        connect_error = functions.guiConnect(
            PLUGIN_CONNECT_TAB_NEW_IF_CURRENT_CONNECTED,
            request.host.c_str(),
            server_address.c_str(),
            server_password,
            nickname,
            default_channel,
            channel_password,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            user_identity,
            nullptr,
            nullptr,
            &handler_id
        );
        if (connect_error != ERROR_ok) {
            return domain::fail(translate_error(connect_error, "failed to start a TeamSpeak GUI connection"));
        }
    } else {
        if (request.identity.empty()) {
            return domain::fail(backend_error(
                "missing_identity",
                "the TeamSpeak plugin backend requires --identity when guiConnect is unavailable",
                domain::ExitCode::usage
            ));
        }

        const unsigned int spawn_error = functions.spawnNewServerConnectionHandler(0, &handler_id);
        if (spawn_error != ERROR_ok) {
            return domain::fail(translate_error(spawn_error, "failed to create a TeamSpeak connection handler"));
        }

        const char* default_channels[2] = {nullptr, nullptr};
        if (!request.default_channel.empty()) {
            default_channels[0] = request.default_channel.c_str();
        }

        connect_error = functions.startConnection(
            handler_id,
            request.identity.c_str(),
            request.host.c_str(),
            request.port,
            request.nickname.c_str(),
            default_channels[0] == nullptr ? nullptr : default_channels,
            request.channel_password.c_str(),
            request.server_password.c_str()
        );
        if (connect_error != ERROR_ok) {
            functions.destroyServerConnectionHandler(handler_id);
            return domain::fail(translate_error(connect_error, "failed to start a TeamSpeak connection"));
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        preferred_handler_id_ = handler_id;
        managed_handler_id_ = handler_id;
        last_connect_request_ = request;
    }
    events_.push(now_event(
        "connection.requested",
        "requested new TeamSpeak client connection",
        {{"server", request.host}, {"port", std::to_string(request.port)}}
    ));
    return domain::ok();
}

auto PluginHostBackend::disconnect(std::string_view reason) -> domain::Result<void> {
    auto handler_id = resolve_handler_id(false);
    if (!handler_id) {
        return domain::ok();
    }
    TS3Functions functions{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!functions_.has_value()) {
            return domain::fail(backend_error(
                "functions_unavailable", "TeamSpeak plugin host functions are not available"
            ));
        }
        functions = *functions_;
    }
    const unsigned int error = functions.stopConnection(handler_id.value(), std::string(reason).c_str());
    if (error != ERROR_ok) {
        return domain::fail(translate_error(error, "failed to stop TeamSpeak connection"));
    }
    return domain::ok();
}

auto PluginHostBackend::plugin_info() const -> domain::Result<domain::PluginInfo> {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!functions_.has_value()) {
        return domain::fail<domain::PluginInfo>(backend_error(
            "functions_unavailable", "TeamSpeak plugin host functions are not available"
        ));
    }

    std::string clientlib_version = "unknown";
    char* raw_version = nullptr;
    if ((*functions_).getClientLibVersion(&raw_version) == ERROR_ok && raw_version != nullptr) {
        Ts3Owned<char> version_owner(&*functions_, raw_version);
        clientlib_version = raw_version;
    }

    const std::string media_socket_path = media_bridge_ == nullptr ? std::string{} : media_bridge_->socket_path();
    return domain::ok(domain::PluginInfo{
        .backend = "plugin",
        .transport = "ts3-plugin/unix-socket",
        .plugin_name = "ts3cli-plugin",
        .plugin_version = TSCLI_VERSION,
        .plugin_available = true,
        .socket_path = bridge::resolve_socket_path(options_.socket_path),
        .media_transport = media_socket_path.empty() ? std::string{} : "unix-stream/frame-v1",
        .media_socket_path = media_socket_path,
        .media_format = bridge::media_format_description(),
        .note = "running inside the TeamSpeak client; linked client library version " + clientlib_version,
    });
}

auto PluginHostBackend::connection_state() const -> domain::Result<domain::ConnectionState> {
    auto handler_id = resolve_handler_id(false);
    domain::ConnectionState state{
        .phase = domain::ConnectionPhase::disconnected,
        .backend = "plugin",
        .connection = domain::ConnectionHandle{handler_id.ok() ? handler_id.value() : 0},
        .server = "",
        .port = 0,
        .nickname = "",
        .identity = "",
        .profile = last_connect_request_.has_value() ? last_connect_request_->profile_name : "",
        .mode = "plugin-control",
    };
    if (!handler_id || handler_id.value() == 0) {
        return domain::ok(state);
    }

    auto phase = connection_phase(handler_id.value());
    if (!phase) {
        return domain::fail<domain::ConnectionState>(phase.error());
    }
    state.phase = phase.value();

    auto self_id = current_self_id(handler_id.value());
    if (self_id) {
        auto nickname = client_string(handler_id.value(), self_id.value(), CLIENT_NICKNAME);
        if (nickname) {
            state.nickname = nickname.value();
        }
        auto identity = client_string(handler_id.value(), self_id.value(), CLIENT_UNIQUE_IDENTIFIER);
        if (identity) {
            state.identity = identity.value();
        }
        auto host = connection_host(handler_id.value(), self_id.value());
        if (host) {
            state.server = host.value();
        }
        auto port = connection_port(handler_id.value(), self_id.value());
        if (port) {
            state.port = port.value();
        }
    }
    if (state.server.empty() && last_connect_request_.has_value()) {
        state.server = last_connect_request_->host;
        state.port = last_connect_request_->port;
        if (state.nickname.empty()) {
            state.nickname = last_connect_request_->nickname;
        }
        if (state.identity.empty()) {
            state.identity = last_connect_request_->identity;
        }
    }
    return domain::ok(state);
}

auto PluginHostBackend::server_info() const -> domain::Result<domain::ServerInfo> {
    auto handler_id = resolve_handler_id(true);
    if (!handler_id) {
        return domain::fail<domain::ServerInfo>(handler_id.error());
    }

    auto name = server_string(handler_id.value(), VIRTUALSERVER_NAME);
    if (!name) {
        return domain::fail<domain::ServerInfo>(name.error());
    }

    auto clients = list_clients();
    if (!clients) {
        return domain::fail<domain::ServerInfo>(clients.error());
    }
    auto channels = list_channels();
    if (!channels) {
        return domain::fail<domain::ServerInfo>(channels.error());
    }

    std::optional<domain::ChannelId> current_channel;
    for (const auto& client : clients.value()) {
        if (client.self) {
            current_channel = client.channel_id;
            break;
        }
    }

    std::string host = last_connect_request_.has_value() ? last_connect_request_->host : "";
    std::uint16_t port = last_connect_request_.has_value() ? last_connect_request_->port : 0;

    return domain::ok(domain::ServerInfo{
        .name = name.value(),
        .host = std::move(host),
        .port = port,
        .backend = "plugin",
        .current_channel = current_channel,
        .channel_count = channels.value().size(),
        .client_count = clients.value().size(),
    });
}

auto PluginHostBackend::list_channels() const -> domain::Result<std::vector<domain::Channel>> {
    auto handler_id = resolve_handler_id(true);
    if (!handler_id) {
        return domain::fail<std::vector<domain::Channel>>(handler_id.error());
    }

    uint64_t* raw_channel_ids = nullptr;
    const auto& functions = *functions_;
    const unsigned int error = functions.getChannelList(handler_id.value(), &raw_channel_ids);
    if (error != ERROR_ok) {
        return domain::fail<std::vector<domain::Channel>>(translate_error(error, "failed to query TeamSpeak channels"));
    }
    Ts3Owned<uint64_t> channel_ids(&functions, raw_channel_ids);

    std::vector<domain::Channel> channels;
    channels.reserve(terminated_array_size(raw_channel_ids));
    for (std::size_t index = 0; raw_channel_ids != nullptr && raw_channel_ids[index] != 0; ++index) {
        const std::uint64_t channel_id = raw_channel_ids[index];
        auto name = channel_string(handler_id.value(), channel_id, CHANNEL_NAME);
        if (!name) {
            return domain::fail<std::vector<domain::Channel>>(name.error());
        }
        uint64_t parent_id = 0;
        functions.getParentChannelOfChannel(handler_id.value(), channel_id, &parent_id);
        auto is_default = channel_bool(handler_id.value(), channel_id, CHANNEL_FLAG_DEFAULT);
        if (!is_default) {
            return domain::fail<std::vector<domain::Channel>>(is_default.error());
        }

        anyID* raw_channel_clients = nullptr;
        std::size_t client_count = 0;
        if (functions.getChannelClientList(handler_id.value(), channel_id, &raw_channel_clients) == ERROR_ok &&
            raw_channel_clients != nullptr) {
            Ts3Owned<anyID> channel_clients(&functions, raw_channel_clients);
            client_count = terminated_array_size(raw_channel_clients);
        }

        channels.push_back(domain::Channel{
            .id = domain::ChannelId{channel_id},
            .name = name.value(),
            .parent_id = parent_id == 0 ? std::optional<domain::ChannelId>{}
                                        : std::optional<domain::ChannelId>{domain::ChannelId{parent_id}},
            .client_count = client_count,
            .is_default = is_default.value(),
            .subscribed = true,
        });
    }
    return domain::ok(std::move(channels));
}

auto PluginHostBackend::list_clients() const -> domain::Result<std::vector<domain::Client>> {
    auto handler_id = resolve_handler_id(true);
    if (!handler_id) {
        return domain::fail<std::vector<domain::Client>>(handler_id.error());
    }

    anyID* raw_client_ids = nullptr;
    const auto& functions = *functions_;
    const unsigned int error = functions.getClientList(handler_id.value(), &raw_client_ids);
    if (error != ERROR_ok) {
        return domain::fail<std::vector<domain::Client>>(translate_error(error, "failed to query TeamSpeak clients"));
    }
    Ts3Owned<anyID> client_ids(&functions, raw_client_ids);

    auto self_id = current_self_id(handler_id.value());
    if (!self_id) {
        return domain::fail<std::vector<domain::Client>>(self_id.error());
    }

    std::vector<domain::Client> clients;
    clients.reserve(terminated_array_size(raw_client_ids));
    for (std::size_t index = 0; raw_client_ids != nullptr && raw_client_ids[index] != 0; ++index) {
        const anyID client_id = raw_client_ids[index];
        auto nickname = client_string(handler_id.value(), client_id, CLIENT_NICKNAME);
        auto unique_identity = client_string(handler_id.value(), client_id, CLIENT_UNIQUE_IDENTIFIER);
        auto talking = client_bool(handler_id.value(), client_id, CLIENT_FLAG_TALKING);
        if (!nickname) {
            return domain::fail<std::vector<domain::Client>>(nickname.error());
        }
        if (!unique_identity) {
            return domain::fail<std::vector<domain::Client>>(unique_identity.error());
        }
        if (!talking) {
            return domain::fail<std::vector<domain::Client>>(talking.error());
        }
        uint64_t channel_id = 0;
        functions.getChannelOfClient(handler_id.value(), client_id, &channel_id);
        clients.push_back(domain::Client{
            .id = domain::ClientId{client_id},
            .nickname = nickname.value(),
            .unique_identity = unique_identity.value(),
            .channel_id = channel_id == 0 ? std::optional<domain::ChannelId>{}
                                          : std::optional<domain::ChannelId>{domain::ChannelId{channel_id}},
            .self = client_id == self_id.value(),
            .talking = talking.value(),
        });
    }
    return domain::ok(std::move(clients));
}

auto PluginHostBackend::get_channel(const domain::Selector& selector) const -> domain::Result<domain::Channel> {
    auto handler_id = resolve_handler_id(true);
    if (!handler_id) {
        return domain::fail<domain::Channel>(handler_id.error());
    }
    return find_channel_by_selector(handler_id.value(), selector);
}

auto PluginHostBackend::get_client(const domain::Selector& selector) const -> domain::Result<domain::Client> {
    auto handler_id = resolve_handler_id(true);
    if (!handler_id) {
        return domain::fail<domain::Client>(handler_id.error());
    }
    return find_client_by_selector(handler_id.value(), selector);
}

auto PluginHostBackend::join_channel(const domain::Selector& selector) -> domain::Result<void> {
    auto handler_id = resolve_handler_id(true);
    if (!handler_id) {
        return domain::fail(handler_id.error());
    }
    auto channel = find_channel_by_selector(handler_id.value(), selector);
    if (!channel) {
        return domain::fail(channel.error());
    }
    auto self_id = current_self_id(handler_id.value());
    if (!self_id) {
        return domain::fail(self_id.error());
    }
    const unsigned int error = (*functions_).requestClientMove(
        handler_id.value(),
        self_id.value(),
        channel.value().id.value,
        "",
        nullptr
    );
    if (error != ERROR_ok) {
        return domain::fail(translate_error(error, "failed to move TeamSpeak client to channel"));
    }
    return domain::ok();
}

auto PluginHostBackend::set_self_muted(bool muted) -> domain::Result<void> {
    auto handler_id = resolve_handler_id(true);
    if (!handler_id) {
        return domain::fail(handler_id.error());
    }

    TS3Functions functions{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!functions_.has_value()) {
            return domain::fail(backend_error(
                "functions_unavailable", "TeamSpeak plugin host functions are not available"
            ));
        }
        functions = *functions_;
    }

    if (functions.setClientSelfVariableAsInt == nullptr || functions.flushClientSelfUpdates == nullptr) {
        return domain::fail(backend_error(
            "client_self_update_unavailable",
            "TeamSpeak self-update functions are not available in the loaded plugin host"
        ));
    }

    const unsigned int set_error = functions.setClientSelfVariableAsInt(
        handler_id.value(), CLIENT_INPUT_MUTED, muted ? MUTEINPUT_MUTED : MUTEINPUT_NONE
    );
    if (set_error != ERROR_ok) {
        return domain::fail(translate_error(set_error, "failed to update TeamSpeak microphone mute state"));
    }

    const unsigned int flush_error = functions.flushClientSelfUpdates(handler_id.value(), nullptr);
    if (flush_error != ERROR_ok) {
        return domain::fail(translate_error(flush_error, "failed to flush TeamSpeak microphone mute update"));
    }

    return domain::ok();
}

auto PluginHostBackend::set_self_away(bool away, std::string_view message) -> domain::Result<void> {
    auto handler_id = resolve_handler_id(true);
    if (!handler_id) {
        return domain::fail(handler_id.error());
    }

    TS3Functions functions{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!functions_.has_value()) {
            return domain::fail(backend_error(
                "functions_unavailable", "TeamSpeak plugin host functions are not available"
            ));
        }
        functions = *functions_;
    }

    if (functions.setClientSelfVariableAsInt == nullptr || functions.setClientSelfVariableAsString == nullptr ||
        functions.flushClientSelfUpdates == nullptr) {
        return domain::fail(backend_error(
            "client_self_update_unavailable",
            "TeamSpeak self-update functions are not available in the loaded plugin host"
        ));
    }

    const unsigned int away_error = functions.setClientSelfVariableAsInt(
        handler_id.value(), CLIENT_AWAY, away ? AWAY_ZZZ : AWAY_NONE
    );
    if (away_error != ERROR_ok) {
        return domain::fail(translate_error(away_error, "failed to update TeamSpeak away status"));
    }

    const std::string away_message = away ? std::string(message) : std::string{};
    const unsigned int message_error = functions.setClientSelfVariableAsString(
        handler_id.value(), CLIENT_AWAY_MESSAGE, away_message.c_str()
    );
    if (message_error != ERROR_ok) {
        return domain::fail(translate_error(message_error, "failed to update TeamSpeak away message"));
    }

    const unsigned int flush_error = functions.flushClientSelfUpdates(handler_id.value(), nullptr);
    if (flush_error != ERROR_ok) {
        return domain::fail(translate_error(flush_error, "failed to flush TeamSpeak away status update"));
    }

    return domain::ok();
}

auto PluginHostBackend::send_message(const domain::MessageRequest& request) -> domain::Result<void> {
    auto handler_id = resolve_handler_id(true);
    if (!handler_id) {
        return domain::fail(handler_id.error());
    }
    const auto& functions = *functions_;
    unsigned int error = ERROR_ok;
    if (request.target_kind == domain::MessageTargetKind::client) {
        auto client = find_client_by_selector(handler_id.value(), domain::Selector{request.target});
        if (!client) {
            return domain::fail(client.error());
        }
        error = functions.requestSendPrivateTextMsg(
            handler_id.value(),
            request.text.c_str(),
            static_cast<anyID>(client.value().id.value),
            nullptr
        );
    } else {
        auto channel = find_channel_by_selector(handler_id.value(), domain::Selector{request.target});
        if (!channel) {
            return domain::fail(channel.error());
        }
        error = functions.requestSendChannelTextMsg(
            handler_id.value(),
            request.text.c_str(),
            channel.value().id.value,
            nullptr
        );
    }
    if (error != ERROR_ok) {
        return domain::fail(translate_error(error, "failed to send TeamSpeak text message"));
    }
    return domain::ok();
}

auto PluginHostBackend::next_event(std::chrono::milliseconds timeout)
    -> domain::Result<std::optional<domain::Event>> {
    return domain::ok(events_.pop_for(timeout));
}

void PluginHostBackend::set_media_bridge(const std::shared_ptr<bridge::MediaBridge>& media_bridge) {
    std::lock_guard<std::mutex> lock(mutex_);
    media_bridge_ = media_bridge;
}

auto PluginHostBackend::activate_media_playback() -> domain::Result<void> {
    std::jthread stale_media_capture_thread;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!media_playback_active_ && media_capture_thread_.joinable()) {
            stale_media_capture_thread = std::move(media_capture_thread_);
        }
    }
    if (stale_media_capture_thread.joinable()) {
        stale_media_capture_thread.join();
    }

    auto handler_id = resolve_handler_id(true);
    if (!handler_id) {
        return domain::fail(handler_id.error());
    }

    TS3Functions functions{};
    MediaPlaybackSession session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!functions_.has_value()) {
            return domain::fail(backend_error(
                "functions_unavailable", "TeamSpeak plugin host functions are not available"
            ));
        }
        if (media_playback_active_) {
            return domain::ok();
        }
        functions = *functions_;
    }
    const auto registered = ensure_custom_capture_device_registered(functions);
    if (!registered) {
        return registered;
    }

    session = snapshot_media_playback_session(handler_id.value(), functions);
    const auto activated = activate_custom_capture_device(session, functions);
    if (!activated) {
        const auto ignored = restore_media_playback_session(session, functions);
        (void)ignored;
        return activated;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    media_playback_active_ = true;
    media_playback_session_ = session;
    media_capture_thread_ = std::jthread([this, server_connection_handler_id = handler_id.value(), functions](std::stop_token stop_token) {
        media_capture_loop(stop_token, server_connection_handler_id, functions);
    });
    return domain::ok();
}

auto PluginHostBackend::deactivate_media_playback() -> domain::Result<void> {
    TS3Functions functions{};
    MediaPlaybackSession session;
    std::jthread media_capture_thread;
    bool have_functions = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!functions_.has_value()) {
            media_playback_active_ = false;
            media_playback_session_ = {};
            if (!media_capture_thread_.joinable()) {
                return domain::ok();
            }
        } else {
            functions = *functions_;
            have_functions = true;
        }
        if (!media_playback_active_ && !media_capture_thread_.joinable()) {
            return domain::ok();
        }
        session = media_playback_session_;
        media_playback_active_ = false;
        media_playback_session_ = {};
        if (media_capture_thread_.joinable()) {
            media_capture_thread_.request_stop();
            if (media_capture_thread_.get_id() != std::this_thread::get_id()) {
                media_capture_thread = std::move(media_capture_thread_);
            }
        }
    }

    if (media_capture_thread.joinable()) {
        media_capture_thread.join();
    }

    if (have_functions && session.handler_id != 0) {
        const auto restored = restore_media_playback_session(session, functions);
        if (!restored) {
            return restored;
        }
    }
    return domain::ok();
}

auto PluginHostBackend::ensure_custom_capture_device_registered(const TS3Functions& functions)
    -> domain::Result<void> {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (custom_capture_device_registered_) {
            return domain::ok();
        }
    }

    if (functions.registerCustomDevice == nullptr) {
        return domain::fail(backend_error(
            "custom_capture_unavailable",
            "TeamSpeak custom capture device registration is not available in the loaded plugin host"
        ));
    }

    const unsigned int error = functions.registerCustomDevice(
        kCustomCaptureDeviceId.data(),
        kCustomCaptureDeviceName.data(),
        bridge::kMediaSampleRate,
        bridge::kMediaPlaybackChannels,
        bridge::kMediaSampleRate,
        bridge::kMediaPlaybackChannels
    );
    if (error != ERROR_ok && error != ERROR_sound_device_already_registerred) {
        return domain::fail(translate_error(error, "failed to register TeamSpeak custom capture device"));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    custom_capture_device_registered_ = true;
    return domain::ok();
}

auto PluginHostBackend::snapshot_media_playback_session(
    std::uint64_t server_connection_handler_id,
    const TS3Functions& functions
) const -> MediaPlaybackSession {
    MediaPlaybackSession session{.handler_id = server_connection_handler_id};

    char* raw_capture_mode = nullptr;
    if (functions.getCurrentCaptureMode != nullptr &&
        functions.getCurrentCaptureMode(server_connection_handler_id, &raw_capture_mode) == ERROR_ok &&
        raw_capture_mode != nullptr) {
        Ts3Owned<char> capture_mode_owner(&functions, raw_capture_mode);
        session.capture_mode = raw_capture_mode;
        session.capture_mode_known = true;
    }

    char* raw_capture_device = nullptr;
    int capture_device_is_default = 0;
    if (functions.getCurrentCaptureDeviceName != nullptr &&
        functions.getCurrentCaptureDeviceName(
            server_connection_handler_id, &raw_capture_device, &capture_device_is_default
        ) == ERROR_ok &&
        raw_capture_device != nullptr) {
        Ts3Owned<char> capture_device_owner(&functions, raw_capture_device);
        const std::string capture_mode = session.capture_mode_known ? session.capture_mode : std::string{};
        if (!(capture_mode == kCustomCaptureMode && std::string(raw_capture_device) == kCustomCaptureDeviceId)) {
            session.capture_device = capture_device_is_default != 0 ? std::string{} : std::string(raw_capture_device);
            session.capture_device_known = true;
        } else {
            session.capture_mode.clear();
            session.capture_mode_known = false;
        }
    }

    int input_deactivated = INPUT_ACTIVE;
    if (functions.getClientSelfVariableAsInt != nullptr &&
        functions.getClientSelfVariableAsInt(server_connection_handler_id, CLIENT_INPUT_DEACTIVATED, &input_deactivated) ==
            ERROR_ok) {
        session.input_deactivated = input_deactivated;
        session.input_deactivated_known = true;
    }

    int input_muted = MUTEINPUT_NONE;
    if (functions.getClientSelfVariableAsInt != nullptr &&
        functions.getClientSelfVariableAsInt(server_connection_handler_id, CLIENT_INPUT_MUTED, &input_muted) ==
            ERROR_ok) {
        session.input_muted = input_muted;
        session.input_muted_known = true;
    }

    return session;
}

auto PluginHostBackend::activate_custom_capture_device(
    const MediaPlaybackSession& session,
    const TS3Functions& functions
) -> domain::Result<void> {
    if (functions.openCaptureDevice == nullptr || functions.processCustomCaptureData == nullptr ||
        functions.setClientSelfVariableAsInt == nullptr || functions.flushClientSelfUpdates == nullptr) {
        return domain::fail(backend_error(
            "custom_capture_unavailable",
            "TeamSpeak custom capture playback requires capture device and self-state controls from the plugin host"
        ));
    }

    if (functions.closeCaptureDevice != nullptr) {
        (void)functions.closeCaptureDevice(session.handler_id);
    }

    const unsigned int open_error = functions.openCaptureDevice(
        session.handler_id, kCustomCaptureMode.data(), kCustomCaptureDeviceId.data()
    );
    if (open_error != ERROR_ok) {
        return domain::fail(translate_error(open_error, "failed to open TeamSpeak custom capture device"));
    }

    if (functions.activateCaptureDevice != nullptr) {
        const unsigned int activate_error = functions.activateCaptureDevice(session.handler_id);
        if (activate_error != ERROR_ok) {
            return domain::fail(translate_error(
                activate_error, "failed to activate TeamSpeak custom capture device"
            ));
        }
    }

    const unsigned int input_active_error = functions.setClientSelfVariableAsInt(
        session.handler_id, CLIENT_INPUT_DEACTIVATED, INPUT_ACTIVE
    );
    if (input_active_error != ERROR_ok) {
        return domain::fail(translate_error(
            input_active_error, "failed to force TeamSpeak capture input active for media playback"
        ));
    }

    const unsigned int input_unmuted_error =
        functions.setClientSelfVariableAsInt(session.handler_id, CLIENT_INPUT_MUTED, MUTEINPUT_NONE);
    if (input_unmuted_error != ERROR_ok) {
        return domain::fail(translate_error(
            input_unmuted_error, "failed to unmute TeamSpeak capture input for media playback"
        ));
    }

    const unsigned int flush_error = functions.flushClientSelfUpdates(session.handler_id, nullptr);
    if (flush_error != ERROR_ok) {
        return domain::fail(translate_error(flush_error, "failed to flush TeamSpeak capture state update"));
    }

    return domain::ok();
}

auto PluginHostBackend::restore_media_playback_session(
    const MediaPlaybackSession& session,
    const TS3Functions& functions
) -> domain::Result<void> {
    if (session.handler_id == 0) {
        return domain::ok();
    }

    if (functions.closeCaptureDevice != nullptr) {
        (void)functions.closeCaptureDevice(session.handler_id);
    }

    if (functions.openCaptureDevice != nullptr && (session.capture_mode_known || session.capture_device_known)) {
        const char* capture_mode = session.capture_mode_known ? session.capture_mode.c_str() : "";
        const char* capture_device = session.capture_device_known ? session.capture_device.c_str() : "";
        const unsigned int open_error =
            functions.openCaptureDevice(session.handler_id, capture_mode, capture_device);
        if (open_error != ERROR_ok) {
            return domain::fail(translate_error(open_error, "failed to restore TeamSpeak capture device"));
        }
        if (functions.activateCaptureDevice != nullptr) {
            const unsigned int activate_error = functions.activateCaptureDevice(session.handler_id);
            if (activate_error != ERROR_ok) {
                return domain::fail(translate_error(
                    activate_error, "failed to reactivate the restored TeamSpeak capture device"
                ));
            }
        }
    }

    if (functions.setClientSelfVariableAsInt != nullptr && functions.flushClientSelfUpdates != nullptr &&
        (session.input_deactivated_known || session.input_muted_known)) {
        if (session.input_deactivated_known) {
            const unsigned int input_error = functions.setClientSelfVariableAsInt(
                session.handler_id, CLIENT_INPUT_DEACTIVATED, session.input_deactivated
            );
            if (input_error != ERROR_ok) {
                return domain::fail(translate_error(
                    input_error, "failed to restore TeamSpeak input deactivation state"
                ));
            }
        }
        if (session.input_muted_known) {
            const unsigned int mute_error = functions.setClientSelfVariableAsInt(
                session.handler_id, CLIENT_INPUT_MUTED, session.input_muted
            );
            if (mute_error != ERROR_ok) {
                return domain::fail(translate_error(mute_error, "failed to restore TeamSpeak input mute state"));
            }
        }
        const unsigned int flush_error = functions.flushClientSelfUpdates(session.handler_id, nullptr);
        if (flush_error != ERROR_ok) {
            return domain::fail(translate_error(flush_error, "failed to flush TeamSpeak input state restore"));
        }
    }

    return domain::ok();
}

void PluginHostBackend::media_capture_loop(
    std::stop_token stop_token,
    std::uint64_t server_connection_handler_id,
    TS3Functions functions
) {
    std::vector<short> samples(kMediaCaptureChunkFrames * bridge::kMediaPlaybackChannels);
    bool observed_bridge_playback = false;
    auto next_tick = std::chrono::steady_clock::now();

    while (!stop_token.stop_requested()) {
        std::shared_ptr<bridge::MediaBridge> media_bridge;
        bool playback_active = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            playback_active =
                media_playback_active_ && media_playback_session_.handler_id == server_connection_handler_id;
            media_bridge = media_bridge_;
        }

        if (!playback_active) {
            return;
        }

        const bool filled = media_bridge != nullptr && media_bridge->fill_playback_samples(
            bridge::kMediaSampleRate,
            bridge::kMediaPlaybackChannels,
            samples.data(),
            kMediaCaptureChunkFrames
        );

        if (!filled) {
            if (observed_bridge_playback) {
                const auto ignored = deactivate_media_playback();
                (void)ignored;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            next_tick = std::chrono::steady_clock::now();
            continue;
        }

        observed_bridge_playback = true;
        const unsigned int error = functions.processCustomCaptureData(
            kCustomCaptureDeviceId.data(), samples.data(), static_cast<int>(samples.size())
        );
        if (error != ERROR_ok) {
            events_.push(now_event(
                "media.playback.error",
                "failed to submit custom capture audio to TeamSpeak",
                {{"handler", std::to_string(server_connection_handler_id)}, {"error", std::to_string(error)}}
            ));
            const auto ignored = deactivate_media_playback();
            (void)ignored;
            return;
        }

        next_tick += kMediaCaptureChunkDuration;
        const auto now = std::chrono::steady_clock::now();
        if (next_tick > now) {
            std::this_thread::sleep_until(next_tick);
        } else {
            next_tick = now;
        }
    }
}

auto PluginHostBackend::resolve_handler_id(bool require_connection) const -> domain::Result<std::uint64_t> {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!functions_.has_value()) {
        return domain::fail<std::uint64_t>(backend_error(
            "functions_unavailable", "TeamSpeak plugin host functions are not available"
        ));
    }
    std::uint64_t handler_id = 0;
    if ((*functions_).getCurrentServerConnectionHandlerID != nullptr) {
        handler_id = (*functions_).getCurrentServerConnectionHandlerID();
    }
    if (handler_id == 0) {
        handler_id = preferred_handler_id_ != 0 ? preferred_handler_id_ : managed_handler_id_;
    }
    if (handler_id == 0) {
        if (require_connection) {
            return domain::fail<std::uint64_t>(backend_error(
                "not_connected", "no active TeamSpeak client connection", domain::ExitCode::connection
            ));
        }
        return domain::ok(static_cast<std::uint64_t>(0));
    }
    return domain::ok(handler_id);
}

auto PluginHostBackend::resolve_media_speaker(
    std::uint64_t server_connection_handler_id,
    std::uint16_t client_id
) -> bridge::MediaSpeaker {
    bridge::MediaSpeaker speaker{
        .handler_id = server_connection_handler_id,
        .client_id = domain::ClientId{client_id},
        .unique_identity = "",
        .nickname = "",
        .channel_id = std::nullopt,
    };
    std::optional<TS3Functions> functions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (const auto it = media_speakers_.find({server_connection_handler_id, client_id});
            it != media_speakers_.end()) {
            speaker = it->second;
        }
        functions = functions_;
    }

    const auto nickname = client_string(server_connection_handler_id, client_id, CLIENT_NICKNAME);
    if (nickname) {
        speaker.nickname = nickname.value();
    }
    const auto unique_identity = client_string(
        server_connection_handler_id, client_id, CLIENT_UNIQUE_IDENTIFIER
    );
    if (unique_identity) {
        speaker.unique_identity = unique_identity.value();
    }
    if (functions.has_value()) {
        uint64_t channel_id = 0;
        if ((*functions).getChannelOfClient(server_connection_handler_id, client_id, &channel_id) == ERROR_ok &&
            channel_id != 0) {
            speaker.channel_id = domain::ChannelId{channel_id};
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    media_speakers_[{server_connection_handler_id, client_id}] = speaker;
    return speaker;
}

auto PluginHostBackend::translate_error(unsigned int error_code, std::string_view fallback) const -> domain::Error {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!functions_.has_value()) {
        return backend_error("ts3_error", std::string(fallback));
    }
    char* raw_message = nullptr;
    if ((*functions_).getErrorMessage(error_code, &raw_message) == ERROR_ok && raw_message != nullptr) {
        Ts3Owned<char> message_owner(&*functions_, raw_message);
        return backend_error("ts3_error", std::string(raw_message), domain::ExitCode::sdk);
    }
    return backend_error("ts3_error", std::string(fallback) + " (" + std::to_string(error_code) + ")");
}

auto PluginHostBackend::current_self_id(std::uint64_t server_connection_handler_id) const
    -> domain::Result<std::uint16_t> {
    anyID self_id = 0;
    const unsigned int error = (*functions_).getClientID(server_connection_handler_id, &self_id);
    if (error != ERROR_ok) {
        return domain::fail<std::uint16_t>(translate_error(error, "failed to query TeamSpeak self client id"));
    }
    return domain::ok(static_cast<std::uint16_t>(self_id));
}

auto PluginHostBackend::connection_phase(std::uint64_t server_connection_handler_id) const
    -> domain::Result<domain::ConnectionPhase> {
    int status = STATUS_DISCONNECTED;
    const unsigned int error = (*functions_).getConnectionStatus(server_connection_handler_id, &status);
    if (error != ERROR_ok) {
        return domain::fail<domain::ConnectionPhase>(translate_error(error, "failed to query TeamSpeak connection status"));
    }
    if (status == STATUS_CONNECTION_ESTABLISHED || status == STATUS_CONNECTED) {
        return domain::ok(domain::ConnectionPhase::connected);
    }
    if (status == STATUS_CONNECTING || status == STATUS_CONNECTION_ESTABLISHING) {
        return domain::ok(domain::ConnectionPhase::connecting);
    }
    return domain::ok(domain::ConnectionPhase::disconnected);
}

auto PluginHostBackend::client_string(
    std::uint64_t server_connection_handler_id,
    std::uint16_t client_id,
    std::size_t flag
) const -> domain::Result<std::string> {
    char* raw_value = nullptr;
    const auto& functions = *functions_;
    const unsigned int error = functions.getClientVariableAsString(
        server_connection_handler_id, static_cast<anyID>(client_id), flag, &raw_value
    );
    if (error != ERROR_ok) {
        return domain::fail<std::string>(translate_error(error, "failed to query TeamSpeak client string"));
    }
    Ts3Owned<char> value_owner(&functions, raw_value);
    return domain::ok(raw_value == nullptr ? std::string{} : std::string(raw_value));
}

auto PluginHostBackend::client_bool(
    std::uint64_t server_connection_handler_id,
    std::uint16_t client_id,
    std::size_t flag
) const -> domain::Result<bool> {
    int raw_value = 0;
    const unsigned int error = (*functions_).getClientVariableAsInt(
        server_connection_handler_id, static_cast<anyID>(client_id), flag, &raw_value
    );
    if (error != ERROR_ok) {
        return domain::fail<bool>(translate_error(error, "failed to query TeamSpeak client flag"));
    }
    return domain::ok(raw_value != 0);
}

auto PluginHostBackend::channel_string(
    std::uint64_t server_connection_handler_id,
    std::uint64_t channel_id,
    std::size_t flag
) const -> domain::Result<std::string> {
    char* raw_value = nullptr;
    const auto& functions = *functions_;
    const unsigned int error = functions.getChannelVariableAsString(
        server_connection_handler_id, channel_id, flag, &raw_value
    );
    if (error != ERROR_ok) {
        return domain::fail<std::string>(translate_error(error, "failed to query TeamSpeak channel string"));
    }
    Ts3Owned<char> value_owner(&functions, raw_value);
    return domain::ok(raw_value == nullptr ? std::string{} : std::string(raw_value));
}

auto PluginHostBackend::channel_bool(
    std::uint64_t server_connection_handler_id,
    std::uint64_t channel_id,
    std::size_t flag
) const -> domain::Result<bool> {
    int raw_value = 0;
    const unsigned int error = (*functions_).getChannelVariableAsInt(
        server_connection_handler_id, channel_id, flag, &raw_value
    );
    if (error != ERROR_ok) {
        return domain::fail<bool>(translate_error(error, "failed to query TeamSpeak channel flag"));
    }
    return domain::ok(raw_value != 0);
}

auto PluginHostBackend::server_string(std::uint64_t server_connection_handler_id, std::size_t flag) const
    -> domain::Result<std::string> {
    char* raw_value = nullptr;
    const auto& functions = *functions_;
    const unsigned int error = functions.getServerVariableAsString(server_connection_handler_id, flag, &raw_value);
    if (error != ERROR_ok) {
        return domain::fail<std::string>(translate_error(error, "failed to query TeamSpeak server string"));
    }
    Ts3Owned<char> value_owner(&functions, raw_value);
    return domain::ok(raw_value == nullptr ? std::string{} : std::string(raw_value));
}

auto PluginHostBackend::connection_host(std::uint64_t server_connection_handler_id, std::uint16_t client_id) const
    -> domain::Result<std::string> {
    char* raw_value = nullptr;
    const auto& functions = *functions_;
    const unsigned int error = functions.getConnectionVariableAsString(
        server_connection_handler_id, static_cast<anyID>(client_id), CONNECTION_SERVER_IP, &raw_value
    );
    if (error != ERROR_ok) {
        return domain::fail<std::string>(translate_error(error, "failed to query TeamSpeak connection host"));
    }
    Ts3Owned<char> value_owner(&functions, raw_value);
    return domain::ok(raw_value == nullptr ? std::string{} : std::string(raw_value));
}

auto PluginHostBackend::connection_port(std::uint64_t server_connection_handler_id, std::uint16_t client_id) const
    -> domain::Result<std::uint16_t> {
    uint64_t raw_port = 0;
    const unsigned int error = (*functions_).getConnectionVariableAsUInt64(
        server_connection_handler_id, static_cast<anyID>(client_id), CONNECTION_SERVER_PORT, &raw_port
    );
    if (error != ERROR_ok) {
        return domain::fail<std::uint16_t>(translate_error(error, "failed to query TeamSpeak connection port"));
    }
    return domain::ok(static_cast<std::uint16_t>(raw_port));
}

auto PluginHostBackend::find_channel_by_selector(
    std::uint64_t server_connection_handler_id,
    const domain::Selector& selector
) const -> domain::Result<domain::Channel> {
    (void)server_connection_handler_id;
    auto channels = list_channels();
    if (!channels) {
        return domain::fail<domain::Channel>(channels.error());
    }
    const auto maybe_id = util::parse_u64(selector.raw);
    for (const auto& channel : channels.value()) {
        if ((maybe_id.has_value() && channel.id.value == *maybe_id) || util::iequals(channel.name, selector.raw)) {
            return domain::ok(channel);
        }
    }
    return domain::fail<domain::Channel>(backend_error(
        "channel_not_found", "channel not found: " + selector.raw, domain::ExitCode::not_found
    ));
}

auto PluginHostBackend::find_client_by_selector(
    std::uint64_t server_connection_handler_id,
    const domain::Selector& selector
) const -> domain::Result<domain::Client> {
    (void)server_connection_handler_id;
    auto clients = list_clients();
    if (!clients) {
        return domain::fail<domain::Client>(clients.error());
    }
    const auto maybe_id = util::parse_u64(selector.raw);
    for (const auto& client : clients.value()) {
        if ((maybe_id.has_value() && client.id.value == *maybe_id) || util::iequals(client.nickname, selector.raw)) {
            return domain::ok(client);
        }
    }
    return domain::fail<domain::Client>(backend_error(
        "client_not_found", "client not found: " + selector.raw, domain::ExitCode::not_found
    ));
}

}  // namespace teamspeak_cli::sdk
