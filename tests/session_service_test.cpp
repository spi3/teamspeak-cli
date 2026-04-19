#include <thread>
#include <vector>

#include "teamspeak_cli/sdk/mock_backend.hpp"
#include "teamspeak_cli/session/session_service.hpp"
#include "test_support.hpp"

namespace {

auto stub_error(std::string code, std::string message) -> teamspeak_cli::domain::Error {
    return teamspeak_cli::domain::make_error(
        "test", std::move(code), std::move(message), teamspeak_cli::domain::ExitCode::internal
    );
}

class StuckConnectingBackend final : public teamspeak_cli::sdk::Backend {
  public:
    [[nodiscard]] auto kind() const -> std::string override { return "test"; }

    auto initialize(const teamspeak_cli::sdk::InitOptions&) -> teamspeak_cli::domain::Result<void> override {
        initialized_ = true;
        return teamspeak_cli::domain::ok();
    }

    auto shutdown() -> teamspeak_cli::domain::Result<void> override {
        initialized_ = false;
        return teamspeak_cli::domain::ok();
    }

    auto connect(const teamspeak_cli::sdk::ConnectRequest& request) -> teamspeak_cli::domain::Result<void> override {
        state_ = teamspeak_cli::domain::ConnectionState{
            .phase = teamspeak_cli::domain::ConnectionPhase::connecting,
            .backend = "test",
            .connection = {77},
            .server = request.host,
            .port = request.port,
            .nickname = request.nickname,
            .identity = request.identity,
            .profile = request.profile_name,
            .mode = "test",
        };
        events_.push_back(teamspeak_cli::domain::Event{
            .type = "connection.requested",
            .summary = "requested test connection",
            .at = std::chrono::system_clock::now(),
            .fields = {},
        });
        events_.push_back(teamspeak_cli::domain::Event{
            .type = "connection.connecting",
            .summary = "test connection is starting",
            .at = std::chrono::system_clock::now(),
            .fields = {},
        });
        return teamspeak_cli::domain::ok();
    }

    auto disconnect(std::string_view) -> teamspeak_cli::domain::Result<void> override {
        state_.phase = teamspeak_cli::domain::ConnectionPhase::disconnected;
        state_.connection = {0};
        return teamspeak_cli::domain::ok();
    }

    [[nodiscard]] auto plugin_info() const -> teamspeak_cli::domain::Result<teamspeak_cli::domain::PluginInfo> override {
        return teamspeak_cli::domain::fail<teamspeak_cli::domain::PluginInfo>(stub_error(
            "unsupported", "plugin_info is not used by this test backend"
        ));
    }

    [[nodiscard]] auto connection_state() const
        -> teamspeak_cli::domain::Result<teamspeak_cli::domain::ConnectionState> override {
        return teamspeak_cli::domain::ok(state_);
    }

    [[nodiscard]] auto server_info() const
        -> teamspeak_cli::domain::Result<teamspeak_cli::domain::ServerInfo> override {
        return teamspeak_cli::domain::fail<teamspeak_cli::domain::ServerInfo>(stub_error(
            "unsupported", "server_info is not used by this test backend"
        ));
    }

    [[nodiscard]] auto list_channels() const
        -> teamspeak_cli::domain::Result<std::vector<teamspeak_cli::domain::Channel>> override {
        return teamspeak_cli::domain::fail<std::vector<teamspeak_cli::domain::Channel>>(stub_error(
            "unsupported", "list_channels is not used by this test backend"
        ));
    }

    [[nodiscard]] auto list_clients() const
        -> teamspeak_cli::domain::Result<std::vector<teamspeak_cli::domain::Client>> override {
        return teamspeak_cli::domain::fail<std::vector<teamspeak_cli::domain::Client>>(stub_error(
            "unsupported", "list_clients is not used by this test backend"
        ));
    }

    [[nodiscard]] auto get_channel(const teamspeak_cli::domain::Selector&) const
        -> teamspeak_cli::domain::Result<teamspeak_cli::domain::Channel> override {
        return teamspeak_cli::domain::fail<teamspeak_cli::domain::Channel>(stub_error(
            "unsupported", "get_channel is not used by this test backend"
        ));
    }

