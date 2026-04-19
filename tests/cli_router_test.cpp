#include <filesystem>
#include <string>
#include <vector>

#include "teamspeak_cli/cli/command_router.hpp"
#include "teamspeak_cli/config/config_store.hpp"
#include "teamspeak_cli/output/render.hpp"
#include "test_support.hpp"

namespace fs = std::filesystem;

namespace {

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

    fs::remove_all(temp_dir);
    return 0;
}
