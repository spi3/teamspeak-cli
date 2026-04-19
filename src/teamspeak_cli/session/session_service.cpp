#include "teamspeak_cli/session/session_service.hpp"

namespace teamspeak_cli::session {

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

auto SessionService::disconnect(std::string_view reason) -> domain::Result<void> {
    return backend_->disconnect(reason);
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
