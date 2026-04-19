#include "teamspeak_cli/sdk/fake_backend.hpp"

#include <chrono>

#include "teamspeak_cli/bridge/socket_paths.hpp"
#include "teamspeak_cli/util/strings.hpp"

namespace teamspeak_cli::sdk {
namespace {

auto sdk_error(std::string code, std::string message, domain::ExitCode exit_code = domain::ExitCode::sdk)
    -> domain::Error {
    return domain::make_error("sdk", std::move(code), std::move(message), exit_code);
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

}  // namespace

FakeBackend::FakeBackend() {
    channels_ = {
        domain::Channel{.id = {1}, .name = "Lobby", .parent_id = std::nullopt, .client_count = 1, .is_default = true},
        domain::Channel{.id = {2}, .name = "Engineering", .parent_id = std::nullopt, .client_count = 2},
        domain::Channel{.id = {3}, .name = "Operations", .parent_id = std::nullopt, .client_count = 1},
        domain::Channel{.id = {4}, .name = "Breakout", .parent_id = domain::ChannelId{2}, .client_count = 0},
    };

    clients_ = {
        domain::Client{.id = {1}, .nickname = "terminal", .unique_identity = "built-test-identity", .channel_id = domain::ChannelId{1}, .self = true},
        domain::Client{.id = {2}, .nickname = "alice", .unique_identity = "sdk-alice", .channel_id = domain::ChannelId{2}},
        domain::Client{.id = {3}, .nickname = "bob", .unique_identity = "sdk-bob", .channel_id = domain::ChannelId{2}, .talking = true},
        domain::Client{.id = {4}, .nickname = "ops-bot", .unique_identity = "sdk-ops-bot", .channel_id = domain::ChannelId{3}},
    };

    state_ = domain::ConnectionState{
        .phase = domain::ConnectionPhase::disconnected,
        .backend = "fake",
        .connection = {0},
        .server = "127.0.0.1",
        .port = 9987,
        .nickname = "terminal",
        .identity = "built-test-identity",
        .profile = "built-test",
        .mode = "one-shot",
    };

    server_ = domain::ServerInfo{
        .name = "Fake TeamSpeak Server",
        .host = "127.0.0.1",
        .port = 9987,
        .backend = "fake",
        .current_channel = domain::ChannelId{1},
        .channel_count = channels_.size(),
        .client_count = clients_.size(),
    };
}

FakeBackend::~FakeBackend() {
    stop_event_loop();
}

auto FakeBackend::kind() const -> std::string {
    return "fake";
}

auto FakeBackend::initialize(const InitOptions& options) -> domain::Result<void> {
    std::lock_guard<std::mutex> lock(mutex_);
    options_ = options;
    initialized_ = true;
    state_.phase = domain::ConnectionPhase::connected;
    state_.connection = {42};
    start_event_loop();
    return domain::ok();
}

auto FakeBackend::shutdown() -> domain::Result<void> {
    stop_event_loop();
    std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = false;
    state_.phase = domain::ConnectionPhase::disconnected;
    state_.connection = {0};
    return domain::ok();
}

auto FakeBackend::connect(const ConnectRequest& request) -> domain::Result<void> {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return domain::fail(sdk_error("not_initialized", "backend is not initialized"));
    }

    state_.phase = domain::ConnectionPhase::connected;
    state_.connection = {42};
    state_.server = request.host;
    state_.port = request.port;
    state_.nickname = request.nickname.empty() ? "terminal" : request.nickname;
    state_.identity = request.identity.empty() ? "fake-generated-identity" : request.identity;
    state_.profile = request.profile_name;

    server_.host = request.host;
    server_.port = request.port;
    server_.current_channel = domain::ChannelId{1};
    server_.channel_count = channels_.size();
    server_.client_count = clients_.size();

    clients_[0].nickname = state_.nickname;
    clients_[0].unique_identity = state_.identity;
    clients_[0].channel_id = domain::ChannelId{1};

    events_.push(now_event(
        "connection.connected",
        "connected to fake TeamSpeak server",
        {{"server", request.host}, {"port", std::to_string(request.port)}}
    ));

