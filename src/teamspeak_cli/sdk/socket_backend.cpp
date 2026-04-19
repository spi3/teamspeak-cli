#include "teamspeak_cli/sdk/socket_backend.hpp"

#include <cerrno>
#include <cstring>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "teamspeak_cli/bridge/protocol.hpp"
#include "teamspeak_cli/bridge/socket_paths.hpp"

namespace teamspeak_cli::sdk {
namespace {

auto bridge_error(std::string code, std::string message, domain::ExitCode exit_code = domain::ExitCode::connection)
    -> domain::Error {
    return domain::make_error("bridge", std::move(code), std::move(message), exit_code);
}

auto socket_connect_error(std::string_view operation, std::string_view socket_path, int error_number)
    -> domain::Error {
    auto error = bridge_error(
        "socket_connect_failed",
        "Unable to " + std::string(operation) +
            " because the TeamSpeak client is not running or the ts3cli plugin is unavailable."
    );
    error.details.emplace("operation", operation);
    error.details.emplace("socket_path", socket_path);
    error.details.emplace("os_error", std::string(std::strerror(error_number)));
    return error;
}

class FileDescriptor {
  public:
    explicit FileDescriptor(int fd = -1) : fd_(fd) {}
    ~FileDescriptor() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    [[nodiscard]] auto get() const -> int { return fd_; }