    [[nodiscard]] auto get_client(const teamspeak_cli::domain::Selector&) const
        -> teamspeak_cli::domain::Result<teamspeak_cli::domain::Client> override {
        return teamspeak_cli::domain::fail<teamspeak_cli::domain::Client>(stub_error(
            "unsupported", "get_client is not used by this test backend"
        ));
    }

    auto join_channel(const teamspeak_cli::domain::Selector&) -> teamspeak_cli::domain::Result<void> override {
        return teamspeak_cli::domain::fail(stub_error(
            "unsupported", "join_channel is not used by this test backend"
        ));
    }

    auto send_message(const teamspeak_cli::domain::MessageRequest&) -> teamspeak_cli::domain::Result<void> override {
        return teamspeak_cli::domain::fail(stub_error(
            "unsupported", "send_message is not used by this test backend"
        ));
    }

    auto next_event(std::chrono::milliseconds timeout)
        -> teamspeak_cli::domain::Result<std::optional<teamspeak_cli::domain::Event>> override {
        if (!events_.empty()) {
            auto event = std::move(events_.front());
            events_.erase(events_.begin());
            return teamspeak_cli::domain::ok(std::optional<teamspeak_cli::domain::Event>{std::move(event)});
        }
        std::this_thread::sleep_for(timeout);
        return teamspeak_cli::domain::ok(std::optional<teamspeak_cli::domain::Event>{});
    }

  private:
    bool initialized_ = false;
    std::vector<teamspeak_cli::domain::Event> events_;
    teamspeak_cli::domain::ConnectionState state_{
        .phase = teamspeak_cli::domain::ConnectionPhase::disconnected,
        .backend = "test",
        .connection = {0},
        .server = "",
        .port = 0,
        .nickname = "",
        .identity = "",
        .profile = "",
        .mode = "test",
    };
};

class FailedConnectionBackend final : public teamspeak_cli::sdk::Backend {
  public:
    [[nodiscard]] auto kind() const -> std::string override { return "test"; }

    auto initialize(const teamspeak_cli::sdk::InitOptions&) -> teamspeak_cli::domain::Result<void> override {
        initialized_ = true;
        return teamspeak_cli::domain::ok();
    }

    auto shutdown() -> teamspeak_cli::domain::Result<void> override {
        initialized_ = false;
        return teamspeak_cli::domain::ok();
    }

    auto connect(const teamspeak_cli::sdk::ConnectRequest& request) -> teamspeak_cli::domain::Result<void> override {
        state_ = teamspeak_cli::domain::ConnectionState{
            .phase = teamspeak_cli::domain::ConnectionPhase::disconnected,
            .backend = "test",
            .connection = {0},
            .server = request.host,
            .port = request.port,
            .nickname = request.nickname,
            .identity = request.identity,
            .profile = request.profile_name,
            .mode = "test",
        };
        events_.push_back(teamspeak_cli::domain::Event{
            .type = "connection.requested",
            .summary = "requested test connection",
            .at = std::chrono::system_clock::now(),
            .fields = {},
        });
        events_.push_back(teamspeak_cli::domain::Event{
            .type = "connection.error",
            .summary = "test connection failed",
            .at = std::chrono::system_clock::now(),
            .fields = {},
        });
        return teamspeak_cli::domain::ok();
    }

    auto disconnect(std::string_view) -> teamspeak_cli::domain::Result<void> override {
        state_.phase = teamspeak_cli::domain::ConnectionPhase::disconnected;
        state_.connection = {0};
        return teamspeak_cli::domain::ok();
    }

    [[nodiscard]] auto plugin_info() const -> teamspeak_cli::domain::Result<teamspeak_cli::domain::PluginInfo> override {
        return teamspeak_cli::domain::fail<teamspeak_cli::domain::PluginInfo>(stub_error(
            "unsupported", "plugin_info is not used by this test backend"
        ));
    }

    [[nodiscard]] auto connection_state() const
        -> teamspeak_cli::domain::Result<teamspeak_cli::domain::ConnectionState> override {
        return teamspeak_cli::domain::ok(state_);
    }

