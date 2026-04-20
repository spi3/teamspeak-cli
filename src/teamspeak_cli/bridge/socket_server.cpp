#include "teamspeak_cli/bridge/socket_server.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "teamspeak_cli/bridge/protocol.hpp"
#include "teamspeak_cli/bridge/socket_paths.hpp"
#include "teamspeak_cli/util/strings.hpp"

namespace teamspeak_cli::bridge {
namespace {

auto bridge_error(std::string code, std::string message, domain::ExitCode exit_code = domain::ExitCode::connection)
    -> domain::Error {
    return domain::make_error("bridge", std::move(code), std::move(message), exit_code);
}

class FileDescriptor {
  public:
    explicit FileDescriptor(int fd = -1) : fd_(fd) {}
    ~FileDescriptor() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    auto operator=(const FileDescriptor&) -> FileDescriptor& = delete;

    FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    auto operator=(FileDescriptor&& other) noexcept -> FileDescriptor& {
        if (this != &other) {
            if (fd_ >= 0) {
                ::close(fd_);
            }
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    [[nodiscard]] auto get() const -> int { return fd_; }
    [[nodiscard]] auto release() -> int {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

  private:
    int fd_;
};

auto unlink_socket_file(const std::string& path) -> void {
    if (path.empty()) {
        return;
    }

    struct stat file_stat {};
    if (::lstat(path.c_str(), &file_stat) != 0) {
        return;
    }

    if (S_ISSOCK(file_stat.st_mode)) {
        ::unlink(path.c_str());
    }
}

auto send_error(int fd, const domain::Error& error) -> void {
    protocol::write_line(fd, protocol::encode_error(error));
    protocol::write_line(fd, {"end"});
}

auto send_ok(int fd, std::string_view type, const std::vector<protocol::Fields>& payload) -> void {
    protocol::write_line(fd, {"ok", std::string(type)});
    for (const auto& line : payload) {
        protocol::write_line(fd, line);
    }
    protocol::write_line(fd, {"end"});
}

auto decode_hex_arg(const protocol::Fields& fields, std::size_t index, std::string_view field_name)
    -> domain::Result<std::string> {
    if (index >= fields.size()) {
        return domain::fail<std::string>(bridge_error(
            "missing_argument", "missing bridge argument: " + std::string(field_name), domain::ExitCode::usage
        ));
    }
    return protocol::hex_decode(fields[index]);
}

auto decode_u64_arg(const protocol::Fields& fields, std::size_t index, std::string_view field_name)
    -> domain::Result<std::uint64_t> {
    if (index >= fields.size()) {
        return domain::fail<std::uint64_t>(bridge_error(
            "missing_argument", "missing bridge argument: " + std::string(field_name), domain::ExitCode::usage
        ));
    }
    const auto parsed = util::parse_u64(fields[index]);
    if (!parsed.has_value()) {
        return domain::fail<std::uint64_t>(bridge_error(
            "invalid_argument", "invalid bridge argument: " + std::string(field_name), domain::ExitCode::usage
        ));
    }
    return domain::ok(*parsed);
}

auto duration_to_timeval(std::chrono::milliseconds timeout) -> timeval {
    if (timeout <= std::chrono::milliseconds::zero()) {
        timeout = std::chrono::milliseconds(1);
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(timeout - seconds);
    return timeval{
        .tv_sec = static_cast<decltype(timeval::tv_sec)>(seconds.count()),
        .tv_usec = static_cast<decltype(timeval::tv_usec)>(micros.count()),
    };
}

auto configure_client_timeout(int fd, std::chrono::milliseconds timeout) -> bool {
    const auto value = duration_to_timeval(timeout);
    return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) == 0 &&
           ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value)) == 0;
}

}  // namespace

SocketBridgeServer::SocketBridgeServer(std::unique_ptr<sdk::Backend> backend) : backend_(std::move(backend)) {}

SocketBridgeServer::~SocketBridgeServer() {
    const auto ignored = stop();
    (void)ignored;
}

auto SocketBridgeServer::start(const sdk::InitOptions& options) -> domain::Result<void> {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        return domain::fail(bridge_error("already_running", "bridge server is already running"));
    }

    socket_path_ = resolve_socket_path(options.socket_path);
    const auto initialized = backend_->initialize(options);
    if (!initialized) {
        return initialized;
    }

    const auto cleanup_start_failure = [this](domain::Error error) -> domain::Result<void> {
        unlink_socket_file(socket_path_);
        socket_path_.clear();
        const auto shutdown = backend_->shutdown();
        (void)shutdown;
        return domain::fail(std::move(error));
    };