    stop_event_loop();
    start_event_loop();
    return domain::ok();
}

auto FakeBackend::disconnect(std::string_view reason) -> domain::Result<void> {
    stop_event_loop();

    std::lock_guard<std::mutex> lock(mutex_);
    state_.phase = domain::ConnectionPhase::disconnected;
    state_.connection = {0};
    events_.push(now_event("connection.disconnected", std::string(reason)));
    return domain::ok();
}

auto FakeBackend::plugin_info() const -> domain::Result<domain::PluginInfo> {
    std::lock_guard<std::mutex> lock(mutex_);
    return domain::ok(domain::PluginInfo{
        .backend = "fake",
        .transport = "in-process",
        .plugin_name = "fake-plugin-host",
        .plugin_version = "development",
        .plugin_available = true,
        .socket_path = bridge::resolve_socket_path(options_.socket_path),
        .note = "built-test plugin host for local development and CI",
    });
}

auto FakeBackend::connection_state() const -> domain::Result<domain::ConnectionState> {
    std::lock_guard<std::mutex> lock(mutex_);
    return domain::ok(state_);
}

auto FakeBackend::server_info() const -> domain::Result<domain::ServerInfo> {
    const auto connected = require_connected();
    if (!connected) {
        return domain::fail<domain::ServerInfo>(connected.error());
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return domain::ok(server_);
}

auto FakeBackend::list_channels() const -> domain::Result<std::vector<domain::Channel>> {
    const auto connected = require_connected();
    if (!connected) {
        return domain::fail<std::vector<domain::Channel>>(connected.error());
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return domain::ok(channels_);
}

auto FakeBackend::list_clients() const -> domain::Result<std::vector<domain::Client>> {
    const auto connected = require_connected();
    if (!connected) {
        return domain::fail<std::vector<domain::Client>>(connected.error());
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return domain::ok(clients_);
}

auto FakeBackend::get_channel(const domain::Selector& selector) const -> domain::Result<domain::Channel> {
    return find_channel(selector);
}

auto FakeBackend::get_client(const domain::Selector& selector) const -> domain::Result<domain::Client> {
    return find_client(selector);
}

auto FakeBackend::join_channel(const domain::Selector& selector) -> domain::Result<void> {
    const auto channel_result = find_channel(selector);
    if (!channel_result) {
        return domain::fail(channel_result.error());
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& channel : channels_) {
        if (channel.id == clients_[0].channel_id.value_or(domain::ChannelId{})) {
            if (channel.client_count > 0) {
                --channel.client_count;
            }
        }
        if (channel.id == channel_result.value().id) {
            ++channel.client_count;
        }
    }
    clients_[0].channel_id = channel_result.value().id;
    server_.current_channel = channel_result.value().id;

    events_.push(now_event(
        "channel.joined",
        "joined channel " + channel_result.value().name,
        {{"channel_id", domain::to_string(channel_result.value().id)}, {"channel_name", channel_result.value().name}}
    ));
    return domain::ok();
}

auto FakeBackend::send_message(const domain::MessageRequest& request) -> domain::Result<void> {
    const auto connected = require_connected();
    if (!connected) {
        return domain::fail(connected.error());
    }

    events_.push(now_event(
        "message.sent",
        "sent message to " + domain::to_string(request.target_kind) + " " + request.target,
        {{"target_kind", domain::to_string(request.target_kind)}, {"target", request.target}, {"text", request.text}}
    ));
    return domain::ok();
}

auto FakeBackend::next_event(std::chrono::milliseconds timeout)
    -> domain::Result<std::optional<domain::Event>> {
    return domain::ok(events_.pop_for(timeout));
}

auto FakeBackend::require_connected() const -> domain::Result<void> {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.phase != domain::ConnectionPhase::connected) {
        return domain::fail(sdk_error(
            "not_connected", "no live TeamSpeak session", domain::ExitCode::connection
        ));
    }
    return domain::ok();
}

auto FakeBackend::find_channel(const domain::Selector& selector) const -> domain::Result<domain::Channel> {
    const auto connected = require_connected();
    if (!connected) {
        return domain::fail<domain::Channel>(connected.error());
    }

    const auto maybe_id = util::parse_u64(selector.raw);
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& channel : channels_) {
        if ((maybe_id.has_value() && channel.id.value == *maybe_id) ||
            util::iequals(channel.name, selector.raw)) {
            return domain::ok(channel);
        }
    }
    return domain::fail<domain::Channel>(sdk_error(
        "channel_not_found", "channel not found: " + selector.raw, domain::ExitCode::not_found
    ));
}

auto FakeBackend::find_client(const domain::Selector& selector) const -> domain::Result<domain::Client> {
    const auto connected = require_connected();
    if (!connected) {
        return domain::fail<domain::Client>(connected.error());
    }

    const auto maybe_id = util::parse_u64(selector.raw);
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& client : clients_) {
        if ((maybe_id.has_value() && client.id.value == *maybe_id) ||
            util::iequals(client.nickname, selector.raw)) {
            return domain::ok(client);
        }
    }
    return domain::fail<domain::Client>(sdk_error(
        "client_not_found", "client not found: " + selector.raw, domain::ExitCode::not_found
    ));
}

void FakeBackend::start_event_loop() {
    event_thread_ = std::jthread([this](std::stop_token stop_token) {
        std::size_t tick = 0;
        while (!stop_token.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const std::size_t slot = tick % 4;
            if (slot == 0) {
                events_.push(now_event(
                    "channel.updated",
                    "Engineering activity increased",
                    {{"channel_id", "2"}, {"client_count", "2"}}
                ));
            } else if (slot == 1) {
                events_.push(now_event(
                    "client.talking",
                    "bob started talking",
                    {{"client_id", "3"}, {"nickname", "bob"}}
                ));
            } else if (slot == 2) {
                events_.push(now_event(
                    "message.received",
                    "received channel text message",
                    {{"from", "alice"}, {"channel_id", "2"}, {"text", "build green"}}
                ));
            } else {
                events_.push(now_event(
                    "heartbeat",
                    "built-test backend event heartbeat",
                    {{"backend", "fake"}}
                ));
            }
            ++tick;
        }
    });
}

void FakeBackend::stop_event_loop() {
    if (event_thread_.joinable()) {
        event_thread_.request_stop();
        event_thread_.join();
    }
}

}  // namespace teamspeak_cli::sdk
