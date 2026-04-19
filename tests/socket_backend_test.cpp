#include <filesystem>

#include "teamspeak_cli/bridge/socket_server.hpp"
#include "teamspeak_cli/sdk/fake_backend.hpp"
#include "teamspeak_cli/sdk/socket_backend.hpp"
#include "test_support.hpp"

int main() {
    namespace fs = std::filesystem;
    using namespace teamspeak_cli;

    const fs::path root = tests::make_temp_path("ts-plugin-socket-test");
    fs::create_directories(root);
    const fs::path socket_path = root / "plugin.sock";

    bridge::SocketBridgeServer server(std::make_unique<sdk::FakeBackend>());
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
    tests::expect_eq(state.value().phase, domain::ConnectionPhase::connected, "built-test plugin host starts connected");

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

    auto event = backend.next_event(std::chrono::milliseconds(500));
    tests::expect(event.ok(), "next event should succeed");
    tests::expect(event.value().has_value(), "next event should yield an event");

    auto shutdown = backend.shutdown();
    tests::expect(shutdown.ok(), "backend shutdown should succeed");
    auto stopped = server.stop();
    tests::expect(stopped.ok(), "socket bridge server should stop");

    fs::remove_all(root);
    return 0;
}