  private:
    int fd_;
};

auto expect_type(
    const bridge::protocol::Response& response,
    std::string_view expected
) -> domain::Result<void> {
    if (response.type != expected) {
        return domain::fail(bridge_error(
            "unexpected_response",
            "bridge returned " + response.type + " for " + std::string(expected),
            domain::ExitCode::internal
        ));
    }
    return domain::ok();
}

}  // namespace

auto SocketBackend::kind() const -> std::string {
    return "plugin";
}

auto SocketBackend::initialize(const InitOptions& options) -> domain::Result<void> {
    options_ = options;
    socket_path_ = bridge::resolve_socket_path(options.socket_path);
    initialized_ = true;
    return domain::ok();
}

auto SocketBackend::shutdown() -> domain::Result<void> {
    initialized_ = false;
    return domain::ok();
}

auto SocketBackend::connect(const ConnectRequest& request) -> domain::Result<void> {
    auto ready = require_initialized();
    if (!ready) {
        return ready;
    }
    auto response = exchange("ask TeamSpeak to connect", {
        std::string(bridge::protocol::kMagic),
        "connect",
        bridge::protocol::hex_encode(request.host),
        std::to_string(request.port),
        bridge::protocol::hex_encode(request.nickname),
        bridge::protocol::hex_encode(request.identity),
        bridge::protocol::hex_encode(request.server_password),
        bridge::protocol::hex_encode(request.channel_password),
        bridge::protocol::hex_encode(request.default_channel),
        bridge::protocol::hex_encode(request.profile_name),
    });
    if (!response) {
        return domain::fail(response.error());
    }
    return expect_type(response.value(), "void");
}

auto SocketBackend::disconnect(std::string_view reason) -> domain::Result<void> {
    auto ready = require_initialized();
    if (!ready) {
        return ready;
    }
    auto response = exchange("ask TeamSpeak to disconnect", {
        std::string(bridge::protocol::kMagic),
        "disconnect",
        bridge::protocol::hex_encode(reason),
    });
    if (!response) {
        return domain::fail(response.error());
    }
    return expect_type(response.value(), "void");
}

auto SocketBackend::plugin_info() const -> domain::Result<domain::PluginInfo> {
    auto ready = require_initialized();
    if (!ready) {
        return domain::fail<domain::PluginInfo>(ready.error());
    }
    auto response = exchange("check TeamSpeak plugin status", {std::string(bridge::protocol::kMagic), "plugin_info"});
    if (!response) {
        if (response.error().code == "socket_connect_failed") {
            return domain::ok(domain::PluginInfo{
                .backend = "plugin",
                .transport = "unix-socket",
                .plugin_name = "ts3cli-plugin",
                .plugin_version = "unavailable",
                .plugin_available = false,
                .socket_path = socket_path_,
                .note = response.error().message,
            });
        }
        return domain::fail<domain::PluginInfo>(response.error());
    }
    auto type_check = expect_type(response.value(), "plugin_info");
    if (!type_check) {
        return domain::fail<domain::PluginInfo>(type_check.error());
    }
    return bridge::protocol::decode_plugin_info(response.value().payload);
}

auto SocketBackend::connection_state() const -> domain::Result<domain::ConnectionState> {
    auto response = exchange("read TeamSpeak status", {std::string(bridge::protocol::kMagic), "connection_state"});
    if (!response) {
        return domain::fail<domain::ConnectionState>(response.error());
    }
    auto type_check = expect_type(response.value(), "connection_state");
    if (!type_check) {
        return domain::fail<domain::ConnectionState>(type_check.error());
    }
    return bridge::protocol::decode_connection_state(response.value().payload);
}

auto SocketBackend::server_info() const -> domain::Result<domain::ServerInfo> {
    auto response = exchange("read TeamSpeak server info", {std::string(bridge::protocol::kMagic), "server_info"});
    if (!response) {
        return domain::fail<domain::ServerInfo>(response.error());
    }
    auto type_check = expect_type(response.value(), "server_info");
    if (!type_check) {
        return domain::fail<domain::ServerInfo>(type_check.error());
    }
    return bridge::protocol::decode_server_info(response.value().payload);
}

auto SocketBackend::list_channels() const -> domain::Result<std::vector<domain::Channel>> {
    auto response = exchange("list TeamSpeak channels", {std::string(bridge::protocol::kMagic), "list_channels"});
    if (!response) {
        return domain::fail<std::vector<domain::Channel>>(response.error());
    }
    auto type_check = expect_type(response.value(), "channels");
    if (!type_check) {
        return domain::fail<std::vector<domain::Channel>>(type_check.error());
    }
    return bridge::protocol::decode_channels(response.value().payload);
}

auto SocketBackend::list_clients() const -> domain::Result<std::vector<domain::Client>> {
    auto response = exchange("list TeamSpeak clients", {std::string(bridge::protocol::kMagic), "list_clients"});
    if (!response) {
        return domain::fail<std::vector<domain::Client>>(response.error());
    }
    auto type_check = expect_type(response.value(), "clients");
    if (!type_check) {
        return domain::fail<std::vector<domain::Client>>(type_check.error());
    }
    return bridge::protocol::decode_clients(response.value().payload);
}

auto SocketBackend::get_channel(const domain::Selector& selector) const -> domain::Result<domain::Channel> {
    auto response = exchange("read TeamSpeak channel details", {
        std::string(bridge::protocol::kMagic),
        "get_channel",
        bridge::protocol::hex_encode(selector.raw),
    });
    if (!response) {
        return domain::fail<domain::Channel>(response.error());
    }
    auto type_check = expect_type(response.value(), "channel");
    if (!type_check) {
        return domain::fail<domain::Channel>(type_check.error());
    }
    auto decoded = bridge::protocol::decode_channels(response.value().payload);
    if (!decoded) {
        return domain::fail<domain::Channel>(decoded.error());
    }
    return domain::ok(decoded.value().front());
}

auto SocketBackend::get_client(const domain::Selector& selector) const -> domain::Result<domain::Client> {
    auto response = exchange("read TeamSpeak client details", {
        std::string(bridge::protocol::kMagic),
        "get_client",
        bridge::protocol::hex_encode(selector.raw),
    });
    if (!response) {
        return domain::fail<domain::Client>(response.error());
    }
    auto type_check = expect_type(response.value(), "client");
    if (!type_check) {
        return domain::fail<domain::Client>(type_check.error());
    }
    auto decoded = bridge::protocol::decode_clients(response.value().payload);
    if (!decoded) {
        return domain::fail<domain::Client>(decoded.error());
    }
    return domain::ok(decoded.value().front());
}

auto SocketBackend::join_channel(const domain::Selector& selector) -> domain::Result<void> {
    auto response = exchange("join the TeamSpeak channel", {
        std::string(bridge::protocol::kMagic),
        "join_channel",
        bridge::protocol::hex_encode(selector.raw),
    });
    if (!response) {
        return domain::fail(response.error());
    }
    return expect_type(response.value(), "void");
}

auto SocketBackend::send_message(const domain::MessageRequest& request) -> domain::Result<void> {
    auto response = exchange("send the TeamSpeak message", {
        std::string(bridge::protocol::kMagic),
        "send_message",
        domain::to_string(request.target_kind),
        bridge::protocol::hex_encode(request.target),
        bridge::protocol::hex_encode(request.text),
    });
    if (!response) {
        return domain::fail(response.error());
    }
    return expect_type(response.value(), "void");
}

auto SocketBackend::next_event(std::chrono::milliseconds timeout)
    -> domain::Result<std::optional<domain::Event>> {
    auto response = exchange("read TeamSpeak events", {
        std::string(bridge::protocol::kMagic),
        "next_event",
        std::to_string(timeout.count()),
    });
    if (!response) {
        return domain::fail<std::optional<domain::Event>>(response.error());
    }
    auto type_check = expect_type(response.value(), "event");
    if (!type_check) {
        return domain::fail<std::optional<domain::Event>>(type_check.error());
    }
    return bridge::protocol::decode_event(response.value().payload);
}

auto SocketBackend::exchange(std::string_view operation, const bridge::protocol::Fields& request) const
    -> domain::Result<bridge::protocol::Response> {
    auto ready = require_initialized();
    if (!ready) {
        return domain::fail<bridge::protocol::Response>(ready.error());
    }

    FileDescriptor fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (fd.get() < 0) {
        return domain::fail<bridge::protocol::Response>(bridge_error(
            "socket_failed", "failed to create control socket: " + std::string(std::strerror(errno))
        ));
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(address.sun_path)) {
        return domain::fail<bridge::protocol::Response>(bridge_error(
            "socket_path_too_long", "control socket path exceeds platform limit", domain::ExitCode::config
        ));
    }
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path_.c_str());

