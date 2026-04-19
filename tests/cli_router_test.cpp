#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "teamspeak_cli/cli/command_router.hpp"
#include "teamspeak_cli/config/config_store.hpp"
#include "teamspeak_cli/output/render.hpp"
#include "test_support.hpp"

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

    const std::string channel_help = router.render_help({"channel"});
    tests::expect_contains(channel_help, "ts channel <subcommand>", "channel help usage");
    tests::expect_contains(channel_help, "Subcommands:", "channel help should list subcommands");
    tests::expect_contains(channel_help, "ts channel list", "channel help should include examples");

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

    const fs::path launcher_path = temp_dir / "fake-client.sh";
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

    auto client_start = parse_command(router, {"client", "start", "--config", config_path.string()});
    tests::expect(client_start.ok(), "client start parse should succeed");
    auto client_start_result = router.dispatch(client_start.value());
    tests::expect(client_start_result.ok(), "client start dispatch should succeed");
    tests::expect_contains(
        output::render(client_start_result.value(), output::Format::json),
        "\"status\":\"started\"",
        "client start should report started"
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
    auto client_stop_result = router.dispatch(client_stop.value());
    tests::expect(client_stop_result.ok(), "client stop dispatch should succeed");
    tests::expect_contains(
        output::render(client_stop_result.value(), output::Format::json),
        "\"status\":\"stopped\"",
        "client stop should report stopped"
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
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto client_status_discovered = router.dispatch(client_status.value());
    tests::expect(client_status_discovered.ok(), "client status with untracked process should succeed");
    const auto discovered_json = output::render(client_status_discovered.value(), output::Format::json);
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

    fs::remove_all(temp_dir);
    return 0;
}
