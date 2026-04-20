#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "teamspeak_cli/bridge/socket_server.hpp"
#include "teamspeak_cli/sdk/mock_backend.hpp"
#include "teamspeak_cli/sdk/socket_backend.hpp"
#include "test_support.hpp"

namespace {

class SocketFile {
  public:
    explicit SocketFile(const std::filesystem::path& path) {
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        teamspeak_cli::tests::expect(fd_ >= 0, "stale socket test fixture should create a unix socket");

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", path.c_str());

        const int bound = ::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        teamspeak_cli::tests::expect(bound == 0, "stale socket test fixture should bind the unix socket");
    }

    ~SocketFile() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

  private:
    int fd_ = -1;
};

class HungSocketServer {
  public:
    explicit HungSocketServer(const std::filesystem::path& path) : path_(path) {
        listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        teamspeak_cli::tests::expect(listen_fd_ >= 0, "hung server should create a unix socket");

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", path.c_str());

        const int bound = ::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        teamspeak_cli::tests::expect(bound == 0, "hung server should bind the unix socket");
        const int listening = ::listen(listen_fd_, 8);
        teamspeak_cli::tests::expect(listening == 0, "hung server should listen on the unix socket");

        thread_ = std::jthread([this](std::stop_token stop_token) {
            const int accepted_fd = ::accept(listen_fd_, nullptr, nullptr);
            if (accepted_fd < 0) {
                return;
            }
            client_fd_ = accepted_fd;
            while (!stop_token.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            ::close(accepted_fd);
            client_fd_ = -1;
        });
    }

    ~HungSocketServer() {
        if (thread_.joinable()) {
            thread_.request_stop();
        }
        if (client_fd_ >= 0) {
            ::shutdown(client_fd_, SHUT_RDWR);
        }
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

  private:
    std::filesystem::path path_;
    int listen_fd_ = -1;
    int client_fd_ = -1;
    std::jthread thread_;
};

namespace domain = teamspeak_cli::domain;
namespace sdk = teamspeak_cli::sdk;

auto counting_backend_error(std::string_view method) -> domain::Error;

class CountingBackend final : public sdk::Backend {
  public:
    [[nodiscard]] auto kind() const -> std::string override {
        return "counting";
    }

    auto initialize(const sdk::InitOptions&) -> domain::Result<void> override {
        ++initialize_calls;
        initialized = true;
        return domain::ok();
    }

    auto shutdown() -> domain::Result<void> override {
        ++shutdown_calls;
        initialized = false;
        return domain::ok();
    }

    auto connect(const sdk::ConnectRequest&) -> domain::Result<void> override {
        return domain::fail(counting_backend_error("connect"));
    }

    auto disconnect(std::string_view) -> domain::Result<void> override {
        return domain::fail(counting_backend_error("disconnect"));
    }

    [[nodiscard]] auto plugin_info() const -> domain::Result<domain::PluginInfo> override {
        return domain::fail<domain::PluginInfo>(counting_backend_error("plugin_info"));
    }

    [[nodiscard]] auto connection_state() const -> domain::Result<domain::ConnectionState> override {
        return domain::fail<domain::ConnectionState>(counting_backend_error("connection_state"));
    }

    [[nodiscard]] auto server_info() const -> domain::Result<domain::ServerInfo> override {
        return domain::fail<domain::ServerInfo>(counting_backend_error("server_info"));
    }

    [[nodiscard]] auto list_channels() const -> domain::Result<std::vector<domain::Channel>> override {
        return domain::fail<std::vector<domain::Channel>>(counting_backend_error("list_channels"));
    }

    [[nodiscard]] auto list_clients() const -> domain::Result<std::vector<domain::Client>> override {
        return domain::fail<std::vector<domain::Client>>(counting_backend_error("list_clients"));
    }

    [[nodiscard]] auto get_channel(const domain::Selector&) const -> domain::Result<domain::Channel> override {
        return domain::fail<domain::Channel>(counting_backend_error("get_channel"));
    }

    [[nodiscard]] auto get_client(const domain::Selector&) const -> domain::Result<domain::Client> override {
        return domain::fail<domain::Client>(counting_backend_error("get_client"));
    }

    auto join_channel(const domain::Selector&) -> domain::Result<void> override {
        return domain::fail(counting_backend_error("join_channel"));
    }

    auto send_message(const domain::MessageRequest&) -> domain::Result<void> override {
        return domain::fail(counting_backend_error("send_message"));
    }

    auto next_event(std::chrono::milliseconds) -> domain::Result<std::optional<domain::Event>> override {
        return domain::fail<std::optional<domain::Event>>(counting_backend_error("next_event"));
    }

    int initialize_calls = 0;
    int shutdown_calls = 0;
    bool initialized = false;
};

auto connect_raw_socket(const std::filesystem::path& path) -> int {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    teamspeak_cli::tests::expect(fd >= 0, "raw client should create a unix socket");

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", path.c_str());

    const int connected = ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    teamspeak_cli::tests::expect(connected == 0, "raw client should connect to the unix socket");
    return fd;
}

auto counting_backend_error(std::string_view method) -> domain::Error {
    return domain::make_error(
        "sdk",
        "not_implemented",
        "counting backend does not implement " + std::string(method),
        domain::ExitCode::sdk
    );
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    using namespace teamspeak_cli;

    const fs::path root = tests::make_temp_path("ts-plugin-socket-test");
    fs::create_directories(root);
    const fs::path socket_path = root / "plugin.sock";

    bridge::SocketBridgeServer server(std::make_unique<sdk::MockBackend>());
    auto started = server.start(sdk::InitOptions{.socket_path = socket_path.string()});
    tests::expect(started.ok(), "socket bridge server should start");

    sdk::SocketBackend backend;
    auto initialized = backend.initialize(sdk::InitOptions{.socket_path = socket_path.string()});
    tests::expect(initialized.ok(), "socket backend should initialize");

    auto info = backend.plugin_info();
    tests::expect(info.ok(), "plugin info should succeed");
    tests::expect_eq(info.value().plugin_available, true, "plugin bridge should be available");

    auto state = backend.connection_state();
    tests::expect(state.ok(), "connection state should succeed");
    tests::expect_eq(state.value().phase, domain::ConnectionPhase::connected, "mock bridge host starts connected");

    auto channels = backend.list_channels();
    tests::expect(channels.ok(), "channel list should succeed");
    tests::expect(!channels.value().empty(), "channel list should not be empty");

    auto clients = backend.list_clients();
    tests::expect(clients.ok(), "client list should succeed");
    tests::expect(!clients.value().empty(), "client list should not be empty");

    auto sent = backend.send_message(domain::MessageRequest{
        .target_kind = domain::MessageTargetKind::channel,
        .target = "Engineering",
        .text = "hello from socket backend",
    });
    tests::expect(sent.ok(), "send message should succeed");
    tests::expect(fs::exists(socket_path), "socket bridge server should create the control socket file");

    auto event = backend.next_event(std::chrono::milliseconds(500));
    tests::expect(event.ok(), "next event should succeed");
    tests::expect(event.value().has_value(), "next event should yield an event");

    auto shutdown = backend.shutdown();
    tests::expect(shutdown.ok(), "backend shutdown should succeed");
    auto stopped = server.stop();
    tests::expect(stopped.ok(), "socket bridge server should stop");
    tests::expect(
        !fs::exists(socket_path),
        "socket bridge server should remove the control socket file on stop"
    );

    const fs::path regular_file_path = root / "regular-file.sock";
    {
        std::ofstream regular_file(regular_file_path);
        tests::expect(regular_file.is_open(), "test should create a regular file at the socket path");
        regular_file << "not a unix socket";
    }

    auto counting_backend = std::make_unique<CountingBackend>();
    auto* counting_backend_raw = counting_backend.get();
    bridge::SocketBridgeServer regular_server(std::move(counting_backend));
    auto regular_started = regular_server.start(sdk::InitOptions{.socket_path = regular_file_path.string()});
    tests::expect(
        !regular_started.ok(),
        "socket bridge server should refuse to unlink a regular file at the configured socket path"
    );
    tests::expect_eq(
        regular_started.error().code,
        std::string("bind_failed"),
        "regular file path should fail at bind instead of being removed"
    );
    tests::expect_eq(
        counting_backend_raw->initialize_calls,
        1,
        "failed bridge startup should initialize the backend once"
    );
    tests::expect_eq(
        counting_backend_raw->shutdown_calls,
        1,
        "failed bridge startup should shut the backend down after initialization"
    );
    tests::expect(
        !counting_backend_raw->initialized,
        "failed bridge startup should leave the backend uninitialized"
    );
    tests::expect(
        regular_server.socket_path().empty(),
        "failed bridge startup should clear the socket path state"
    );
    tests::expect(
        fs::exists(regular_file_path),
        "regular file should remain in place after a failed socket server start"
    );
    auto regular_stopped = regular_server.stop();
    tests::expect(regular_stopped.ok(), "failed bridge server should still stop cleanly");
    tests::expect_eq(
        counting_backend_raw->shutdown_calls,
        1,
        "failed bridge startup should not require an additional shutdown during stop"
    );

    const fs::path stale_socket_path = root / "stale.sock";
    SocketFile stale_socket(stale_socket_path);
    stale_socket.close();

    sdk::SocketBackend unavailable_backend;
    auto unavailable_initialized = unavailable_backend.initialize(sdk::InitOptions{.socket_path = stale_socket_path.string()});
    tests::expect(unavailable_initialized.ok(), "backend should initialize for unavailable bridge test");

    auto unavailable_state = unavailable_backend.connection_state();
    tests::expect(!unavailable_state.ok(), "connection state should fail when the bridge is unavailable");
    tests::expect_eq(
        unavailable_state.error().code,
        std::string("socket_connect_failed"),
        "unavailable bridge should surface a socket connect error"
    );
    tests::expect_contains(
        unavailable_state.error().message,
        "Unable to read TeamSpeak status because the TeamSpeak client is not running or the ts3cli plugin is unavailable.",
        "connection failure should explain the likely cause"
    );
    tests::expect(
        unavailable_state.error().message.find(stale_socket_path.string()) == std::string::npos,
        "connection failure should not expose the socket path in the default error message"
    );
    tests::expect_eq(
        unavailable_state.error().details.at("operation"),
        std::string("read TeamSpeak status"),
        "connection failure should record the attempted operation"
    );
    tests::expect_eq(
        unavailable_state.error().details.at("socket_path"),
        stale_socket_path.string(),
        "connection failure should preserve the socket path in debug details"
    );
    tests::expect_eq(
        unavailable_state.error().details.at("os_error"),
        std::string(std::strerror(ECONNREFUSED)),
        "connection failure should preserve the OS socket error in debug details"
    );

    const fs::path hung_socket_path = root / "hung.sock";
    HungSocketServer hung_server(hung_socket_path);

    sdk::SocketBackend hung_backend;
    auto hung_initialized = hung_backend.initialize(sdk::InitOptions{
        .socket_path = hung_socket_path.string(),
        .command_timeout = std::chrono::milliseconds(100),
    });
    tests::expect(hung_initialized.ok(), "backend should initialize for hung bridge test");

    const auto hung_started_at = std::chrono::steady_clock::now();
    auto hung_state = hung_backend.connection_state();
    const auto hung_elapsed = std::chrono::steady_clock::now() - hung_started_at;
    tests::expect(!hung_state.ok(), "connection state should fail when the bridge does not respond");
    tests::expect_eq(
        hung_state.error().code,
        std::string("socket_timeout"),
        "hung bridge should surface a socket timeout"
    );
    tests::expect_eq(
        hung_state.error().details.at("stage"),
        std::string("read"),
        "hung bridge should time out while reading the response"
    );
    tests::expect_eq(
        hung_state.error().details.at("timeout_ms"),
        std::string("100"),
        "hung bridge should preserve the configured timeout"
    );
    tests::expect_less_equal(
        std::chrono::duration_cast<std::chrono::milliseconds>(hung_elapsed),
        std::chrono::milliseconds(500),
        "hung bridge should fail quickly instead of hanging indefinitely"
    );

    const fs::path stalled_socket_path = root / "stalled.sock";
    bridge::SocketBridgeServer stalled_server(std::make_unique<sdk::MockBackend>());
    auto stalled_started = stalled_server.start(sdk::InitOptions{
        .socket_path = stalled_socket_path.string(),
        .command_timeout = std::chrono::milliseconds(100),
    });
    tests::expect(stalled_started.ok(), "socket bridge server should start for stalled client test");

    const int stalled_client_fd = connect_raw_socket(stalled_socket_path);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    sdk::SocketBackend recovered_backend;
    auto recovered_initialized = recovered_backend.initialize(sdk::InitOptions{
        .socket_path = stalled_socket_path.string(),
        .command_timeout = std::chrono::milliseconds(100),
    });
    tests::expect(recovered_initialized.ok(), "backend should initialize for stalled client recovery test");

    auto recovered_state = recovered_backend.connection_state();
    tests::expect(
        recovered_state.ok(),
        "connection state should recover after a stalled client times out on the bridge server"
    );

    ::close(stalled_client_fd);
    auto recovered_shutdown = recovered_backend.shutdown();
    tests::expect(recovered_shutdown.ok(), "recovered backend shutdown should succeed");
    auto stalled_stopped = stalled_server.stop();
    tests::expect(stalled_stopped.ok(), "stalled socket bridge server should stop");

    fs::remove_all(root);
    return 0;
}