    [[nodiscard]] auto server_info() const
        -> teamspeak_cli::domain::Result<teamspeak_cli::domain::ServerInfo> override {
        return teamspeak_cli::domain::fail<teamspeak_cli::domain::ServerInfo>(stub_error(
            "unsupported", "server_info is not used by this test backend"
        ));
    }

    [[nodiscard]] auto list_channels() const
        -> teamspeak_cli::domain::Result<std::vector<teamspeak_cli::domain::Channel>> override {
        return teamspeak_cli::domain::fail<std::vector<teamspeak_cli::domain::Channel>>(stub_error(
            "unsupported", "list_channels is not used by this test backend"
        ));
    }

    [[nodiscard]] auto list_clients() const
        -> teamspeak_cli::domain::Result<std::vector<teamspeak_cli::domain::Client>> override {
        return teamspeak_cli::domain::fail<std::vector<teamspeak_cli::domain::Client>>(stub_error(
            "unsupported", "list_clients is not used by this test backend"
        ));
    }

    [[nodiscard]] auto get_channel(const teamspeak_cli::domain::Selector&) const
        -> teamspeak_cli::domain::Result<teamspeak_cli::domain::Channel> override {
        return teamspeak_cli::domain::fail<teamspeak_cli::domain::Channel>(stub_error(
            "unsupported", "get_channel is not used by this test backend"
        ));
    }

    [[nodiscard]] auto get_client(const teamspeak_cli::domain::Selector&) const
        -> teamspeak_cli::domain::Result<teamspeak_cli::domain::Client> override {
        return teamspeak_cli::domain::fail<teamspeak_cli::domain::Client>(stub_error(
            "unsupported", "get_client is not used by this test backend"
        ));
    }

    auto join_channel(const teamspeak_cli::domain::Selector&) -> teamspeak_cli::domain::Result<void> override {
        return teamspeak_cli::domain::fail(stub_error(
            "unsupported", "join_channel is not used by this test backend"
        ));
    }

    auto send_message(const teamspeak_cli::domain::MessageRequest&) -> teamspeak_cli::domain::Result<void> override {
        return teamspeak_cli::domain::fail(stub_error(
            "unsupported", "send_message is not used by this test backend"
        ));
    }

    auto next_event(std::chrono::milliseconds timeout)
        -> teamspeak_cli::domain::Result<std::optional<teamspeak_cli::domain::Event>> override {
        if (!events_.empty()) {
            auto event = std::move(events_.front());
            events_.erase(events_.begin());
            return teamspeak_cli::domain::ok(std::optional<teamspeak_cli::domain::Event>{std::move(event)});
        }
        std::this_thread::sleep_for(timeout);
        return teamspeak_cli::domain::ok(std::optional<teamspeak_cli::domain::Event>{});
    }

  private:
    bool initialized_ = false;
    std::vector<teamspeak_cli::domain::Event> events_;
    teamspeak_cli::domain::ConnectionState state_{
        .phase = teamspeak_cli::domain::ConnectionPhase::disconnected,
        .backend = "test",
        .connection = {0},
        .server = "",
        .port = 0,
        .nickname = "",
        .identity = "",
        .profile = "",
        .mode = "test",
    };
};

}  // namespace