    if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        const int error_number = errno;
        return domain::fail<bridge::protocol::Response>(
            socket_connect_error(operation, socket_path_, error_number)
        );
    }

    if (!bridge::protocol::write_line(fd.get(), request)) {
        return domain::fail<bridge::protocol::Response>(bridge_error(
            "socket_write_failed", "failed to write request to plugin control socket"
        ));
    }

    std::string line;
    if (!bridge::protocol::read_line(fd.get(), line)) {
        return domain::fail<bridge::protocol::Response>(bridge_error(
            "socket_read_failed", "failed to read reply from plugin control socket"
        ));
    }
    const auto header = bridge::protocol::split_fields(line);
    if (header.empty()) {
        return domain::fail<bridge::protocol::Response>(bridge_error(
            "invalid_response", "empty bridge response"
        ));
    }
    if (header[0] == "error") {
        return domain::fail<bridge::protocol::Response>(bridge::protocol::decode_error(header));
    }
    if (header.size() != 2 || header[0] != "ok") {
        return domain::fail<bridge::protocol::Response>(bridge_error(
            "invalid_response", "invalid bridge response header"
        ));
    }

    bridge::protocol::Response response{.type = header[1], .payload = {}};
    while (bridge::protocol::read_line(fd.get(), line)) {
        const auto fields = bridge::protocol::split_fields(line);
        if (fields.size() == 1 && fields[0] == "end") {
            return domain::ok(std::move(response));
        }
        response.payload.push_back(fields);
    }

    return domain::fail<bridge::protocol::Response>(bridge_error(
        "socket_read_failed", "bridge response ended unexpectedly"
    ));
}

auto SocketBackend::require_initialized() const -> domain::Result<void> {
    if (!initialized_) {
        return domain::fail(bridge_error(
            "not_initialized", "plugin backend is not initialized", domain::ExitCode::internal
        ));
    }
    return domain::ok();
}

}  // namespace teamspeak_cli::sdk