    std::error_code ec;
    if (const auto parent = std::filesystem::path(socket_path_).parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return cleanup_start_failure(bridge_error(
                "mkdir_failed", "failed to create socket directory: " + ec.message(), domain::ExitCode::config
            ));
        }
    }

    unlink_socket_file(socket_path_);

    FileDescriptor listen_socket(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (listen_socket.get() < 0) {
        return cleanup_start_failure(bridge_error(
            "socket_failed", "failed to create bridge socket: " + std::string(std::strerror(errno))
        ));
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(address.sun_path)) {
        return cleanup_start_failure(bridge_error(
            "socket_path_too_long", "control socket path exceeds platform limit", domain::ExitCode::config
        ));
    }
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path_.c_str());

    if (::bind(
            listen_socket.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)
        ) < 0) {
        return cleanup_start_failure(bridge_error(
            "bind_failed", "failed to bind control socket: " + std::string(std::strerror(errno))
        ));
    }

    if (::listen(listen_socket.get(), 8) < 0) {
        return cleanup_start_failure(bridge_error(
            "listen_failed", "failed to listen on control socket: " + std::string(std::strerror(errno))
        ));
    }

    listen_fd_ = listen_socket.release();
    client_timeout_ = options.command_timeout;
    running_ = true;
    accept_thread_ = std::jthread([this](std::stop_token stop_token) {
        accept_loop(stop_token);
    });
    return domain::ok();
}

auto SocketBridgeServer::stop() -> domain::Result<void> {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return domain::ok();
    }

    if (accept_thread_.joinable()) {
        accept_thread_.request_stop();
    }

    if (listen_fd_ >= 0 && !socket_path_.empty()) {
        FileDescriptor wake_socket(::socket(AF_UNIX, SOCK_STREAM, 0));
        if (wake_socket.get() >= 0) {
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path_.c_str());
            ::connect(wake_socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        }
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    unlink_socket_file(socket_path_);
    running_ = false;
    return backend_->shutdown();
}

auto SocketBridgeServer::socket_path() const -> std::string {
    std::lock_guard<std::mutex> lock(mutex_);
    return socket_path_;
}

void SocketBridgeServer::accept_loop(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        const int client_fd = ::accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (stop_token.stop_requested()) {
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EBADF || errno == EINVAL) {
                return;
            }
            continue;
        }
        handle_client(client_fd);
        ::close(client_fd);
    }
}