int main() {
    using namespace teamspeak_cli;

    session::SessionService session(std::make_unique<sdk::MockBackend>());

    auto initialized = session.initialize(sdk::InitOptions{});
    tests::expect(initialized.ok(), "session initialize should succeed");

    auto connected = session.connect_and_wait(sdk::ConnectRequest{
        .host = "127.0.0.1",
        .port = 9987,
        .nickname = "tester",
        .identity = "test-identity",
        .server_password = "",
        .channel_password = "",
        .default_channel = "",
        .profile_name = "built-test",
    },
                                           std::chrono::milliseconds(250));
    tests::expect(connected.ok(), "session connect should succeed");
    tests::expect(connected.value().connected, "connect_and_wait should report a connected result");
    tests::expect(!connected.value().timed_out, "connect_and_wait should not time out for the mock backend");
    tests::expect_eq(
        connected.value().state.nickname, std::string("tester"), "connect_and_wait should propagate the nickname"
    );
    tests::expect_eq(
        connected.value().lifecycle.size(), std::size_t(3), "mock backend should emit a complete connection lifecycle"
    );
    tests::expect_eq(
        connected.value().lifecycle[0].type, std::string("connection.requested"), "first lifecycle event type"
    );
    tests::expect_eq(
        connected.value().lifecycle[1].type, std::string("connection.connecting"), "second lifecycle event type"
    );
    tests::expect_eq(
        connected.value().lifecycle[2].type, std::string("connection.connected"), "third lifecycle event type"
    );

    auto joined = session.join_channel(domain::Selector{"Engineering"});
    tests::expect(joined.ok(), "join channel should succeed");

    auto server = session.server_info();
    tests::expect(server.ok(), "server info should succeed");
    tests::expect_eq(server.value().current_channel->value, std::uint64_t(2), "current channel updated");

    auto disconnected = session.disconnect_and_wait("done", std::chrono::milliseconds(250));
    tests::expect(disconnected.ok(), "disconnect should succeed");
    tests::expect(disconnected.value().disconnected, "disconnect_and_wait should report a disconnected result");
    tests::expect(!disconnected.value().timed_out, "disconnect_and_wait should not time out for the mock backend");
    tests::expect_eq(
        disconnected.value().state.phase,
        domain::ConnectionPhase::disconnected,
        "disconnect_and_wait should end in the disconnected phase"
    );
    tests::expect_eq(
        disconnected.value().lifecycle.size(),
        std::size_t(1),
        "disconnect_and_wait should capture the disconnect lifecycle"
    );
    tests::expect_eq(
        disconnected.value().lifecycle[0].type,
        std::string("connection.disconnected"),
        "disconnect_and_wait should include the disconnected event"
    );
    auto shutdown = session.shutdown();
    tests::expect(shutdown.ok(), "shutdown should succeed");

    session::SessionService stuck_session(std::make_unique<StuckConnectingBackend>());
    tests::expect(stuck_session.initialize(sdk::InitOptions{}).ok(), "stuck backend initialize should succeed");
    auto stuck_connect = stuck_session.connect_and_wait(sdk::ConnectRequest{
        .host = "127.0.0.1",
        .port = 9987,
        .nickname = "tester",
        .identity = "test-identity",
        .server_password = "",
        .channel_password = "",
        .default_channel = "",
        .profile_name = "stuck",
    },
                                                        std::chrono::milliseconds(60));
    tests::expect(stuck_connect.ok(), "stuck connect_and_wait should succeed");
    tests::expect(!stuck_connect.value().connected, "stuck connect should not report connected");
    tests::expect(stuck_connect.value().timed_out, "stuck connect should time out");
    tests::expect_eq(
        stuck_connect.value().state.phase,
        domain::ConnectionPhase::connecting,
        "stuck connect should preserve the last connecting phase"
    );
    tests::expect_eq(
        stuck_connect.value().lifecycle.size(), std::size_t(2), "stuck connect should capture the emitted lifecycle"
    );
    tests::expect(stuck_session.shutdown().ok(), "stuck backend shutdown should succeed");

    session::SessionService failed_session(std::make_unique<FailedConnectionBackend>());
    tests::expect(failed_session.initialize(sdk::InitOptions{}).ok(), "failed backend initialize should succeed");
    auto failed_connect = failed_session.connect_and_wait(sdk::ConnectRequest{
        .host = "127.0.0.1",
        .port = 9987,
        .nickname = "tester",
        .identity = "test-identity",
        .server_password = "",
        .channel_password = "",
        .default_channel = "",
        .profile_name = "failed",
    },
                                                          std::chrono::milliseconds(250));
    tests::expect(failed_connect.ok(), "failed connect_and_wait should succeed");
    tests::expect(!failed_connect.value().connected, "failed connect should not report connected");
    tests::expect(!failed_connect.value().timed_out, "failed connect should complete without timing out");
    tests::expect_eq(
        failed_connect.value().state.phase,
        domain::ConnectionPhase::disconnected,
        "failed connect should end in a disconnected phase"
    );
    tests::expect_eq(
        failed_connect.value().lifecycle.back().type,
        std::string("connection.error"),
        "failed connect should include the terminal error event"
    );
    tests::expect(failed_session.shutdown().ok(), "failed backend shutdown should succeed");
    return 0;
}
