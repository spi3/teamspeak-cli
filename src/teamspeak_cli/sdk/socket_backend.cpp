#include "teamspeak_cli/sdk/socket_backend.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "teamspeak_cli/bridge/media_bridge.hpp"
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

auto socket_timeout_error(
    std::string_view operation,
    std::string_view socket_path,
    std::string_view stage,
    std::chrono::milliseconds timeout
) -> domain::Error {
    std::string message = "Timed out while ";
    if (stage == "connect") {
        message += "contacting the TeamSpeak client plugin";
    } else if (stage == "write") {
        message += "sending a request to the TeamSpeak client plugin";
    } else {
        message += "waiting for the TeamSpeak client plugin to respond";
    }
    message += " for " + std::string(operation) + ".";

    auto error = bridge_error("socket_timeout", std::move(message));
    error.details.emplace("operation", operation);
    error.details.emplace("socket_path", socket_path);
    error.details.emplace("stage", stage);
    error.details.emplace("timeout_ms", std::to_string(timeout.count()));
    return error;
}

auto socket_io_error(
    std::string_view operation,
    std::string_view socket_path,
    std::string_view stage,
    int error_number,
    std::chrono::milliseconds timeout
) -> domain::Error {
    if (error_number == EAGAIN || error_number == EWOULDBLOCK) {
        return socket_timeout_error(operation, socket_path, stage, timeout);
    }

    std::string code = "socket_";
    code += stage;
    code += "_failed";

    std::string message = "failed to ";
    if (stage == "write") {
        message += "write request to";
    } else {
        message += "read reply from";
    }
    message += " plugin control socket";
    if (error_number != 0) {
        message += ": ";
        message += std::strerror(error_number);
    }

    auto error = bridge_error(std::move(code), std::move(message));
    error.details.emplace("operation", operation);
    error.details.emplace("socket_path", socket_path);
    error.details.emplace("stage", stage);
    if (error_number != 0) {
        error.details.emplace("os_error", std::string(std::strerror(error_number)));
    }
    return error;
}

auto normalized_timeout(std::chrono::milliseconds timeout) -> std::chrono::milliseconds {
    if (timeout <= std::chrono::milliseconds::zero()) {
        return std::chrono::milliseconds(1);
    }
    return timeout;
}

auto duration_to_poll_timeout(std::chrono::milliseconds timeout) -> int {
    const auto safe_timeout = normalized_timeout(timeout);
    constexpr auto max_poll_timeout = std::chrono::milliseconds(std::numeric_limits<int>::max());
    if (safe_timeout >= max_poll_timeout) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(safe_timeout.count());
}

auto duration_to_timeval(std::chrono::milliseconds timeout) -> timeval {
    const auto safe_timeout = normalized_timeout(timeout);
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(safe_timeout);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(safe_timeout - seconds);
    return timeval{
        .tv_sec = static_cast<decltype(timeval::tv_sec)>(seconds.count()),
        .tv_usec = static_cast<decltype(timeval::tv_usec)>(micros.count()),
    };
}

auto set_socket_timeout(int fd, int option_name, std::chrono::milliseconds timeout) -> domain::Result<void> {
    const auto value = duration_to_timeval(timeout);
    if (::setsockopt(fd, SOL_SOCKET, option_name, &value, sizeof(value)) != 0) {
        return domain::fail(bridge_error(
            "socket_option_failed",
            "failed to configure plugin control socket timeout: " + std::string(std::strerror(errno)),
            domain::ExitCode::internal
        ));
    }
    return domain::ok();
}