void SocketBridgeServer::handle_client(int client_fd) {
    if (!configure_client_timeout(client_fd, client_timeout_)) {
        return;
    }

    std::string line;
    if (!protocol::read_line(client_fd, line)) {
        return;
    }
    const auto fields = protocol::split_fields(line);
    if (fields.size() < 2 || fields[0] != protocol::kMagic) {
        send_error(client_fd, bridge_error(
            "invalid_request", "invalid bridge request", domain::ExitCode::usage
        ));
        return;
    }

    const std::string& command = fields[1];

    if (command == "plugin_info") {
        auto info = backend_->plugin_info();
        if (!info) {
            send_error(client_fd, info.error());
            return;
        }
        send_ok(client_fd, "plugin_info", protocol::encode(info.value()));
        return;
    }

    if (command == "connect") {
        if (fields.size() != 10) {
            send_error(client_fd, bridge_error(
                "invalid_request", "connect expects 8 arguments", domain::ExitCode::usage
            ));
            return;
        }
        auto host = decode_hex_arg(fields, 2, "host");
        auto port = decode_u64_arg(fields, 3, "port");
        auto nickname = decode_hex_arg(fields, 4, "nickname");
        auto identity = decode_hex_arg(fields, 5, "identity");
        auto server_password = decode_hex_arg(fields, 6, "server_password");
        auto channel_password = decode_hex_arg(fields, 7, "channel_password");
        auto default_channel = decode_hex_arg(fields, 8, "default_channel");
        auto profile_name = decode_hex_arg(fields, 9, "profile_name");
        if (!host) {
            send_error(client_fd, host.error());
            return;
        }
        if (!port) {
            send_error(client_fd, port.error());
            return;
        }
        if (!nickname) {
            send_error(client_fd, nickname.error());
            return;
        }
        if (!identity) {
            send_error(client_fd, identity.error());
            return;
        }
        if (!server_password) {
            send_error(client_fd, server_password.error());
            return;
        }
        if (!channel_password) {
            send_error(client_fd, channel_password.error());
            return;
        }
        if (!default_channel) {
            send_error(client_fd, default_channel.error());
            return;
        }
        if (!profile_name) {
            send_error(client_fd, profile_name.error());
            return;
        }
        auto connected = backend_->connect(sdk::ConnectRequest{
            .host = host.value(),
            .port = static_cast<std::uint16_t>(port.value()),
            .nickname = nickname.value(),
            .identity = identity.value(),
            .server_password = server_password.value(),
            .channel_password = channel_password.value(),
            .default_channel = default_channel.value(),
            .profile_name = profile_name.value(),
        });
        if (!connected) {
            send_error(client_fd, connected.error());
            return;
        }
        send_ok(client_fd, "void", {});
        return;
    }

    if (command == "disconnect") {
        auto reason = fields.size() > 2 ? decode_hex_arg(fields, 2, "reason")
                                        : domain::ok(std::string("ts disconnect"));
        if (!reason) {
            send_error(client_fd, reason.error());
            return;
        }
        auto disconnected = backend_->disconnect(reason.value());
        if (!disconnected) {
            send_error(client_fd, disconnected.error());
            return;
        }
        send_ok(client_fd, "void", {});
        return;
    }

    if (command == "connection_state") {
        auto state = backend_->connection_state();
        if (!state) {
            send_error(client_fd, state.error());
            return;
        }
        send_ok(client_fd, "connection_state", protocol::encode(state.value()));
        return;
    }

    if (command == "server_info") {
        auto info = backend_->server_info();
        if (!info) {
            send_error(client_fd, info.error());
            return;
        }
        send_ok(client_fd, "server_info", protocol::encode(info.value()));
        return;
    }

    if (command == "list_channels") {
        auto channels = backend_->list_channels();
        if (!channels) {
            send_error(client_fd, channels.error());
            return;
        }
        send_ok(client_fd, "channels", protocol::encode_channels(channels.value()));
        return;
    }

    if (command == "get_channel" || command == "join_channel") {
        auto selector = decode_hex_arg(fields, 2, "selector");
        if (!selector) {
            send_error(client_fd, selector.error());
            return;
        }
        if (command == "join_channel") {
            auto joined = backend_->join_channel(domain::Selector{selector.value()});
            if (!joined) {
                send_error(client_fd, joined.error());
                return;
            }
            send_ok(client_fd, "void", {});
            return;
        }
        auto channel = backend_->get_channel(domain::Selector{selector.value()});
        if (!channel) {
            send_error(client_fd, channel.error());
            return;
        }
        send_ok(client_fd, "channel", {protocol::encode(channel.value())});
        return;
    }

    if (command == "list_clients") {
        auto clients = backend_->list_clients();
        if (!clients) {
            send_error(client_fd, clients.error());
            return;
        }
        send_ok(client_fd, "clients", protocol::encode_clients(clients.value()));
        return;
    }

    if (command == "get_client") {
        auto selector = decode_hex_arg(fields, 2, "selector");
        if (!selector) {
            send_error(client_fd, selector.error());
            return;
        }
        auto client = backend_->get_client(domain::Selector{selector.value()});
        if (!client) {
            send_error(client_fd, client.error());
            return;
        }
        send_ok(client_fd, "client", {protocol::encode(client.value())});
        return;
    }

    if (command == "send_message") {
        if (fields.size() != 5) {
            send_error(client_fd, bridge_error(
                "invalid_request", "send_message expects 3 arguments", domain::ExitCode::usage
            ));
            return;
        }
        auto target = decode_hex_arg(fields, 3, "target");
        auto text = decode_hex_arg(fields, 4, "text");
        if (!target) {
            send_error(client_fd, target.error());
            return;
        }
        if (!text) {
            send_error(client_fd, text.error());
            return;
        }
        domain::MessageTargetKind target_kind{};
        if (fields[2] == "client") {
            target_kind = domain::MessageTargetKind::client;
        } else if (fields[2] == "channel") {
            target_kind = domain::MessageTargetKind::channel;
        } else {
            send_error(client_fd, bridge_error(
                "invalid_target", "invalid message target", domain::ExitCode::usage
            ));
            return;
        }
        auto sent = backend_->send_message(domain::MessageRequest{
            .target_kind = target_kind,
            .target = target.value(),
            .text = text.value(),
        });
        if (!sent) {
            send_error(client_fd, sent.error());
            return;
        }
        send_ok(client_fd, "void", {});
        return;
    }

    if (command == "next_event") {
        auto timeout_ms = decode_u64_arg(fields, 2, "timeout_ms");
        if (!timeout_ms) {
            send_error(client_fd, timeout_ms.error());
            return;
        }
        auto event = backend_->next_event(std::chrono::milliseconds(timeout_ms.value()));
        if (!event) {
            send_error(client_fd, event.error());
            return;
        }
        send_ok(client_fd, "event", protocol::encode_event(event.value()));
        return;
    }

    send_error(client_fd, bridge_error(
        "unknown_command", "unknown bridge command: " + command, domain::ExitCode::usage
    ));
}

}  // namespace teamspeak_cli::bridge
