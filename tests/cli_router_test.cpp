#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "teamspeak_cli/cli/command_router.hpp"
#include "teamspeak_cli/config/config_store.hpp"
#include "teamspeak_cli/output/render.hpp"
#include "test_support.hpp"

namespace teamspeak_cli::cli {
auto read_install_receipt_value_for_test(std::string_view value) -> std::string;
}

namespace fs = std::filesystem;

namespace {

class EnvGuard {
  public:
    EnvGuard(std::string name, std::string value) : name_(std::move(name)) {
        if (const char* current = std::getenv(name_.c_str())) {
            previous_ = current;
        }
        ::setenv(name_.c_str(), value.c_str(), 1);
    }

    ~EnvGuard() {
        if (previous_.has_value()) {
            ::setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

  private:
    std::string name_;
    std::optional<std::string> previous_;
};

class SocketFile {
  public:
    explicit SocketFile(const fs::path& path) {
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        teamspeak_cli::tests::expect(fd_ >= 0, "socket fixture should create a unix socket");

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", path.c_str());

        const int bound = ::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        teamspeak_cli::tests::expect(bound == 0, "socket fixture should bind the unix socket");
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

auto parse_command(
    const teamspeak_cli::cli::CommandRouter& router,
    const std::vector<std::string>& args
) -> teamspeak_cli::domain::Result<teamspeak_cli::cli::ParsedCommand> {
    std::vector<std::string> storage;
    storage.reserve(args.size() + 1);
    storage.emplace_back("ts");
    storage.insert(storage.end(), args.begin(), args.end());

    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& arg : storage) {
        argv.push_back(arg.data());
    }

    return router.parse(static_cast<int>(argv.size()), argv.data());
}

}  // namespace

int main() {
    using namespace teamspeak_cli;

    cli::CommandRouter router;
    config::ConfigStore config_store;

    const std::string help = router.render_help({});
    tests::expect_contains(
        help, "It is not ServerQuery, WebQuery", "top-level help should reinforce the product scope"
    );
    tests::expect_contains(
        help, "Run `ts <command> --help`", "top-level help should point users at command-specific help"
    );

    tests::expect_eq(
        cli::read_install_receipt_value_for_test("/tmp/teamspeak-cli/install dir"),
        std::string("/tmp/teamspeak-cli/install dir"),
        "raw receipt values should remain unchanged"
    );
    tests::expect_eq(
        cli::read_install_receipt_value_for_test("/tmp/teamspeak-cli/install\\ dir/xdotool\\\\root"),
        std::string("/tmp/teamspeak-cli/install dir/xdotool\\root"),
        "bash-escaped receipt values should be decoded"
    );

    const std::string channel_help = router.render_help({"channel"});
    tests::expect_contains(channel_help, "ts channel <subcommand>", "channel help usage");
    tests::expect_contains(channel_help, "Subcommands:", "channel help should list subcommands");
    tests::expect_contains(channel_help, "ts channel list", "channel help should include examples");
    tests::expect_contains(channel_help, "ts channel clients [id-or-name]", "channel help should include channel clients");

    const std::vector<std::string> grouped_commands = {
        "plugin", "sdk", "config", "profile", "server", "channel", "client", "message", "events"
    };
    for (const auto& grouped_command : grouped_commands) {
        auto grouped = parse_command(router, {grouped_command});
        tests::expect(grouped.ok(), grouped_command + " parse should succeed");
        tests::expect_eq(grouped.value().path.size(), std::size_t(1), grouped_command + " path size");
        tests::expect_eq(grouped.value().path[0], grouped_command, grouped_command + " path name");
        tests::expect(grouped.value().show_help, grouped_command + " should show contextual help");
    }

    auto invalid_group_subcommand = parse_command(router, {"channel", "missing"});
    tests::expect(!invalid_group_subcommand.ok(), "unknown channel subcommand should fail");
    tests::expect_contains(
        invalid_group_subcommand.error().message,
        "unknown command: channel missing",
        "unknown group subcommand message"
    );

    auto parsed = parse_command(
        router,
        {"--json", "events", "watch", "--count", "3", "--timeout-ms", "25"}
    );
    tests::expect(parsed.ok(), "events watch parse should succeed");
    tests::expect_eq(parsed.value().global.format, output::Format::json, "json global flag");
    tests::expect_eq(parsed.value().path.size(), std::size_t(2), "events watch path size");
    tests::expect_eq(parsed.value().path[0], std::string("events"), "command path head");
    tests::expect_eq(parsed.value().path[1], std::string("watch"), "command path tail");
    tests::expect_eq(parsed.value().options.at("count"), std::string("3"), "count option");
    tests::expect_eq(parsed.value().options.at("timeout-ms"), std::string("25"), "timeout option");

    auto version = parse_command(router, {"version"});
    tests::expect(version.ok(), "version parse should succeed");
    auto version_result = router.dispatch(version.value());
    tests::expect(version_result.ok(), "version dispatch should succeed");
    tests::expect_contains(
        output::render(version_result.value(), output::Format::json),
        "\"name\":\"ts\"",
        "version json should include executable name"
    );

    const fs::path temp_dir = tests::make_temp_path("ts-cli-router-test");
    fs::create_directories(temp_dir);
    const fs::path config_path = temp_dir / "config.ini";

    auto init = parse_command(router, {"config", "init", "--config", config_path.string()});
    tests::expect(init.ok(), "config init parse should succeed");
    auto init_result = router.dispatch(init.value());
    tests::expect(init_result.ok(), "config init dispatch should succeed");
    tests::expect(fs::exists(config_path), "config init should write a config file");

    auto use_plugin = parse_command(
        router, {"profile", "use", "plugin-local", "--config", config_path.string()}
    );
    tests::expect(use_plugin.ok(), "profile use parse should succeed");
    auto use_result = router.dispatch(use_plugin.value());
    tests::expect(use_result.ok(), "profile use dispatch should succeed");

    auto loaded = config_store.load(config_path);
    tests::expect(loaded.ok(), "config should load after profile switch");
    tests::expect_eq(
        loaded.value().active_profile, std::string("plugin-local"), "active profile should persist"
    );
    auto plugin_profile = config_store.find_profile(loaded.value(), "plugin-local");
    tests::expect(plugin_profile.ok(), "plugin-local profile should exist for launch testing");
    const fs::path launch_socket_path = temp_dir / "plugin.sock";
    plugin_profile.value()->control_socket_path = launch_socket_path.string();
    auto saved_launch_config = config_store.save(config_path, loaded.value());
    tests::expect(saved_launch_config.ok(), "launch test config should save");

    auto connect_command = parse_command(
        router,
        {
            "--profile",
            "mock-local",
            "--config",
            config_path.string(),
            "--server",
            "voice.example.com:9987",
            "--nickname",
            "cli-tester",
            "connect",
        }
    );
    tests::expect(connect_command.ok(), "connect parse should succeed");
    std::vector<std::string> connect_progress;
    auto connect_result = router.dispatch(connect_command.value(), [&](std::string_view message) {
        connect_progress.emplace_back(message);
    });
    tests::expect(connect_result.ok(), "connect dispatch should succeed");
    tests::expect_eq(connect_result.value().exit_code, domain::ExitCode::ok, "connect exit code");
    tests::expect_eq(connect_progress.size(), std::size_t(4), "connect should stream four progress updates");
    tests::expect_contains(
        connect_progress[0],
        "Connecting to voice.example.com:9987 as cli-tester using profile mock-local",
        "connect progress should start with contextual connection info"
    );
    tests::expect_contains(
        connect_progress[1],
        "TeamSpeak accepted the request to connect to voice.example.com:9987.",
        "connect progress should explain the request acceptance"
    );
    tests::expect_contains(
        connect_progress[2],
        "started establishing the server connection",
        "connect progress should describe the connecting phase"
    );
    tests::expect_contains(
        connect_progress[3],
        "connection is ready",
        "connect progress should describe the final connected phase"
    );
    const auto connect_json = output::render(connect_result.value(), output::Format::json);
    tests::expect_contains(connect_json, "\"result\":\"connected\"", "connect json should report success");
    tests::expect_contains(connect_json, "\"lifecycle\":[", "connect json should include the lifecycle");
    tests::expect_contains(
        connect_json,
        "\"type\":\"connection.connected\"",
        "connect json should include the terminal connection event"
    );
    const auto connect_table = output::render(connect_result.value(), output::Format::table);
    tests::expect_contains(
        connect_table,
        "Connected to voice.example.com:9987 as cli-tester.",
        "connect table should summarize the final result in prose"
    );
    tests::expect_contains(connect_table, "Connection Context", "connect table should render contextual details");
    tests::expect(
        connect_table.find("connection.requested") == std::string::npos,
        "connect table should not expose raw lifecycle event codes"
    );
    tests::expect(
        connect_table.find("What Happened") == std::string::npos,
        "connect table should not repeat streamed lifecycle narration"
    );

    auto expect_connect_server = [&](const std::string& server_value, const std::string& expected_target) {
        auto connect = parse_command(
            router,
            {
                "--profile",
                "mock-local",
                "--config",
                config_path.string(),
                "--server",
                server_value,
                "--nickname",
                "cli-tester",
                "connect",
            }
        );
        tests::expect(connect.ok(), "server override parse should succeed");

        std::vector<std::string> progress;
        auto result = router.dispatch(connect.value(), [&](std::string_view message) {
            progress.emplace_back(message);
        });
        tests::expect(result.ok(), "server override dispatch should succeed");
        tests::expect_eq(progress.size(), std::size_t(4), "server override should stream four progress updates");
        tests::expect_contains(
            progress[0],
            "Connecting to " + expected_target + " as cli-tester using profile mock-local",
            "server override progress should start with contextual connection info"
        );
        tests::expect_contains(
            progress[1],
            "TeamSpeak accepted the request to connect to " + expected_target + ".",
            "server override progress should explain the request acceptance"
        );
        tests::expect_contains(
            output::render(result.value(), output::Format::table),
            "Connected to " + expected_target + " as cli-tester.",
            "server override table should summarize the final result"
        );
    };

    expect_connect_server("[2001:db8::1]:10001", "[2001:db8::1]:10001");
    expect_connect_server("[2001:db8::2]", "[2001:db8::2]:9987");
    expect_connect_server("2001:db8::3", "2001:db8::3:9987");

    auto disconnect_command = parse_command(
        router,
        {
            "--profile",
            "mock-local",
            "--config",
            config_path.string(),
            "disconnect",
        }
    );
    tests::expect(disconnect_command.ok(), "disconnect parse should succeed");
    std::vector<std::string> disconnect_progress;
    auto disconnect_result = router.dispatch(disconnect_command.value(), [&](std::string_view message) {
        disconnect_progress.emplace_back(message);
    });
    tests::expect(disconnect_result.ok(), "disconnect dispatch should succeed");
    tests::expect_eq(disconnect_result.value().exit_code, domain::ExitCode::ok, "disconnect exit code");
    tests::expect_eq(
        disconnect_progress.size(), std::size_t(2), "disconnect should stream the request and final event"
    );
    tests::expect_contains(
        disconnect_progress[0],
        "Requesting disconnect from the current TeamSpeak server.",
        "disconnect should announce the outgoing request"
    );
    tests::expect_contains(
        disconnect_progress[1],
        "server connection is closed",
        "disconnect should narrate the disconnection event"
    );
    const auto disconnect_json = output::render(disconnect_result.value(), output::Format::json);
    tests::expect_contains(
        disconnect_json,
        "\"result\":\"disconnected\"",
        "disconnect json should report a disconnected result"
    );
    tests::expect_contains(
        disconnect_json,
        "\"type\":\"connection.disconnected\"",
        "disconnect json should include the terminal disconnect event"
    );
    const auto disconnect_table = output::render(disconnect_result.value(), output::Format::table);
    tests::expect_contains(
        disconnect_table,
        "Disconnected from 127.0.0.1:9987.",
        "disconnect table should summarize the outcome in prose"
    );
    tests::expect(
        disconnect_table.find("connection.disconnected") == std::string::npos,
        "disconnect table should not expose raw event codes"
    );

    auto invalid_message = parse_command(
        router,
        {
            "message",
            "send",
            "--target",
            "server",
            "--id",
            "1",
            "--text",
            "hello",
            "--config",
            config_path.string(),
        }
    );
    tests::expect(invalid_message.ok(), "invalid message parse should still succeed");
    auto invalid_result = router.dispatch(invalid_message.value());
    tests::expect(!invalid_result.ok(), "invalid message target should fail during dispatch");
    tests::expect_eq(
        invalid_result.error().exit_code, domain::ExitCode::usage, "invalid target exit code"
    );
    tests::expect_contains(
        output::render_error(invalid_result.error(), output::Format::table, false),
        "ts message send --help",
        "invalid message target should suggest the command help"
    );

    auto missing_profile = parse_command(
        router, {"profile", "use", "missing-profile", "--config", config_path.string()}
    );
    tests::expect(missing_profile.ok(), "missing profile parse should succeed");
    auto missing_profile_result = router.dispatch(missing_profile.value());
    tests::expect(!missing_profile_result.ok(), "missing profile dispatch should fail");
    tests::expect_contains(
        output::render_error(missing_profile_result.error(), output::Format::table, false),
        "ts profile list",
        "missing profile should suggest listing available profiles"
    );

    auto missing_channel = parse_command(
        router, {"channel", "get", "missing-channel", "--profile", "mock-local", "--config", config_path.string()}
    );
    tests::expect(missing_channel.ok(), "missing channel parse should succeed");
    auto missing_channel_result = router.dispatch(missing_channel.value());
    tests::expect(!missing_channel_result.ok(), "missing channel dispatch should fail");
    tests::expect_contains(
        output::render_error(missing_channel_result.error(), output::Format::table, false),
        "ts channel list",
        "missing channel should suggest listing channels"
    );

    auto channel_clients_all = parse_command(
        router, {"channel", "clients", "--profile", "mock-local", "--config", config_path.string()}
    );
    tests::expect(channel_clients_all.ok(), "channel clients parse should succeed");
    auto channel_clients_all_result = router.dispatch(channel_clients_all.value());
    tests::expect(channel_clients_all_result.ok(), "channel clients dispatch should succeed");
    const auto channel_clients_all_json = output::render(channel_clients_all_result.value(), output::Format::json);
    tests::expect_contains(
        channel_clients_all_json,
        "\"name\":\"Engineering\"",
        "channel clients json should include Engineering"
    );
    tests::expect_contains(
        channel_clients_all_json,
        "\"nickname\":\"bob\"",
        "channel clients json should include channel members"
    );
    tests::expect_contains(
        channel_clients_all_json,
        "\"name\":\"Breakout\"",
        "channel clients json should include empty channels"
    );
    tests::expect_contains(
        channel_clients_all_json,
        "\"clients\":[]",
        "channel clients json should preserve empty channel membership lists"
    );
    const auto channel_clients_all_table = output::render(channel_clients_all_result.value(), output::Format::table);
    tests::expect_contains(
        channel_clients_all_table,
        "Engineering",
        "channel clients table should include channel names"
    );
    tests::expect_contains(
        channel_clients_all_table,
        "bob",
        "channel clients table should include grouped client rows"
    );
    tests::expect_contains(
        channel_clients_all_table,
        "Breakout",
        "channel clients table should include empty channels"
    );

    auto channel_clients_one = parse_command(
        router,
        {"channel", "clients", "Engineering", "--profile", "mock-local", "--config", config_path.string()}
    );
    tests::expect(channel_clients_one.ok(), "channel clients with selector parse should succeed");
    auto channel_clients_one_result = router.dispatch(channel_clients_one.value());
    tests::expect(channel_clients_one_result.ok(), "channel clients with selector dispatch should succeed");
    const auto channel_clients_one_json = output::render(channel_clients_one_result.value(), output::Format::json);
    tests::expect_contains(
        channel_clients_one_json,
        "\"name\":\"Engineering\"",
        "channel clients selector json should include the requested channel"
    );
    tests::expect_contains(
        channel_clients_one_json,
        "\"nickname\":\"alice\"",
        "channel clients selector json should include matching members"
    );
    tests::expect_contains(
        channel_clients_one_json,
        "\"nickname\":\"bob\"",
        "channel clients selector json should include all members in the requested channel"
    );
    tests::expect(
        channel_clients_one_json.find("\"name\":\"Lobby\"") == std::string::npos,
        "channel clients selector json should not include other channels"
    );
    tests::expect(
        channel_clients_one_json.find("\"nickname\":\"terminal\"") == std::string::npos,
        "channel clients selector json should not include clients from other channels"
    );

    const fs::path launcher_path = temp_dir / "mock-client.sh";
    const fs::path socket_env_path = temp_dir / "client-socket.txt";
    {
        std::ofstream launcher(launcher_path, std::ios::trunc);
        launcher << "#!/usr/bin/env bash\n";
        launcher << "set -euo pipefail\n";
        launcher << "printf '%s\\n' \"${TS_CONTROL_SOCKET_PATH:-}\" >\"" << socket_env_path.string() << "\"\n";
        launcher << "trap 'exit 0' TERM INT\n";
        launcher << "while true; do sleep 1; done\n";
    }
    fs::permissions(
        launcher_path,
        fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
        fs::perm_options::replace
    );

    const fs::path state_home = temp_dir / "state";
    fs::create_directories(state_home);
    const fs::path pid_file = state_home / "teamspeak-cli" / "client.pid";
    const std::string simple_discovery_name = "__ts_cli_router_simple_" + std::to_string(::getpid());
    const std::string wrapped_discovery_name =
        "ts3client_linux_amd64_wrapped_" + std::to_string(::getpid());
    const std::string discovered_process_name =
        "ts3client_linux_amd64_discovered_" + std::to_string(::getpid());

    EnvGuard launcher_env("TS_CLIENT_LAUNCHER", launcher_path.string());
    EnvGuard state_env("XDG_STATE_HOME", state_home.string());
    EnvGuard simple_discovery_env("TS_CLIENT_DISCOVERY_NAME", simple_discovery_name);
    EnvGuard headless_env("TS_CLIENT_HEADLESS", "0");

    auto client_status = parse_command(router, {"client", "status"});
    tests::expect(client_status.ok(), "client status parse should succeed");
    auto client_status_before_start = router.dispatch(client_status.value());
    tests::expect(client_status_before_start.ok(), "client status before start should succeed");
    tests::expect_contains(
        output::render(client_status_before_start.value(), output::Format::json),
        "\"status\":\"not-running\"",
        "client status before start should report not running"
    );
    const auto client_status_before_table = output::render(client_status_before_start.value(), output::Format::table);
    tests::expect_contains(
        client_status_before_table,
        "The local TeamSpeak client is not running.",
        "client status should summarize the stopped state in prose"
    );
    tests::expect(
        client_status_before_table.find("Action") == std::string::npos,
        "client status should not render the old machine-style action field"
    );

    auto client_start = parse_command(router, {"client", "start", "--config", config_path.string()});
    tests::expect(client_start.ok(), "client start parse should succeed");
    std::vector<std::string> client_start_progress;
    auto client_start_result = router.dispatch(client_start.value(), [&](std::string_view message) {
        client_start_progress.emplace_back(message);
    });
    tests::expect(client_start_result.ok(), "client start dispatch should succeed");
    tests::expect_contains(
        output::render(client_start_result.value(), output::Format::json),
        "\"status\":\"started\"",
        "client start should report started"
    );
    tests::expect_contains(
        client_start_progress.front(),
        "Checking whether a local TeamSpeak client is already running.",
        "client start should begin by explaining the preflight check"
    );
    tests::expect_contains(
        client_start_progress[1],
        launch_socket_path.string(),
        "client start should mention the chosen control socket path"
    );
    tests::expect_contains(
        client_start_progress.back(),
        "The TeamSpeak client started as PID ",
        "client start should report the launched pid as progress"
    );
    const auto client_start_table = output::render(client_start_result.value(), output::Format::table);
    tests::expect_contains(
        client_start_table,
        "Started the local TeamSpeak client as PID",
        "client start should summarize the result in prose"
    );
    tests::expect(fs::exists(pid_file), "client start should write a pid file");

    std::ifstream pid_input(pid_file);
    int started_pid = 0;
    pid_input >> started_pid;
    tests::expect(started_pid > 0, "client start should record a positive pid");
    for (int attempt = 0; attempt < 20 && !fs::exists(socket_env_path); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    tests::expect(fs::exists(socket_env_path), "client start should export a socket path to the launched client");
    {
        std::ifstream socket_input(socket_env_path);
        std::string launched_socket_path;
        std::getline(socket_input, launched_socket_path);
        tests::expect_eq(
            launched_socket_path,
            launch_socket_path.string(),
            "client start should launch the client with the active profile socket path"
        );
    }

    auto client_status_running = router.dispatch(client_status.value());
    tests::expect(client_status_running.ok(), "client status while running should succeed");
    tests::expect_contains(
        output::render(client_status_running.value(), output::Format::json),
        "\"status\":\"running\"",
        "client status while running should report running"
    );

    auto client_start_again_result = router.dispatch(client_start.value());
    tests::expect(client_start_again_result.ok(), "second client start dispatch should succeed");
    tests::expect_contains(
        output::render(client_start_again_result.value(), output::Format::json),
        "\"status\":\"already-running\"",
        "second client start should report already running"
    );

    auto client_stop = parse_command(router, {"client", "stop"});
    tests::expect(client_stop.ok(), "client stop parse should succeed");
    std::vector<std::string> client_stop_progress;
    auto client_stop_result = router.dispatch(client_stop.value(), [&](std::string_view message) {
        client_stop_progress.emplace_back(message);
    });
    tests::expect(client_stop_result.ok(), "client stop dispatch should succeed");
    tests::expect_contains(
        output::render(client_stop_result.value(), output::Format::json),
        "\"status\":\"stopped\"",
        "client stop should report stopped"
    );
    tests::expect_contains(
        client_stop_progress.front(),
        "Looking for a running local TeamSpeak client process.",
        "client stop should explain the initial process lookup"
    );
    tests::expect_contains(
        client_stop_progress.back(),
        "Sending SIGTERM to the TeamSpeak client process group rooted at PID ",
        "client stop should narrate the shutdown signal"
    );
    const auto client_stop_table = output::render(client_stop_result.value(), output::Format::table);
    tests::expect_contains(
        client_stop_table,
        "Stopped the local TeamSpeak client process rooted at PID",
        "client stop should summarize the result in prose"
    );
    tests::expect(!fs::exists(pid_file), "client stop should remove the pid file");

    for (int attempt = 0; attempt < 20; ++attempt) {
        if (::kill(started_pid, 0) != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    tests::expect(::kill(started_pid, 0) != 0, "client stop should terminate the tracked process");

    auto client_stop_again_result = router.dispatch(client_stop.value());
    tests::expect(client_stop_again_result.ok(), "second client stop dispatch should succeed");
    tests::expect_contains(
        output::render(client_stop_again_result.value(), output::Format::json),
        "\"status\":\"not-running\"",
        "second client stop should report not running"
    );

    auto client_status_after_stop = router.dispatch(client_status.value());
    tests::expect(client_status_after_stop.ok(), "client status after stop should succeed");
    tests::expect_contains(
        output::render(client_status_after_stop.value(), output::Format::json),
        "\"status\":\"not-running\"",
        "client status after stop should report not running"
    );

    const fs::path wrapped_launcher_path = temp_dir / "wrapped-client.sh";
    {
        std::ofstream launcher(wrapped_launcher_path, std::ios::trunc);
        launcher << "#!/usr/bin/env bash\n";
        launcher << "set -euo pipefail\n";
        launcher << "/bin/bash -lc 'exec -a " << wrapped_discovery_name << " sleep 30'\n";
    }
    fs::permissions(
        wrapped_launcher_path,
        fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
        fs::perm_options::replace
    );

    {
        EnvGuard wrapped_launcher_env("TS_CLIENT_LAUNCHER", wrapped_launcher_path.string());
        EnvGuard wrapped_discovery_env("TS_CLIENT_DISCOVERY_NAME", wrapped_discovery_name);
        auto wrapped_start = router.dispatch(client_start.value());
        tests::expect(wrapped_start.ok(), "wrapped client start should succeed");
        std::ifstream wrapped_pid_input(pid_file);
        int wrapped_pid = 0;
        wrapped_pid_input >> wrapped_pid;
        tests::expect(wrapped_pid > 0, "wrapped client start should record a positive pid");

        auto wrapped_status = router.dispatch(client_status.value());
        tests::expect(wrapped_status.ok(), "wrapped client status should succeed");
        tests::expect_contains(
            output::render(wrapped_status.value(), output::Format::json),
            "\"status\":\"running\"",
            "wrapped client should be reported as running"
        );

        auto wrapped_stop = router.dispatch(client_stop.value());
        tests::expect(wrapped_stop.ok(), "wrapped client stop should succeed");
        const auto wrapped_stop_json = output::render(wrapped_stop.value(), output::Format::json);
        tests::expect_contains(
            wrapped_stop_json,
            "\"status\":\"stopped\"",
            "wrapped client stop should report stopped"
        );
        tests::expect_contains(
            wrapped_stop_json,
            "process group stopped with SIGTERM",
            "wrapped client stop should indicate group termination"
        );
        tests::expect(!fs::exists(pid_file), "wrapped client stop should remove the pid file");

        for (int attempt = 0; attempt < 20; ++attempt) {
            if (::kill(wrapped_pid, 0) != 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        tests::expect(::kill(wrapped_pid, 0) != 0, "wrapped client launcher should terminate");

        auto wrapped_status_after = router.dispatch(client_status.value());
        tests::expect(wrapped_status_after.ok(), "wrapped client status after stop should succeed");
        tests::expect_contains(
            output::render(wrapped_status_after.value(), output::Format::json),
            "\"status\":\"not-running\"",
            "wrapped client stop should leave no detected process"
        );
    }

    const pid_t discovered_child = ::fork();
    tests::expect(discovered_child >= 0, "untracked discovery child should fork");
    if (discovered_child == 0) {
        (void)::setsid();
        ::execl(
            "/bin/bash",
            "/bin/bash",
            "-lc",
            ("exec -a " + discovered_process_name + " sleep 30").c_str(),
            static_cast<char*>(nullptr)
        );
        _exit(127);
    }

    auto discovery_guard = [&] {
        if (::kill(discovered_child, 0) == 0) {
            ::kill(discovered_child, SIGTERM);
        }
        int status = 0;
        (void)::waitpid(discovered_child, &status, 0);
    };

    EnvGuard discovered_process_env("TS_CLIENT_DISCOVERY_NAME", discovered_process_name);
    teamspeak_cli::domain::Result<teamspeak_cli::output::CommandOutput> client_status_discovered =
        router.dispatch(client_status.value());
    std::string discovered_json;
    for (int attempt = 0; attempt < 40; ++attempt) {
        tests::expect(client_status_discovered.ok(), "client status with untracked process should succeed");
        discovered_json = output::render(client_status_discovered.value(), output::Format::json);
        if (discovered_json.find("\"status\":\"running\"") != std::string::npos) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        client_status_discovered = router.dispatch(client_status.value());
    }
    tests::expect_contains(
        discovered_json,
        "\"status\":\"running\"",
        "client status should detect an untracked running client process"
    );
    tests::expect_contains(
        discovered_json,
        "detected running TeamSpeak client without a ts pid file",
        "client status should explain that the process is untracked"
    );

    auto client_stop_discovered = router.dispatch(client_stop.value());
    tests::expect(client_stop_discovered.ok(), "client stop should stop an untracked discovered process");
    const auto stop_discovered_json = output::render(client_stop_discovered.value(), output::Format::json);
    tests::expect_contains(
        stop_discovered_json,
        "\"status\":\"stopped\"",
        "client stop should report stopped for a discovered process"
    );
    tests::expect_contains(
        stop_discovered_json,
        "stopping detected TeamSpeak client without a ts pid file",
        "client stop should explain that it stopped a discovered process"
    );
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (::kill(discovered_child, 0) != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    tests::expect(::kill(discovered_child, 0) != 0, "client stop should terminate a discovered process");
    discovery_guard();

    auto loaded_for_status = config_store.load(config_path);
    tests::expect(loaded_for_status.ok(), "config should load before status error test");
    auto plugin_profile_for_status = config_store.find_profile(loaded_for_status.value(), "plugin-local");
    tests::expect(plugin_profile_for_status.ok(), "plugin-local profile should exist for status error test");
    const fs::path stale_socket_path = temp_dir / "stale.sock";
    plugin_profile_for_status.value()->control_socket_path = stale_socket_path.string();
    auto saved_status_config = config_store.save(config_path, loaded_for_status.value());
    tests::expect(saved_status_config.ok(), "status error test config should save");

    SocketFile stale_socket(stale_socket_path);
    stale_socket.close();

    auto status_command = parse_command(router, {"status", "--config", config_path.string()});
    tests::expect(status_command.ok(), "status parse should succeed");
    auto status_result = router.dispatch(status_command.value());
    tests::expect(!status_result.ok(), "status dispatch should fail when the plugin bridge is unavailable");
    const auto status_error_table = output::render_error(status_result.error(), output::Format::table, false);
    tests::expect_contains(
        status_error_table,
        "ts client start",
        "status error should suggest launching the TeamSpeak client"
    );
    tests::expect_contains(
        status_error_table,
        "ts plugin info",
        "status error should suggest checking the plugin bridge"
    );
    const auto status_error_json = output::render_error(status_result.error(), output::Format::json, false);
    tests::expect_contains(
        status_error_json,
        "\"hints\":[",
        "status error json should include structured hints"
    );
    tests::expect_contains(
        status_error_json,
        "ts client start",
        "status error json should include the client start hint"
    );

    fs::remove_all(temp_dir);
    return 0;
}