auto connect_with_timeout(
    int fd,
    const sockaddr_un& address,
    socklen_t address_size,
    std::string_view operation,
    std::string_view socket_path,
    std::chrono::milliseconds timeout
) -> domain::Result<void> {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return domain::fail(bridge_error(
            "socket_flags_failed",
            "failed to read plugin control socket flags: " + std::string(std::strerror(errno)),
            domain::ExitCode::internal
        ));
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return domain::fail(bridge_error(
            "socket_flags_failed",
            "failed to configure plugin control socket: " + std::string(std::strerror(errno)),
            domain::ExitCode::internal
        ));
    }

    const int connected = ::connect(fd, reinterpret_cast<const sockaddr*>(&address), address_size);
    if (connected == 0) {
        if (::fcntl(fd, F_SETFL, flags) != 0) {
            return domain::fail(bridge_error(
                "socket_flags_failed",
                "failed to restore plugin control socket flags: " + std::string(std::strerror(errno)),
                domain::ExitCode::internal
            ));
        }
        return domain::ok();
    }

    const int connect_errno = errno;
    if (connect_errno != EINPROGRESS && connect_errno != EAGAIN) {
        return domain::fail(socket_connect_error(operation, socket_path, connect_errno));
    }

    pollfd poll_state{
        .fd = fd,
        .events = POLLOUT,
        .revents = 0,
    };

    while (true) {
        const int ready = ::poll(&poll_state, 1, duration_to_poll_timeout(timeout));
        if (ready > 0) {
            int socket_error = 0;
            socklen_t socket_error_size = sizeof(socket_error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) != 0) {
                return domain::fail(bridge_error(
                    "socket_option_failed",
                    "failed to query plugin control socket state: " + std::string(std::strerror(errno)),
                    domain::ExitCode::internal
                ));
            }
            if (socket_error != 0) {
                return domain::fail(socket_connect_error(operation, socket_path, socket_error));
            }
            break;
        }
        if (ready == 0) {
            return domain::fail(socket_timeout_error(operation, socket_path, "connect", timeout));
        }
        if (errno == EINTR) {
            continue;
        }
        return domain::fail(socket_connect_error(operation, socket_path, errno));
    }

    if (::fcntl(fd, F_SETFL, flags) != 0) {
        return domain::fail(bridge_error(
            "socket_flags_failed",
            "failed to restore plugin control socket flags: " + std::string(std::strerror(errno)),
            domain::ExitCode::internal
        ));
    }
    return domain::ok();
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
    },
                             command_timeout());
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
    },
                             command_timeout());
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
    auto response = exchange(
        "check TeamSpeak plugin status",
        {std::string(bridge::protocol::kMagic), "plugin_info"},
        command_timeout()
    );
    if (!response) {
        if (response.error().code == "socket_connect_failed") {
            return domain::ok(domain::PluginInfo{
                .backend = "plugin",
                .transport = "unix-socket",
                .plugin_name = "ts3cli-plugin",
                .plugin_version = "unavailable",
                .plugin_available = false,
                .socket_path = socket_path_,
                .media_transport = "unix-stream/frame-v1",
                .media_socket_path = bridge::resolve_media_socket_path(socket_path_),
                .media_format = bridge::media_format_description(),
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
    auto response = exchange(
        "read TeamSpeak status",
        {std::string(bridge::protocol::kMagic), "connection_state"},
        command_timeout()
    );
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
    auto response = exchange(
        "read TeamSpeak server info",
        {std::string(bridge::protocol::kMagic), "server_info"},
        command_timeout()
    );
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
    auto response = exchange(
        "list TeamSpeak channels",
        {std::string(bridge::protocol::kMagic), "list_channels"},
        command_timeout()
    );
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
    auto response = exchange(
        "list TeamSpeak clients",
        {std::string(bridge::protocol::kMagic), "list_clients"},
        command_timeout()
    );
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
    },
                             command_timeout());
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
    },
                             command_timeout());
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
    },
                             command_timeout());
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
    },
                             command_timeout());
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
    },
                             std::max(command_timeout(), timeout + std::chrono::seconds(1)));
    if (!response) {
        return domain::fail<std::optional<domain::Event>>(response.error());
    }
    auto type_check = expect_type(response.value(), "event");
    if (!type_check) {
        return domain::fail<std::optional<domain::Event>>(type_check.error());
    }
    return bridge::protocol::decode_event(response.value().payload);
}

auto SocketBackend::exchange(
    std::string_view operation,
    const bridge::protocol::Fields& request,
    std::chrono::milliseconds timeout
) const
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

    auto connected = connect_with_timeout(
        fd.get(), address, sizeof(address), operation, socket_path_, timeout
    );
    if (!connected) {
        return domain::fail<bridge::protocol::Response>(connected.error());
    }

    auto write_timeout = set_socket_timeout(fd.get(), SO_SNDTIMEO, timeout);
    if (!write_timeout) {
        return domain::fail<bridge::protocol::Response>(write_timeout.error());
    }
    auto read_timeout = set_socket_timeout(fd.get(), SO_RCVTIMEO, timeout);
    if (!read_timeout) {
        return domain::fail<bridge::protocol::Response>(read_timeout.error());
    }

    errno = 0;
    if (!bridge::protocol::write_line(fd.get(), request)) {
        return domain::fail<bridge::protocol::Response>(
            socket_io_error(operation, socket_path_, "write", errno, timeout)
        );
    }

    std::string line;
    errno = 0;
    if (!bridge::protocol::read_line(fd.get(), line)) {
        return domain::fail<bridge::protocol::Response>(
            socket_io_error(operation, socket_path_, "read", errno, timeout)
        );
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
    while (true) {
        errno = 0;
        if (!bridge::protocol::read_line(fd.get(), line)) {
            break;
        }
        const auto fields = bridge::protocol::split_fields(line);
        if (fields.size() == 1 && fields[0] == "end") {
            return domain::ok(std::move(response));
        }
        response.payload.push_back(fields);
    }

    if (errno == 0) {
        return domain::fail<bridge::protocol::Response>(bridge_error(
            "socket_read_failed", "bridge response ended unexpectedly"
        ));
    }
    return domain::fail<bridge::protocol::Response>(
        socket_io_error(operation, socket_path_, "read", errno, timeout)
    );
}

auto SocketBackend::require_initialized() const -> domain::Result<void> {
    if (!initialized_) {
        return domain::fail(bridge_error(
            "not_initialized", "plugin backend is not initialized", domain::ExitCode::internal
        ));
    }
    return domain::ok();
}

auto SocketBackend::command_timeout() const -> std::chrono::milliseconds {
    return normalized_timeout(options_.command_timeout);
}

}  // namespace teamspeak_cli::sdk
