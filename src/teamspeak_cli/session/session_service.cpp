#include "teamspeak_cli/session/session_service.hpp"

namespace teamspeak_cli::session {
namespace {

constexpr auto kConnectPollInterval = std::chrono::milliseconds(250);

auto is_connection_lifecycle_event(const domain::Event& event) -> bool {
    return event.type.rfind("connection.", 0) == 0 || event.type == "server.error";
}

auto has_terminal_connection_failure(const std::vector<domain::Event>& lifecycle) -> bool {
    for (const auto& event : lifecycle) {
        if (event.type == "connection.error" || event.type == "connection.disconnected" ||
            event.type == "server.error") {
            return true;
        }
    }
    return false;
}

auto current_connection_state(sdk::Backend& backend) -> domain::Result<domain::ConnectionState> {
    auto state = backend.connection_state();
    if (!state) {
        return domain::fail<domain::ConnectionState>(state.error());
    }
    return domain::ok(state.value());
}

auto collect_connection_events(
    sdk::Backend& backend,
    std::vector<domain::Event>& lifecycle,
    const ConnectEventCallback& on_event = {}
) -> domain::Result<void> {
    while (true) {
        auto next = backend.next_event(std::chrono::milliseconds::zero());
        if (!next) {
            return domain::fail(next.error());
        }
        if (!next.value().has_value()) {
            return domain::ok();
        }
        if (is_connection_lifecycle_event(*next.value())) {
            lifecycle.push_back(std::move(*next.value()));
            if (on_event) {
                on_event(lifecycle.back());
            }
        }
    }
}

}  // namespace

SessionService::SessionService(std::unique_ptr<sdk::Backend> backend) : backend_(std::move(backend)) {}

auto SessionService::initialize(const sdk::InitOptions& options) -> domain::Result<void> {
    return backend_->initialize(options);
}

auto SessionService::shutdown() -> domain::Result<void> {
    return backend_->shutdown();
}

auto SessionService::connect(const sdk::ConnectRequest& request) -> domain::Result<void> {
    return backend_->connect(request);
}

auto SessionService::connect_and_wait(
    const sdk::ConnectRequest& request,
    std::chrono::milliseconds timeout,
    ConnectEventCallback on_event
)
    -> domain::Result<ConnectResult> {
    ConnectResult result{
        .state = domain::ConnectionState{},
        .lifecycle = {},
        .connected = false,
        .timed_out = false,
        .timeout = timeout,
    };

    auto cleared = collect_connection_events(*backend_, result.lifecycle);
    if (!cleared) {
        return domain::fail<ConnectResult>(cleared.error());
    }
    result.lifecycle.clear();

    auto connected = backend_->connect(request);
    if (!connected) {
        return domain::fail<ConnectResult>(connected.error());
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        auto drained = collect_connection_events(*backend_, result.lifecycle, on_event);
        if (!drained) {
            return domain::fail<ConnectResult>(drained.error());
        }

        auto state = backend_->connection_state();
        if (!state) {
            return domain::fail<ConnectResult>(state.error());
        }
        result.state = state.value();

        if (result.state.phase == domain::ConnectionPhase::connected) {
            result.connected = true;
            break;
        }

        if (result.state.phase == domain::ConnectionPhase::disconnected &&
            has_terminal_connection_failure(result.lifecycle)) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            result.timed_out = true;
            break;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        auto next = backend_->next_event(std::min(kConnectPollInterval, remaining));
        if (!next) {
            return domain::fail<ConnectResult>(next.error());
        }
        if (next.value().has_value() && is_connection_lifecycle_event(*next.value())) {
            result.lifecycle.push_back(std::move(*next.value()));
            if (on_event) {
                on_event(result.lifecycle.back());
            }
        }
    }

    auto final_events = collect_connection_events(*backend_, result.lifecycle, on_event);
    if (!final_events) {
        return domain::fail<ConnectResult>(final_events.error());
    }

    return domain::ok(std::move(result));
}

auto SessionService::disconnect(std::string_view reason) -> domain::Result<void> {
    return backend_->disconnect(reason);
}

auto SessionService::disconnect_and_wait(
    std::string_view reason,
    std::chrono::milliseconds timeout,
    ConnectEventCallback on_event
) -> domain::Result<DisconnectResult> {
    DisconnectResult result{
        .state = domain::ConnectionState{},
        .lifecycle = {},
        .disconnected = false,
        .timed_out = false,
        .timeout = timeout,
    };

    auto cleared = collect_connection_events(*backend_, result.lifecycle);
    if (!cleared) {
        return domain::fail<DisconnectResult>(cleared.error());
    }
    result.lifecycle.clear();

    auto disconnected = backend_->disconnect(reason);
    if (!disconnected) {
        return domain::fail<DisconnectResult>(disconnected.error());
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        auto drained = collect_connection_events(*backend_, result.lifecycle, on_event);
        if (!drained) {
            return domain::fail<DisconnectResult>(drained.error());
        }

        auto state = current_connection_state(*backend_);
        if (!state) {
            return domain::fail<DisconnectResult>(state.error());
        }
        result.state = state.value();

        if (result.state.phase == domain::ConnectionPhase::disconnected) {
            result.disconnected = true;
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            result.timed_out = true;
            break;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        auto next = backend_->next_event(std::min(kConnectPollInterval, remaining));
        if (!next) {
            return domain::fail<DisconnectResult>(next.error());
        }
        if (next.value().has_value() && is_connection_lifecycle_event(*next.value())) {
            result.lifecycle.push_back(std::move(*next.value()));
            if (on_event) {
                on_event(result.lifecycle.back());
            }
        }
    }

    auto final_events = collect_connection_events(*backend_, result.lifecycle, on_event);
    if (!final_events) {
        return domain::fail<DisconnectResult>(final_events.error());
    }

    return domain::ok(std::move(result));
}

auto SessionService::plugin_info() const -> domain::Result<domain::PluginInfo> {
    return backend_->plugin_info();
}

auto SessionService::connection_state() const -> domain::Result<domain::ConnectionState> {
    return backend_->connection_state();
}

auto SessionService::server_info() const -> domain::Result<domain::ServerInfo> {
    return backend_->server_info();
}

auto SessionService::list_channels() const -> domain::Result<std::vector<domain::Channel>> {
    return backend_->list_channels();
}

auto SessionService::list_clients() const -> domain::Result<std::vector<domain::Client>> {
    return backend_->list_clients();
}

auto SessionService::get_channel(const domain::Selector& selector) const -> domain::Result<domain::Channel> {
    return backend_->get_channel(selector);
}

auto SessionService::get_client(const domain::Selector& selector) const -> domain::Result<domain::Client> {
    return backend_->get_client(selector);
}

auto SessionService::join_channel(const domain::Selector& selector) -> domain::Result<void> {
    return backend_->join_channel(selector);
}

auto SessionService::send_message(const domain::MessageRequest& request) -> domain::Result<void> {
    return backend_->send_message(request);
}

auto SessionService::watch_events(std::size_t count, std::chrono::milliseconds timeout)
    -> domain::Result<std::vector<domain::Event>> {
    std::vector<domain::Event> events;
    events.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        auto next = backend_->next_event(timeout);
        if (!next) {
            return domain::fail<std::vector<domain::Event>>(next.error());
        }
        if (!next.value().has_value()) {
            break;
        }
        events.push_back(std::move(*next.value()));
    }
    return domain::ok(std::move(events));
}

}  // namespace teamspeak_cli::session
