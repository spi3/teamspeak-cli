#include "teamspeak_cli/sdk/fake_backend.hpp"
#include "teamspeak_cli/session/session_service.hpp"
#include "test_support.hpp"

int main() {
    using namespace teamspeak_cli;

    session::SessionService session(std::make_unique<sdk::FakeBackend>());

    auto initialized = session.initialize(sdk::InitOptions{});
    tests::expect(initialized.ok(), "session initialize should succeed");

    auto connected = session.connect(sdk::ConnectRequest{
        .host = "127.0.0.1",
        .port = 9987,
        .nickname = "tester",
        .identity = "test-identity",
        .server_password = "",
        .channel_password = "",
        .default_channel = "",
        .profile_name = "built-test",
    });
    tests::expect(connected.ok(), "session connect should succeed");

    auto status = session.connection_state();
    tests::expect(status.ok(), "status should succeed");
    tests::expect_eq(status.value().nickname, std::string("tester"), "nickname propagated");

    auto joined = session.join_channel(domain::Selector{"Engineering"});
    tests::expect(joined.ok(), "join channel should succeed");

    auto server = session.server_info();
    tests::expect(server.ok(), "server info should succeed");
    tests::expect_eq(server.value().current_channel->value, std::uint64_t(2), "current channel updated");

    auto disconnected = session.disconnect("done");
    tests::expect(disconnected.ok(), "disconnect should succeed");
    auto shutdown = session.shutdown();
    tests::expect(shutdown.ok(), "shutdown should succeed");
    return 0;
}
