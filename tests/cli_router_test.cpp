#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "teamspeak_cli/cli/command_router.hpp"
#include "teamspeak_cli/config/config_store.hpp"
#include "teamspeak_cli/daemon/runtime.hpp"
#include "teamspeak_cli/output/render.hpp"
#include "test_support.hpp"

namespace teamspeak_cli::cli {
auto read_install_receipt_value_for_test(std::string_view value) -> std::string;
auto find_executable_with_fallbacks_for_test(
    std::string_view executable_name,
    const std::vector<std::filesystem::path>& fallback_paths
) -> std::optional<std::filesystem::path>;
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

auto normalize_json_timestamps(std::string value) -> std::string {
    constexpr std::string_view needle = "\"timestamp\":\"";
    std::size_t pos = 0;
    while ((pos = value.find(needle, pos)) != std::string::npos) {
        const auto timestamp_start = pos + needle.size();
        const auto timestamp_end = value.find('"', timestamp_start);
        if (timestamp_end == std::string::npos) {
            break;
        }
        value.replace(timestamp_start, timestamp_end - timestamp_start, "<timestamp>");
        pos = timestamp_start + std::string_view("<timestamp>").size();
    }
    return value;
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
    tests::expect_contains(
        help, "--output <table|json|yaml>  yaml is experimental", "top-level help should mark yaml experimental"
    );
    tests::expect_contains(help, "--field <path>", "top-level help should list field extraction");
    tests::expect_contains(help, "--no-headers", "top-level help should list no-header table output");
    tests::expect_contains(help, "--wide", "top-level help should list wide table output");
    tests::expect_contains(help, "mute  Mute your TeamSpeak microphone", "top-level help should list mute");
    tests::expect_contains(help, "away  Set your TeamSpeak status to away", "top-level help should list away");
    tests::expect_contains(
        help,
        "update  Update this release install from the official GitHub release",
        "top-level help should list update"
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
    {
        const fs::path temp_dir = fs::temp_directory_path() / "ts-cli-find-executable-fallback-test";
        std::error_code cleanup_ec;
        fs::remove_all(temp_dir, cleanup_ec);
        fs::create_directories(temp_dir);
        const fs::path fallback_executable_path = temp_dir / "ldconfig";
        {
            std::ofstream executable(fallback_executable_path, std::ios::trunc);
            executable << "#!/usr/bin/env bash\n";
            executable << "exit 0\n";
        }
        fs::permissions(
            fallback_executable_path,
            fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
            fs::perm_options::replace
        );
        EnvGuard empty_path_env("PATH", "");
        const auto fallback_resolution =
            cli::find_executable_with_fallbacks_for_test("ldconfig", {fallback_executable_path});
        tests::expect(
            fallback_resolution.has_value(),
            "executable lookup should resolve known fallback paths when PATH is empty"
        );
        tests::expect_eq(
            fallback_resolution->lexically_normal().string(),
            fallback_executable_path.lexically_normal().string(),
            "executable lookup should return the fallback executable path"
        );
        fs::remove_all(temp_dir, cleanup_ec);
    }

    const std::string channel_help = router.render_help({"channel"});
    tests::expect_contains(channel_help, "ts channel <subcommand>", "channel help usage");
    tests::expect_contains(
        channel_help,
        "Global options: --output (yaml experimental)",
        "command help should mark yaml output experimental"
    );
    tests::expect_contains(channel_help, "Subcommands:", "channel help should list subcommands");
    tests::expect_contains(channel_help, "ts channel list", "channel help should include examples");
    tests::expect_contains(channel_help, "ts channel clients [id-or-name]", "channel help should include channel clients");

    const std::string profile_help = router.render_help({"profile"});
    tests::expect_contains(profile_help, "create  Create a config profile", "profile help should list profile create");
    tests::expect_contains(
        profile_help,
        "ts profile create <name> [--copy-from <name>] [--activate]",
        "profile help should include the create example"
    );

    const std::string client_help = router.render_help({"client"});
    tests::expect_contains(client_help, "logs  Show recent TeamSpeak client logs", "client help should list client logs");
    tests::expect_contains(
        client_help,
        "ts client logs [--count N]",
        "client help should include the client logs example"
    );

    const std::string playback_help = router.render_help({"playback"});
    tests::expect_contains(
        playback_help,
        "status  Show media playback and audio routing diagnostics",
        "playback help should list playback status"
    );
    tests::expect_contains(
        playback_help,
        "send  Send a WAV file through the plugin media bridge",
        "playback help should list playback send"
    );
    tests::expect_contains(
        playback_help,
        "ts playback send --file <wav> [--clear] [--timeout-ms N]",
        "playback help should include the playback send example"
    );

    const std::string away_help = router.render_help({"away"});
    tests::expect_contains(away_help, "ts away [--message <text>]", "away help should include message usage");
    tests::expect_contains(
        away_help,
        "--message <text>  Away message to show in TeamSpeak. (default: empty message)",
        "away help should describe the message option"
    );

    const std::string update_help = router.render_help({"update"});
    tests::expect_contains(update_help, "ts update [--release-tag TAG]", "update help should include release tag usage");
    tests::expect_contains(
        update_help,
        "--release-tag <tag>  Install a specific GitHub release tag instead of the latest release. (default: latest release)",
        "update help should describe release tag default"
    );
    tests::expect_contains(update_help, "ts update --release-tag v1.2.3", "update help should include examples");

    const std::string status_help = router.render_help({"status"});
    tests::expect_contains(
        status_help,
        "JSON output is a single connection status object suitable for --field extraction.",
        "status help should include scriptable output note"
    );

    const std::string channel_list_help = router.render_help({"channel", "list"});
    tests::expect_contains(
        channel_list_help,
        "JSON output is an array of channel objects; table output supports --wide and --no-headers.",
        "channel list help should include output note"
    );

    const std::string client_logs_help = router.render_help({"client", "logs"});
    tests::expect_contains(
        client_logs_help,
        "--count <N>  Number of recent log lines to print. (accepted: positive integer; default: 80)",
        "client logs help should describe count default"
    );

    const std::string daemon_start_help = router.render_help({"daemon", "start"});
    tests::expect_contains(
        daemon_start_help,
        "--poll-ms <N>  Polling interval used by the event daemon. (accepted: positive integer milliseconds; default: 500)",
        "daemon start help should describe poll interval default"
    );

    const std::vector<std::string> grouped_commands = {
        "plugin", "sdk", "config", "profile", "server", "channel", "client", "message", "playback", "events"
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

    auto parsed_field = parse_command(router, {"--json", "status", "--field", "phase"});
    tests::expect(parsed_field.ok(), "json field parse should succeed after the command");
    tests::expect(parsed_field.value().global.field_path.has_value(), "field path should be stored globally");
    tests::expect_eq(
        *parsed_field.value().global.field_path,
        std::string("phase"),
        "field path should preserve the requested dot path"
    );

    auto parsed_nested_field = parse_command(
        router, {"--json", "plugin", "info", "--field", "media_diagnostics.transmit_path_ready"}
    );
    tests::expect(parsed_nested_field.ok(), "nested json field parse should succeed");
    tests::expect_eq(
        *parsed_nested_field.value().global.field_path,
        std::string("media_diagnostics.transmit_path_ready"),
        "nested field path should preserve dots"
    );

    auto parsed_table_options = parse_command(router, {"channel", "list", "--no-headers", "--wide"});
    tests::expect(parsed_table_options.ok(), "table option parse should succeed after the command");
    tests::expect(parsed_table_options.value().global.no_headers, "no-headers should be stored globally");
    tests::expect(parsed_table_options.value().global.wide, "wide should be stored globally");

    auto parsed_json_table_options = parse_command(router, {"--json", "channel", "list", "--wide", "--no-headers"});
    tests::expect(parsed_json_table_options.ok(), "json output should accept table options");
    tests::expect_eq(
        parsed_json_table_options.value().global.format,
        output::Format::json,
        "json table option parse should preserve json format"
    );

    auto table_field = parse_command(router, {"status", "--field", "phase"});
    tests::expect(!table_field.ok(), "field extraction without json should fail during parse");
    tests::expect_contains(
        table_field.error().message,
        "--field requires JSON output",
        "field extraction should require json output"
    );

    auto yaml_field = parse_command(router, {"--output", "yaml", "status", "--field", "phase"});
    tests::expect(!yaml_field.ok(), "field extraction with yaml should fail during parse");
    tests::expect_contains(
        yaml_field.error().message,
        "--field requires JSON output",
        "field extraction should reject yaml output"
    );

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
    std::error_code temp_cleanup_ec;
    fs::remove_all(temp_dir, temp_cleanup_ec);
    fs::create_directories(temp_dir);
    const fs::path config_path = temp_dir / "config.ini";

    {
        const fs::path update_prefix = temp_dir / "update-prefix";
        const fs::path update_client_dir = temp_dir / "update-client";
        const fs::path update_managed_dir = temp_dir / "update-managed";
        const fs::path update_config_path = temp_dir / "update-config.ini";
        const fs::path update_receipt_path = temp_dir / "update-receipt.env";
        const fs::path update_log_path = temp_dir / "update-installer-args.log";
        const fs::path fake_installer_path = temp_dir / "fake-install-release.sh";

        {
            std::ofstream receipt(update_receipt_path, std::ios::trunc);
            receipt << "receipt_version=1\n";
            receipt << "prefix=" << update_prefix.string() << '\n';
            receipt << "client_install_dir=" << update_client_dir.string() << '\n';
            receipt << "managed_dir=" << update_managed_dir.string() << '\n';
            receipt << "config_path=" << update_config_path.string() << '\n';
            receipt << "config_created_by_installer=0\n";
            receipt << "release_repo=example/fork\n";
            receipt << "release_tag=v0.0.1\n";
        }

        {
            std::ofstream fake_installer(fake_installer_path, std::ios::trunc);
            fake_installer << "#!/usr/bin/env bash\n";
            fake_installer << "set -euo pipefail\n";
            fake_installer << "log_path=\"${TS_UPDATE_TEST_LOG:?}\"\n";
            fake_installer << "repo=\"\"\n";
            fake_installer << "release_tag=\"vfake-latest\"\n";
            fake_installer << "prefix=\"\"\n";
            fake_installer << "client_dir=\"\"\n";
            fake_installer << "managed_dir=\"\"\n";
            fake_installer << "config_path=\"\"\n";
            fake_installer << ": >\"${log_path}\"\n";
            fake_installer << "for arg in \"$@\"; do printf '%s\\n' \"${arg}\" >>\"${log_path}\"; done\n";
            fake_installer << "while [[ $# -gt 0 ]]; do\n";
            fake_installer << "  case \"$1\" in\n";
            fake_installer << "    --repo) repo=\"$2\"; shift 2 ;;\n";
            fake_installer << "    --release-tag) release_tag=\"$2\"; shift 2 ;;\n";
            fake_installer << "    --prefix) prefix=\"$2\"; shift 2 ;;\n";
            fake_installer << "    --client-dir) client_dir=\"$2\"; shift 2 ;;\n";
            fake_installer << "    --managed-dir) managed_dir=\"$2\"; shift 2 ;;\n";
            fake_installer << "    --config-path) config_path=\"$2\"; shift 2 ;;\n";
            fake_installer << "    *) shift ;;\n";
            fake_installer << "  esac\n";
            fake_installer << "done\n";
            fake_installer << "receipt=\"${TS_UPDATE_RECEIPT_PATH:?}\"\n";
            fake_installer << "cat >\"${receipt}\" <<EOF\n";
            fake_installer << "receipt_version=1\n";
            fake_installer << "prefix=${prefix}\n";
            fake_installer << "client_install_dir=${client_dir}\n";
            fake_installer << "managed_dir=${managed_dir}\n";
            fake_installer << "config_path=${config_path}\n";
            fake_installer << "release_repo=${repo}\n";
            fake_installer << "release_tag=${release_tag}\n";
            fake_installer << "EOF\n";
        }

        EnvGuard receipt_env("TS_UPDATE_RECEIPT_PATH", update_receipt_path.string());
        EnvGuard installer_env("TS_UPDATE_INSTALLER_PATH", fake_installer_path.string());
        EnvGuard log_env("TS_UPDATE_TEST_LOG", update_log_path.string());

        auto update = parse_command(router, {"update", "--release-tag", "v9.8.7"});
        tests::expect(update.ok(), "update parse should succeed");
        tests::expect_eq(
            update.value().options.at("release-tag"),
            std::string("v9.8.7"),
            "update should parse release tag option"
        );

        std::vector<std::string> update_progress;
        auto update_result = router.dispatch(update.value(), [&](std::string_view message) {
            update_progress.emplace_back(message);
        });
        tests::expect(update_result.ok(), "update dispatch should succeed with a fake installer");
        tests::expect(
            !update_progress.empty(),
            "update should stream progress before invoking the release installer"
        );
        tests::expect_contains(
            output::render(update_result.value(), output::Format::json),
            "\"release_repo\":\"spi3/teamspeak-cli\"",
            "update should force the official release repo"
        );
        tests::expect_contains(
            output::render(update_result.value(), output::Format::json),
            "\"release_tag\":\"v9.8.7\"",
            "update should report the installed release tag from the refreshed receipt"
        );

        std::ifstream log_file(update_log_path);
        const std::string update_log(
            (std::istreambuf_iterator<char>(log_file)),
            std::istreambuf_iterator<char>()
        );
        tests::expect_contains(update_log, "--repo\nspi3/teamspeak-cli\n", "update should pass the official repo");
        tests::expect_contains(update_log, "--release-tag\nv9.8.7\n", "update should pass the requested tag");
        tests::expect_contains(update_log, "--prefix\n" + update_prefix.string() + "\n", "update should reuse prefix");
        tests::expect_contains(
            update_log,
            "--client-dir\n" + update_client_dir.string() + "\n",
            "update should reuse client dir"
        );
        tests::expect_contains(
            update_log,
            "--managed-dir\n" + update_managed_dir.string() + "\n",
            "update should reuse managed dir"
        );
        tests::expect_contains(
            update_log,
            "--config-path\n" + update_config_path.string() + "\n",
            "update should reuse config path"
        );
        tests::expect_contains(update_log, "--skip-config\n", "update should preserve skipped config installs");
    }

    auto init = parse_command(router, {"config", "init", "--config", config_path.string()});
    tests::expect(init.ok(), "config init parse should succeed");
    auto init_result = router.dispatch(init.value());
    tests::expect(init_result.ok(), "config init dispatch should succeed");
    tests::expect(fs::exists(config_path), "config init should write a config file");

    auto json_status = parse_command(
        router, {"status", "--profile", "mock-local", "--config", config_path.string()}
    );
    tests::expect(json_status.ok(), "json status parse should succeed");
    auto json_status_result = router.dispatch(json_status.value());
    tests::expect(json_status_result.ok(), "json status dispatch should succeed");
    tests::expect_eq(
        output::render(json_status_result.value(), output::Format::json),
        std::string("{\"backend\":\"mock\",\"connection\":\"42\",\"identity\":\"mock-local-identity\",\"mode\":\"one-shot\",\"nickname\":\"terminal\",\"phase\":\"connected\",\"port\":9987,\"profile\":\"mock-local\",\"server\":\"127.0.0.1\"}"),
        "status json should match the documented connection state shape"
    );

    auto json_plugin_info = parse_command(
        router, {"plugin", "info", "--profile", "mock-local", "--config", config_path.string()}
    );
    tests::expect(json_plugin_info.ok(), "json plugin info parse should succeed");
    auto json_plugin_info_result = router.dispatch(json_plugin_info.value());
    tests::expect(json_plugin_info_result.ok(), "json plugin info dispatch should succeed");
    tests::expect_eq(
        output::render(json_plugin_info_result.value(), output::Format::json),
        std::string("{\"backend\":\"mock\",\"media_diagnostics\":{\"active_speaker_count\":0,\"capture\":{\"device\":\"mock-capture\",\"is_default\":true,\"known\":true,\"mode\":\"mock\"},\"captured_voice_edit_attached\":false,\"consumer_connected\":false,\"custom_capture_device_id\":\"mock-capture-loop\",\"custom_capture_device_name\":\"Mock Media Bridge\",\"custom_capture_device_registered\":true,\"custom_capture_path_available\":true,\"dropped_audio_chunks\":0,\"dropped_playback_chunks\":0,\"injected_playback_attached_to_capture\":false,\"last_error\":\"\",\"playback\":{\"device\":\"mock-playback\",\"is_default\":true,\"known\":true,\"mode\":\"mock\"},\"playback_active\":false,\"pulse_sink\":\"\",\"pulse_source\":\"\",\"pulse_source_is_monitor\":false,\"queued_playback_samples\":0,\"transmit_path\":\"mock-capture-loop\",\"transmit_path_ready\":true},\"media_format\":\"pcm_s16le @48000 Hz mono\",\"media_socket_path\":\"\",\"media_transport\":\"\",\"note\":\"mock bridge host for local development and CI\",\"plugin_available\":true,\"plugin_name\":\"mock-plugin-host\",\"plugin_version\":\"development\",\"socket_path\":\"/run/user/1000/ts3cli.sock\",\"transport\":\"in-process\"}"),
        "plugin info json should match the documented object shape"
    );

    auto json_channel_list = parse_command(
        router, {"channel", "list", "--profile", "mock-local", "--config", config_path.string()}
    );
    tests::expect(json_channel_list.ok(), "json channel list parse should succeed");
    auto json_channel_list_result = router.dispatch(json_channel_list.value());
    tests::expect(json_channel_list_result.ok(), "json channel list dispatch should succeed");
    const std::string expected_channel_list_json =
        "[{\"client_count\":1,\"id\":\"1\",\"is_default\":true,\"name\":\"Lobby\",\"parent_id\":null,\"subscribed\":true},{\"client_count\":2,\"id\":\"2\",\"is_default\":false,\"name\":\"Engineering\",\"parent_id\":null,\"subscribed\":true},{\"client_count\":1,\"id\":\"3\",\"is_default\":false,\"name\":\"Operations\",\"parent_id\":null,\"subscribed\":true},{\"client_count\":0,\"id\":\"4\",\"is_default\":false,\"name\":\"Breakout\",\"parent_id\":\"2\",\"subscribed\":true}]";
    tests::expect_eq(
        output::render(json_channel_list_result.value(), output::Format::json),
        expected_channel_list_json,
        "channel list json should match the documented array shape"
    );
    const auto default_channel_list_table = output::render(json_channel_list_result.value(), output::Format::table);
    tests::expect_contains(
        default_channel_list_table,
        "ID  Name         Parent  Clients  Default",
        "default channel list table should keep existing headers"
    );
    tests::expect(
        default_channel_list_table.find("Subscribed") == std::string::npos,
        "default channel list table should not include wide columns"
    );

    auto wide_channel_list = parse_command(
        router, {"channel", "list", "--wide", "--profile", "mock-local", "--config", config_path.string()}
    );
    tests::expect(wide_channel_list.ok(), "wide channel list parse should succeed");
    auto wide_channel_list_result = router.dispatch(wide_channel_list.value());
    tests::expect(wide_channel_list_result.ok(), "wide channel list dispatch should succeed");
    const auto wide_channel_list_table = output::render(wide_channel_list_result.value(), output::Format::table);
    tests::expect_contains(
        wide_channel_list_table,
        "Subscribed",
        "wide channel list table should include subscription state"
    );
    tests::expect_eq(
        output::render(wide_channel_list_result.value(), output::Format::json),
        expected_channel_list_json,
        "wide channel list json should remain unchanged"
    );
    const auto no_header_channel_list_table = output::render(
        json_channel_list_result.value(),
        output::Format::table,
        output::TableRenderOptions{.show_headers = false}
    );
    tests::expect(
        no_header_channel_list_table.find("ID  Name") == std::string::npos,
        "no-header channel list table should omit headers"
    );
    tests::expect_contains(
        no_header_channel_list_table,
        "Lobby",
        "no-header channel list table should keep rows"
    );

    auto json_client_list = parse_command(
        router, {"client", "list", "--profile", "mock-local", "--config", config_path.string()}
    );
    tests::expect(json_client_list.ok(), "json client list parse should succeed");
    auto json_client_list_result = router.dispatch(json_client_list.value());
    tests::expect(json_client_list_result.ok(), "json client list dispatch should succeed");
    const std::string expected_client_list_json =
        "[{\"channel_id\":\"1\",\"id\":\"1\",\"nickname\":\"terminal\",\"self\":true,\"talking\":false,\"unique_identity\":\"mock-local-identity\"},{\"channel_id\":\"2\",\"id\":\"2\",\"nickname\":\"alice\",\"self\":false,\"talking\":false,\"unique_identity\":\"sdk-alice\"},{\"channel_id\":\"2\",\"id\":\"3\",\"nickname\":\"bob\",\"self\":false,\"talking\":true,\"unique_identity\":\"sdk-bob\"},{\"channel_id\":\"3\",\"id\":\"4\",\"nickname\":\"ops-bot\",\"self\":false,\"talking\":false,\"unique_identity\":\"sdk-ops-bot\"}]";
    tests::expect_eq(
        output::render(json_client_list_result.value(), output::Format::json),
        expected_client_list_json,
        "client list json should match the documented array shape"
    );
    const auto default_client_list_table = output::render(json_client_list_result.value(), output::Format::table);
    tests::expect_contains(
        default_client_list_table,
        "ID  Nickname",
        "default client list table should keep existing headers"
    );
    tests::expect(
        default_client_list_table.find("Unique Identity") == std::string::npos,
        "default client list table should not include unique identities"
    );

    auto wide_client_list = parse_command(
        router, {"client", "list", "--wide", "--profile", "mock-local", "--config", config_path.string()}
    );
    tests::expect(wide_client_list.ok(), "wide client list parse should succeed");
    auto wide_client_list_result = router.dispatch(wide_client_list.value());
    tests::expect(wide_client_list_result.ok(), "wide client list dispatch should succeed");
    tests::expect_contains(
        output::render(wide_client_list_result.value(), output::Format::table),
        "Unique Identity",
        "wide client list table should include unique identities"
    );
    tests::expect_eq(
        output::render(wide_client_list_result.value(), output::Format::json),
        expected_client_list_json,
        "wide client list json should remain unchanged"
    );

    auto create_profile = parse_command(
        router,
        {
            "profile",
            "create",
            "qa",
            "--copy-from",
            "mock-local",
            "--activate",
            "--config",
            config_path.string(),
        }
    );
    tests::expect(create_profile.ok(), "profile create parse should succeed");
    tests::expect_eq(
        create_profile.value().options.at("copy-from"), std::string("mock-local"), "profile create copy source"
    );
    tests::expect(create_profile.value().flags.contains("activate"), "profile create activate flag");
    auto create_profile_result = router.dispatch(create_profile.value());
    tests::expect(create_profile_result.ok(), "profile create dispatch should succeed");
    tests::expect_contains(
        output::render(create_profile_result.value(), output::Format::table),
        "created profile qa from mock-local and set it as the default",
        "profile create table output"
    );

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
    auto created_profile = config_store.find_profile(loaded.value(), "qa");
    tests::expect(created_profile.ok(), "created profile should persist");
    tests::expect_eq(created_profile.value()->backend, std::string("mock"), "created profile backend");
    tests::expect_eq(
        created_profile.value()->identity, std::string("mock-local-identity"), "created profile copied identity"
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
    tests::expect_eq(
        normalize_json_timestamps(connect_json),
        std::string("{\"connected\":true,\"lifecycle\":[{\"fields\":{\"port\":\"9987\",\"server\":\"voice.example.com\"},\"summary\":\"requested new mock TeamSpeak connection\",\"timestamp\":\"<timestamp>\",\"type\":\"connection.requested\"},{\"fields\":{\"port\":\"9987\",\"server\":\"voice.example.com\"},\"summary\":\"connection is starting\",\"timestamp\":\"<timestamp>\",\"type\":\"connection.connecting\"},{\"fields\":{\"port\":\"9987\",\"server\":\"voice.example.com\"},\"summary\":\"connected to mock TeamSpeak server\",\"timestamp\":\"<timestamp>\",\"type\":\"connection.connected\"}],\"result\":\"connected\",\"state\":{\"backend\":\"mock\",\"connection\":\"42\",\"identity\":\"mock-local-identity\",\"mode\":\"one-shot\",\"nickname\":\"cli-tester\",\"phase\":\"connected\",\"port\":9987,\"profile\":\"mock-local\",\"server\":\"voice.example.com\"},\"timed_out\":false,\"timeout_ms\":15000}"),
        "connect json should match the documented object shape"
    );
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
    tests::expect_eq(
        normalize_json_timestamps(disconnect_json),
        std::string("{\"disconnected\":true,\"lifecycle\":[{\"fields\":{},\"summary\":\"ts disconnect\",\"timestamp\":\"<timestamp>\",\"type\":\"connection.disconnected\"}],\"result\":\"disconnected\",\"state\":{\"backend\":\"mock\",\"connection\":\"0\",\"identity\":\"mock-local-identity\",\"mode\":\"one-shot\",\"nickname\":\"terminal\",\"phase\":\"disconnected\",\"port\":9987,\"profile\":\"mock-local\",\"server\":\"127.0.0.1\"},\"timed_out\":false,\"timeout_ms\":10000}"),
        "disconnect json should match the documented object shape"
    );
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

    auto mute_command = parse_command(
        router, {"mute", "--profile", "mock-local", "--config", config_path.string()}
    );
    tests::expect(mute_command.ok(), "mute parse should succeed");
    auto mute_result = router.dispatch(mute_command.value());
    tests::expect(mute_result.ok(), "mute dispatch should succeed");
    tests::expect_contains(
        output::render(mute_result.value(), output::Format::json),
        "\"muted\":true",
        "mute json should report muted"
    );
    tests::expect_contains(
        output::render(mute_result.value(), output::Format::table),
        "Muted your TeamSpeak microphone.",
        "mute table should summarize the action"
    );

    auto away_command = parse_command(
        router,
        {"away", "--message", "In a meeting", "--profile", "mock-local", "--config", config_path.string()}
    );
    tests::expect(away_command.ok(), "away parse should succeed");
    tests::expect_eq(
        away_command.value().options.at("message"),
        std::string("In a meeting"),
        "away parse should capture the away message"
    );
    auto away_result = router.dispatch(away_command.value());
    tests::expect(away_result.ok(), "away dispatch should succeed");
    const auto away_json = output::render(away_result.value(), output::Format::json);
    tests::expect_contains(away_json, "\"away\":true", "away json should report away");
    tests::expect_contains(
        away_json,
        "\"message\":\"In a meeting\"",
        "away json should include the away message"
    );
    tests::expect_contains(
        output::render(away_result.value(), output::Format::table),
        "Set your TeamSpeak status to away: In a meeting",
        "away table should summarize the action"
    );

    auto back_command = parse_command(
        router, {"back", "--profile", "mock-local", "--config", config_path.string()}
    );
    tests::expect(back_command.ok(), "back parse should succeed");
    auto back_result = router.dispatch(back_command.value());
    tests::expect(back_result.ok(), "back dispatch should succeed");
    tests::expect_contains(
        output::render(back_result.value(), output::Format::json),
        "\"away\":false",
        "back json should report away cleared"
    );

    auto unmute_command = parse_command(
        router, {"unmute", "--profile", "mock-local", "--config", config_path.string()}
    );
    tests::expect(unmute_command.ok(), "unmute parse should succeed");
    auto unmute_result = router.dispatch(unmute_command.value());
    tests::expect(unmute_result.ok(), "unmute dispatch should succeed");
    tests::expect_contains(
        output::render(unmute_result.value(), output::Format::json),
        "\"muted\":false",
        "unmute json should report unmuted"
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

    const std::string message_send_help = router.render_help({"message", "send"});
    tests::expect_contains(
        message_send_help,
        "--target <client|channel>  Message destination kind. (accepted: client, channel)",
        "message send help should describe accepted target values"
    );
    tests::expect_contains(
        message_send_help,
        "--text <message>  Message body to send.",
        "message send help should describe text"
    );

    auto playback_send = parse_command(
        router,
        {"playback", "send", "--file", "/tmp/message.wav", "--clear", "--timeout-ms", "5000"}
    );
    tests::expect(playback_send.ok(), "playback send parse should succeed");
    tests::expect_eq(playback_send.value().path.size(), std::size_t(2), "playback send path size");
    tests::expect_eq(playback_send.value().path[0], std::string("playback"), "playback send path head");
    tests::expect_eq(playback_send.value().path[1], std::string("send"), "playback send path tail");
    tests::expect_eq(
        playback_send.value().options.at("file"),
        std::string("/tmp/message.wav"),
        "playback send file option"
    );
    tests::expect_eq(
        playback_send.value().options.at("timeout-ms"),
        std::string("5000"),
        "playback send timeout option"
    );
    tests::expect(playback_send.value().flags.contains("clear"), "playback send clear flag");

    auto playback_status = parse_command(
        router,
        {"playback", "status", "--profile", "mock-local", "--config", config_path.string()}
    );
    tests::expect(playback_status.ok(), "playback status parse should succeed");
    auto playback_status_result = router.dispatch(playback_status.value());
    tests::expect(playback_status_result.ok(), "playback status dispatch should succeed");
    const auto playback_status_json = output::render(playback_status_result.value(), output::Format::json);
    tests::expect_eq(
        playback_status_json,
        std::string("{\"active_speaker_count\":0,\"capture\":{\"device\":\"mock-capture\",\"is_default\":true,\"known\":true,\"mode\":\"mock\"},\"captured_voice_edit_attached\":false,\"consumer_connected\":false,\"custom_capture_device_id\":\"mock-capture-loop\",\"custom_capture_device_name\":\"Mock Media Bridge\",\"custom_capture_device_registered\":true,\"custom_capture_path_available\":true,\"dropped_audio_chunks\":0,\"dropped_playback_chunks\":0,\"injected_playback_attached_to_capture\":false,\"last_error\":\"\",\"playback\":{\"device\":\"mock-playback\",\"is_default\":true,\"known\":true,\"mode\":\"mock\"},\"playback_active\":false,\"pulse_sink\":\"\",\"pulse_source\":\"\",\"pulse_source_is_monitor\":false,\"queued_playback_samples\":0,\"transmit_path\":\"mock-capture-loop\",\"transmit_path_ready\":true}"),
        "playback status json should match the documented media diagnostics shape"
    );
    tests::expect_contains(
        playback_status_json,
        "\"device\":\"mock-capture\"",
        "playback status json should include effective capture diagnostics"
    );
    tests::expect_contains(
        playback_status_json,
        "\"custom_capture_path_available\":true",
        "playback status json should include transmit path availability"
    );
    tests::expect_contains(
        output::render(playback_status_result.value(), output::Format::table),
        "Transmit path ready",
        "playback status table should summarize transmit readiness"
    );

    const std::string playback_send_help = router.render_help({"playback", "send"});
    tests::expect_contains(
        playback_send_help,
        "--timeout-ms <N>  How long to wait for the media bridge operation. (accepted: integer milliseconds; default: 60000)",
        "playback send help should describe timeout default"
    );

    auto events_watch_empty = parse_command(
        router,
        {"events", "watch", "--count", "1", "--timeout-ms", "1", "--profile", "mock-local", "--config", config_path.string()}
    );
    tests::expect(events_watch_empty.ok(), "events watch parse should succeed");
    auto events_watch_empty_result = router.dispatch(events_watch_empty.value());
    tests::expect(events_watch_empty_result.ok(), "events watch dispatch should succeed");
    tests::expect_eq(
        output::render(events_watch_empty_result.value(), output::Format::json),
        std::string("[]"),
        "events watch json should use an array top-level value when no events arrive"
    );

    const std::string events_watch_help = router.render_help({"events", "watch"});
    tests::expect_contains(
        events_watch_help,
        "--count <N>  Maximum number of events to collect before returning. (accepted: positive integer; default: 5)",
        "events watch help should describe count default"
    );
    tests::expect_contains(
        events_watch_help,
        "JSON output is an array of domain event objects, which may be empty on timeout.",
        "events watch help should include output note"
    );

    const std::string hook_add_help = router.render_help({"events", "hook", "add"});
    tests::expect_contains(
        hook_add_help,
        "--message-kind <client|channel|server>  Limit message hooks to one message kind. (accepted: client, channel, server)",
        "hook add help should describe message kind values"
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

    auto duplicate_profile = parse_command(
        router, {"profile", "create", "qa", "--config", config_path.string()}
    );
    tests::expect(duplicate_profile.ok(), "duplicate profile create parse should succeed");
    auto duplicate_profile_result = router.dispatch(duplicate_profile.value());
    tests::expect(!duplicate_profile_result.ok(), "duplicate profile create should fail");
    tests::expect_contains(
        output::render_error(duplicate_profile_result.error(), output::Format::table, false),
        "ts profile use <name>",
        "duplicate profile create should suggest using the existing profile"
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
    tests::expect_eq(
        channel_clients_all_json,
        std::string("[{\"channel\":{\"client_count\":1,\"id\":\"1\",\"is_default\":true,\"name\":\"Lobby\",\"parent_id\":null,\"subscribed\":true},\"clients\":[{\"channel_id\":\"1\",\"id\":\"1\",\"nickname\":\"terminal\",\"self\":true,\"talking\":false,\"unique_identity\":\"mock-local-identity\"}]},{\"channel\":{\"client_count\":2,\"id\":\"2\",\"is_default\":false,\"name\":\"Engineering\",\"parent_id\":null,\"subscribed\":true},\"clients\":[{\"channel_id\":\"2\",\"id\":\"2\",\"nickname\":\"alice\",\"self\":false,\"talking\":false,\"unique_identity\":\"sdk-alice\"},{\"channel_id\":\"2\",\"id\":\"3\",\"nickname\":\"bob\",\"self\":false,\"talking\":true,\"unique_identity\":\"sdk-bob\"}]},{\"channel\":{\"client_count\":1,\"id\":\"3\",\"is_default\":false,\"name\":\"Operations\",\"parent_id\":null,\"subscribed\":true},\"clients\":[{\"channel_id\":\"3\",\"id\":\"4\",\"nickname\":\"ops-bot\",\"self\":false,\"talking\":false,\"unique_identity\":\"sdk-ops-bot\"}]},{\"channel\":{\"client_count\":0,\"id\":\"4\",\"is_default\":false,\"name\":\"Breakout\",\"parent_id\":\"2\",\"subscribed\":true},\"clients\":[]}]"),
        "channel clients json should match the documented array-of-groups shape"
    );
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
    tests::expect(
        channel_clients_all_table.find("Unique Identity") == std::string::npos,
        "default channel clients table should not include wide columns"
    );

    auto wide_channel_clients = parse_command(
        router, {"channel", "clients", "--wide", "--profile", "mock-local", "--config", config_path.string()}
    );
    tests::expect(wide_channel_clients.ok(), "wide channel clients parse should succeed");
    auto wide_channel_clients_result = router.dispatch(wide_channel_clients.value());
    tests::expect(wide_channel_clients_result.ok(), "wide channel clients dispatch should succeed");
    const auto wide_channel_clients_table = output::render(wide_channel_clients_result.value(), output::Format::table);
    tests::expect_contains(
        wide_channel_clients_table,
        "Unique Identity",
        "wide channel clients table should include unique identities"
    );
    tests::expect_contains(
        wide_channel_clients_table,
        "sdk-bob",
        "wide channel clients table should include client identity values"
    );
    tests::expect_eq(
        output::render(wide_channel_clients_result.value(), output::Format::json),
        channel_clients_all_json,
        "wide channel clients json should remain unchanged"
    );

    auto channel_clients_one = parse_command(
        router,
        {"channel", "clients", "Engineering", "--profile", "mock-local", "--config", config_path.string()}
    );
    tests::expect(channel_clients_one.ok(), "channel clients with selector parse should succeed");
    auto channel_clients_one_result = router.dispatch(channel_clients_one.value());
    tests::expect(channel_clients_one_result.ok(), "channel clients with selector dispatch should succeed");
    const auto channel_clients_one_json = output::render(channel_clients_one_result.value(), output::Format::json);
    tests::expect_eq(
        channel_clients_one_json,
        std::string("{\"channel\":{\"client_count\":2,\"id\":\"2\",\"is_default\":false,\"name\":\"Engineering\",\"parent_id\":null,\"subscribed\":true},\"clients\":[{\"channel_id\":\"2\",\"id\":\"2\",\"nickname\":\"alice\",\"self\":false,\"talking\":false,\"unique_identity\":\"sdk-alice\"},{\"channel_id\":\"2\",\"id\":\"3\",\"nickname\":\"bob\",\"self\":false,\"talking\":true,\"unique_identity\":\"sdk-bob\"}]}"),
        "channel clients selector json should match the documented group object shape"
    );
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
    const fs::path home_dir = temp_dir / "home";
    fs::create_directories(home_dir);
    EnvGuard home_env("HOME", home_dir.string());
    EnvGuard simple_discovery_env("TS_CLIENT_DISCOVERY_NAME", simple_discovery_name);
    EnvGuard headless_env("TS_CLIENT_HEADLESS", "0");
    EnvGuard systemd_run_disabled_env("TS_CLIENT_SYSTEMD_RUN", "0");

    auto client_status = parse_command(router, {"client", "status"});
    tests::expect(client_status.ok(), "client status parse should succeed");
    auto client_logs = parse_command(router, {"client", "logs", "--count", "2"});
    tests::expect(client_logs.ok(), "client logs parse should succeed");
    tests::expect_eq(client_logs.value().options.at("count"), std::string("2"), "client logs count option");
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

    const fs::path client_log_path = state_home / "teamspeak-cli" / "client.log";
    {
        std::ofstream client_log(client_log_path, std::ios::trunc);
        client_log << "wrapper line 1\n";
        client_log << "wrapper line 2\n";
        client_log << "wrapper line 3\n";
    }
    const fs::path teamspeak_log_dir = home_dir / ".ts3client" / "logs";
    fs::create_directories(teamspeak_log_dir);
    const fs::path teamspeak_log_path = teamspeak_log_dir / "client_2026-04-21.log";
    {
        std::ofstream teamspeak_log(teamspeak_log_path, std::ios::trunc);
        teamspeak_log << "ts line 1\n";
        teamspeak_log << "ts line 2\n";
        teamspeak_log << "ts line 3\n";
    }

    auto client_logs_result = router.dispatch(client_logs.value());
    tests::expect(client_logs_result.ok(), "client logs dispatch should succeed");
    const auto client_logs_json = output::render(client_logs_result.value(), output::Format::json);
    tests::expect_contains(client_logs_json, "\"status\":\"found\"", "client logs should report found");
    tests::expect_contains(client_logs_json, "\"kind\":\"launcher\"", "client logs json should include launcher logs");
    tests::expect_contains(client_logs_json, "\"kind\":\"teamspeak\"", "client logs json should include teamspeak logs");
    tests::expect_contains(
        client_logs_json,
        client_log_path.string(),
        "client logs json should include the tracked launcher log path"
    );
    tests::expect_contains(
        client_logs_json,
        teamspeak_log_path.string(),
        "client logs json should include the TeamSpeak log path"
    );
    const auto client_logs_table = output::render(client_logs_result.value(), output::Format::table);
    tests::expect_contains(
        client_logs_table,
        "Recent TeamSpeak client logs (last 2 lines per file)",
        "client logs should summarize the tail view"
    );
    tests::expect_contains(client_logs_table, "wrapper line 2", "client logs should include the wrapper log tail");
    tests::expect_contains(client_logs_table, "wrapper line 3", "client logs should include the last wrapper line");
    tests::expect(
        client_logs_table.find("wrapper line 1") == std::string::npos,
        "client logs should trim older wrapper lines beyond --count"
    );
    tests::expect_contains(client_logs_table, "ts line 2", "client logs should include the TeamSpeak log tail");
    tests::expect_contains(client_logs_table, "ts line 3", "client logs should include the latest TeamSpeak log line");
    tests::expect(
        client_logs_table.find("ts line 1") == std::string::npos,
        "client logs should trim older TeamSpeak log lines beyond --count"
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
    {
        const fs::path fake_xvfb_path = temp_dir / "fake-xvfb.sh";
        const fs::path fake_xdotool_path = temp_dir / "fake-xdotool.sh";
        const fs::path fake_xdotool_log_path = temp_dir / "headless-xdotool.log";
        const fs::path fake_xdotool_phase_path = temp_dir / "headless-xdotool-phase.txt";
        const std::string fake_display = ":169";
        const fs::path fake_x11_socket_path = fs::path("/tmp/.X11-unix") / "X169";

        std::error_code socket_cleanup_ec;
        fs::remove(fake_x11_socket_path, socket_cleanup_ec);

        {
            std::ofstream xvfb(fake_xvfb_path, std::ios::trunc);
            xvfb << "#!/usr/bin/env bash\n";
            xvfb << "set -euo pipefail\n";
            xvfb << "display=\"${1:-}\"\n";
            xvfb << "socket_dir=\"/tmp/.X11-unix\"\n";
            xvfb << "socket_path=\"${socket_dir}/X${display#:}\"\n";
            xvfb << "mkdir -p \"${socket_dir}\"\n";
            xvfb << ": >\"${socket_path}\"\n";
            xvfb << "trap 'rm -f \"${socket_path}\"; exit 0' TERM INT EXIT\n";
            xvfb << "while true; do sleep 1; done\n";
        }
        fs::permissions(
            fake_xvfb_path,
            fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
            fs::perm_options::replace
        );

        {
            std::ofstream phase(fake_xdotool_phase_path, std::ios::trunc);
            phase << "license\n";
        }
        {
            std::ofstream xdotool(fake_xdotool_path, std::ios::trunc);
            xdotool << "#!/usr/bin/env bash\n";
            xdotool << "set -euo pipefail\n";
            xdotool << "log_path=\"" << fake_xdotool_log_path.string() << "\"\n";
            xdotool << "phase_path=\"" << fake_xdotool_phase_path.string() << "\"\n";
            xdotool << "printf '%s\\n' \"$*\" >>\"${log_path}\"\n";
            xdotool << "command=\"$1\"\n";
            xdotool << "shift || true\n";
            xdotool << "case \"${command}\" in\n";
            xdotool << "  search)\n";
            xdotool << "    title=\"${!#}\"\n";
            xdotool << "    phase=\"$(cat \"${phase_path}\" 2>/dev/null || printf 'license')\"\n";
            xdotool << "    case \"${title}:${phase}\" in\n";
            xdotool << "      'License agreement:license') printf 'headless-license\\n' ;;\n";
            xdotool << "      'Warning:warning') printf 'headless-warning\\n' ;;\n";
            xdotool << "      'Identities:identities') printf 'headless-identities\\n' ;;\n";
            xdotool << "    esac\n";
            xdotool << "    ;;\n";
            xdotool << "  getwindowgeometry)\n";
            xdotool << "    printf 'WIDTH=740\\nHEIGHT=700\\n'\n";
            xdotool << "    ;;\n";
            xdotool << "  mousemove)\n";
            xdotool << "    if [[ \"$*\" == *'headless-license'* ]]; then\n";
            xdotool << "      printf 'warning\\n' >\"${phase_path}\"\n";
            xdotool << "    fi\n";
            xdotool << "    ;;\n";
            xdotool << "  key)\n";
            xdotool << "    if [[ \"$*\" == *'headless-warning Escape'* ]]; then\n";
            xdotool << "      printf 'identities\\n' >\"${phase_path}\"\n";
            xdotool << "    elif [[ \"$*\" == *'headless-identities Escape'* ]]; then\n";
            xdotool << "      printf 'done\\n' >\"${phase_path}\"\n";
            xdotool << "    fi\n";
            xdotool << "    ;;\n";
            xdotool << "esac\n";
        }
        fs::permissions(
            fake_xdotool_path,
            fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
            fs::perm_options::replace
        );

        EnvGuard forced_headless_env("TS_CLIENT_HEADLESS", "1");
        EnvGuard fake_xvfb_env("TS_CLIENT_XVFB", fake_xvfb_path.string());
        EnvGuard fake_display_env("TS_CLIENT_HEADLESS_DISPLAY", fake_display);
        EnvGuard fake_xdotool_env("TS_CLIENT_XDOTOOL", fake_xdotool_path.string());
        EnvGuard fake_xdotool_library_env("TS_CLIENT_XDOTOOL_LIBRARY_PATH", temp_dir.string());

        std::vector<std::string> headless_start_progress;
        auto headless_start_result = router.dispatch(client_start.value(), [&](std::string_view message) {
            headless_start_progress.emplace_back(message);
        });
        tests::expect(headless_start_result.ok(), "headless client start should succeed");
        tests::expect(
            headless_start_progress.size() >= std::size_t(4),
            "headless client start should emit the expected progress updates"
        );
        tests::expect_contains(
            headless_start_progress[2],
            "DISPLAY :169",
            "headless client start should report the chosen Xvfb display"
        );

        std::ifstream xdotool_log_input(fake_xdotool_log_path);
        const std::string xdotool_log(
            (std::istreambuf_iterator<char>(xdotool_log_input)),
            std::istreambuf_iterator<char>()
        );
        tests::expect_contains(
            xdotool_log,
            "search --name Warning",
            "headless client start should look for the first-run warning dialog"
        );
        tests::expect_contains(
            xdotool_log,
            "key --window headless-warning Escape",
            "headless client start should dismiss the first-run warning dialog"
        );
        tests::expect_contains(
            xdotool_log,
            "key --window headless-identities Escape",
            "headless client start should continue dismissing the identities dialog"
        );

        auto headless_stop_result = router.dispatch(client_stop.value());
        tests::expect(headless_stop_result.ok(), "headless client stop should succeed");
        fs::remove(fake_x11_socket_path, socket_cleanup_ec);
    }
    {
        const fs::path managed_xvfb_path =
            home_dir / ".cache" / "teamspeak-cli" / "install" / "xvfb" / "root" / "usr" / "bin" / "Xvfb";
        const fs::path managed_launcher_path = temp_dir / "managed-xvfb-client.sh";
        const std::string managed_display = ":167";
        const fs::path managed_x11_socket_path = fs::path("/tmp/.X11-unix") / "X167";
        std::error_code managed_socket_cleanup_ec;
        fs::remove(managed_x11_socket_path, managed_socket_cleanup_ec);
        fs::create_directories(managed_xvfb_path.parent_path());

        {
            std::ofstream xvfb(managed_xvfb_path, std::ios::trunc);
            xvfb << "#!/bin/sh\n";
            xvfb << "set -eu\n";
            xvfb << "display=\"${1:-}\"\n";
            xvfb << "socket_dir=\"/tmp/.X11-unix\"\n";
            xvfb << "socket_path=\"${socket_dir}/X${display#:}\"\n";
            xvfb << "/bin/mkdir -p \"${socket_dir}\"\n";
            xvfb << ": >\"${socket_path}\"\n";
            xvfb << "trap 'rm -f \"${socket_path}\"; exit 0' TERM INT EXIT\n";
            xvfb << "while true; do /bin/sleep 1; done\n";
        }
        fs::permissions(
            managed_xvfb_path,
            fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
            fs::perm_options::replace
        );

        {
            std::ofstream launcher(managed_launcher_path, std::ios::trunc);
            launcher << "#!/bin/sh\n";
            launcher << "set -eu\n";
            launcher << "trap 'exit 0' TERM INT\n";
            launcher << "while true; do /bin/sleep 1; done\n";
        }
        fs::permissions(
            managed_launcher_path,
            fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
            fs::perm_options::replace
        );

        EnvGuard managed_launcher_env("TS_CLIENT_LAUNCHER", managed_launcher_path.string());
        EnvGuard forced_headless_env("TS_CLIENT_HEADLESS", "1");
        EnvGuard managed_display_env("TS_CLIENT_HEADLESS_DISPLAY", managed_display);
        EnvGuard no_xvfb_path_env("PATH", (temp_dir / "path-without-xvfb").string());

        std::vector<std::string> managed_headless_progress;
        auto managed_headless_start = router.dispatch(client_start.value(), [&](std::string_view message) {
            managed_headless_progress.emplace_back(message);
        });
        tests::expect(managed_headless_start.ok(), "managed Xvfb client start should succeed");
        tests::expect(
            managed_headless_progress.size() >= std::size_t(3),
            "managed Xvfb client start should emit the headless display progress update"
        );
        tests::expect_contains(
            managed_headless_progress[2],
            "DISPLAY :167",
            "headless client start should use the managed Xvfb fallback when Xvfb is not on PATH"
        );

        auto managed_headless_stop = router.dispatch(client_stop.value());
        tests::expect(managed_headless_stop.ok(), "managed Xvfb client stop should succeed");
        fs::remove(managed_x11_socket_path, managed_socket_cleanup_ec);
    }

    const fs::path missing_library_client_dir = temp_dir / "missing-library-client";
    const fs::path missing_library_launcher_path = missing_library_client_dir / "ts3client_runscript.sh";
    const fs::path missing_library_marker_path = temp_dir / "missing-library-runscript.txt";
    fs::create_directories(missing_library_client_dir);
    {
        std::ofstream launcher(missing_library_launcher_path, std::ios::trunc);
        launcher << "#!/usr/bin/env bash\n";
        launcher << "set -euo pipefail\n";
        launcher << "printf 'RUNSCRIPT_EXECUTED\\n' >\"" << missing_library_marker_path.string() << "\"\n";
    }
    fs::permissions(
        missing_library_launcher_path,
        fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
        fs::perm_options::replace
    );

    const fs::path fake_ldconfig_dir = temp_dir / "fake-ldconfig-bin";
    const fs::path fake_ldconfig_path = fake_ldconfig_dir / "ldconfig";
    fs::create_directories(fake_ldconfig_dir);
    {
        std::ofstream ldconfig(fake_ldconfig_path, std::ios::trunc);
        ldconfig << "#!/usr/bin/env bash\n";
        ldconfig << "set -euo pipefail\n";
        ldconfig << "printf '0 libs found in cache `/etc/ld.so.cache`\\n'\n";
    }
    fs::permissions(
        fake_ldconfig_path,
        fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
        fs::perm_options::replace
    );

    {
        EnvGuard missing_library_launcher_env("TS_CLIENT_LAUNCHER", missing_library_launcher_path.string());
        EnvGuard fake_path_env("PATH", "/usr/bin:/bin");
        EnvGuard fake_ldconfig_env("TS3_CLIENT_LDCONFIG", fake_ldconfig_path.string());

        auto missing_library_start = router.dispatch(client_start.value());
        tests::expect(!missing_library_start.ok(), "client start should fail when libXi.so.6 is unavailable");
        tests::expect_eq(
            missing_library_start.error().code,
            std::string("missing_runtime_library"),
            "client start should return the missing runtime library error"
        );
        const auto missing_library_table =
            output::render_error(missing_library_start.error(), output::Format::table, false);
        tests::expect_contains(
            missing_library_table,
            "libXi.so.6",
            "client start error should name the missing shared library"
        );
        tests::expect_contains(
            missing_library_table,
            "runtime-libs",
            "client start error should explain how to provide the runtime library"
        );
        tests::expect(
            !fs::exists(missing_library_marker_path),
            "client start preflight should stop before executing the TeamSpeak runscript when ldconfig is outside PATH"
        );
    }

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

    {
        const fs::path fake_systemd_dir = temp_dir / "fake-systemd-bin";
        const fs::path fake_systemd_state_dir = temp_dir / "fake-systemd-state";
        const fs::path fake_systemd_run_path = fake_systemd_dir / "systemd-run";
        const fs::path fake_systemctl_path = fake_systemd_dir / "systemctl";
        fs::create_directories(fake_systemd_dir);
        fs::create_directories(fake_systemd_state_dir);

        {
            std::ofstream systemd_run(fake_systemd_run_path, std::ios::trunc);
            systemd_run << "#!/usr/bin/env bash\n";
            systemd_run << "set -euo pipefail\n";
            systemd_run << "state_dir=\"${TS_FAKE_SYSTEMD_STATE_DIR:?}\"\n";
            systemd_run << "unit_name=\"\"\n";
            systemd_run << "args=()\n";
            systemd_run << "while (($#)); do\n";
            systemd_run << "  if [[ \"$1\" == *$'\\n'* ]]; then\n";
            systemd_run << "    exit 1\n";
            systemd_run << "  fi\n";
            systemd_run << "  case \"$1\" in\n";
            systemd_run << "    --unit=*) unit_name=\"${1#--unit=}\" ;;\n";
            systemd_run << "    --setenv=*) export \"${1#--setenv=}\" ;;\n";
            systemd_run << "    --user|--collect|--quiet|--same-dir) ;;\n";
            systemd_run << "    --service-type=*|--property=*|--description=*) ;;\n";
            systemd_run << "    *) args=(\"$@\") ; break ;;\n";
            systemd_run << "  esac\n";
            systemd_run << "  shift\n";
            systemd_run << "done\n";
            systemd_run << "printf '%s\\n' \"${unit_name}\" >\"${state_dir}/unit\"\n";
            systemd_run << "\"${args[@]}\" >/dev/null 2>&1 &\n";
            systemd_run << "child_pid=$!\n";
            systemd_run << "printf '%s\\n' \"${child_pid}\" >\"${state_dir}/mainpid\"\n";
            systemd_run << "exit 0\n";
        }
        fs::permissions(
            fake_systemd_run_path,
            fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
            fs::perm_options::replace
        );

        {
            std::ofstream systemctl(fake_systemctl_path, std::ios::trunc);
            systemctl << "#!/usr/bin/env bash\n";
            systemctl << "set -euo pipefail\n";
            systemctl << "state_dir=\"${TS_FAKE_SYSTEMD_STATE_DIR:?}\"\n";
            systemctl << "if [[ \"${1:-}\" == \"--user\" ]]; then\n";
            systemctl << "  shift\n";
            systemctl << "fi\n";
            systemctl << "command_name=\"${1:-}\"\n";
            systemctl << "shift || true\n";
            systemctl << "case \"${command_name}\" in\n";
            systemctl << "  show-environment)\n";
            systemctl << "    printf 'XDG_RUNTIME_DIR=/run/user/test\\n'\n";
            systemctl << "    ;;\n";
            systemctl << "  show)\n";
            systemctl << "    while (($#)); do\n";
            systemctl << "      case \"$1\" in\n";
            systemctl << "        --property=*|--value) shift ;;\n";
            systemctl << "        *) unit_name=\"$1\" ; shift ; break ;;\n";
            systemctl << "      esac\n";
            systemctl << "    done\n";
            systemctl << "    pid=0\n";
            systemctl << "    if [[ -f \"${state_dir}/mainpid\" ]]; then\n";
            systemctl << "      pid=\"$(cat \"${state_dir}/mainpid\")\"\n";
            systemctl << "    fi\n";
            systemctl << "    if [[ \"${pid}\" != \"0\" ]] && kill -0 \"${pid}\" >/dev/null 2>&1; then\n";
            systemctl << "      printf 'ActiveState=active\\nSubState=running\\nMainPID=%s\\n' \"${pid}\"\n";
            systemctl << "    else\n";
            systemctl << "      printf 'ActiveState=inactive\\nSubState=dead\\nMainPID=0\\n'\n";
            systemctl << "    fi\n";
            systemctl << "    ;;\n";
            systemctl << "  stop)\n";
            systemctl << "    unit_name=\"${1:-}\"\n";
            systemctl << "    printf '%s\\n' \"${unit_name}\" >\"${state_dir}/stopped-unit\"\n";
            systemctl << "    if [[ -f \"${state_dir}/mainpid\" ]]; then\n";
            systemctl << "      pid=\"$(cat \"${state_dir}/mainpid\")\"\n";
            systemctl << "      kill \"${pid}\" >/dev/null 2>&1 || true\n";
            systemctl << "      for _ in $(seq 1 100); do\n";
            systemctl << "        if ! kill -0 \"${pid}\" >/dev/null 2>&1; then\n";
            systemctl << "          break\n";
            systemctl << "        fi\n";
            systemctl << "        sleep 0.05\n";
            systemctl << "      done\n";
            systemctl << "      rm -f \"${state_dir}/mainpid\"\n";
            systemctl << "    fi\n";
            systemctl << "    ;;\n";
            systemctl << "  *)\n";
            systemctl << "    exit 1\n";
            systemctl << "    ;;\n";
            systemctl << "esac\n";
        }
        fs::permissions(
            fake_systemctl_path,
            fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
            fs::perm_options::replace
        );

        const fs::path fake_systemd_xvfb_path = temp_dir / "fake-systemd-xvfb.sh";
        const std::string fake_systemd_display = ":168";
        const fs::path fake_systemd_socket_path = fs::path("/tmp/.X11-unix") / "X168";
        std::error_code systemd_socket_cleanup_ec;
        fs::remove(fake_systemd_socket_path, systemd_socket_cleanup_ec);
        {
            std::ofstream xvfb(fake_systemd_xvfb_path, std::ios::trunc);
            xvfb << "#!/usr/bin/env bash\n";
            xvfb << "set -euo pipefail\n";
            xvfb << "display=\"${1:-}\"\n";
            xvfb << "socket_dir=\"/tmp/.X11-unix\"\n";
            xvfb << "socket_path=\"${socket_dir}/X${display#:}\"\n";
            xvfb << "mkdir -p \"${socket_dir}\"\n";
            xvfb << ": >\"${socket_path}\"\n";
            xvfb << "trap 'rm -f \"${socket_path}\"; exit 0' TERM INT EXIT\n";
            xvfb << "while true; do sleep 1; done\n";
        }
        fs::permissions(
            fake_systemd_xvfb_path,
            fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
            fs::perm_options::replace
        );

        const std::string inherited_path = []() -> std::string {
            if (const char* current_path = std::getenv("PATH"); current_path != nullptr) {
                return current_path;
            }
            return "/usr/bin:/bin";
        }();
        EnvGuard forced_systemd_run_env("TS_CLIENT_SYSTEMD_RUN", "1");
        EnvGuard fake_systemd_state_env("TS_FAKE_SYSTEMD_STATE_DIR", fake_systemd_state_dir.string());
        EnvGuard fake_path_env("PATH", fake_systemd_dir.string() + ":" + inherited_path);

        auto systemd_start = router.dispatch(client_start.value());
        tests::expect(systemd_start.ok(), "systemd-backed client start should succeed");
        const auto systemd_start_json = output::render(systemd_start.value(), output::Format::json);
        tests::expect_contains(
            systemd_start_json,
            "transient user systemd unit",
            "systemd-backed client start should explain the launch strategy"
        );
        tests::expect(fs::exists(pid_file), "systemd-backed client start should still write a pid file");
        const fs::path unit_file = state_home / "teamspeak-cli" / "client.unit";
        tests::expect(fs::exists(unit_file), "systemd-backed client start should write a unit file");

        std::ifstream systemd_pid_input(pid_file);
        int systemd_pid = 0;
        systemd_pid_input >> systemd_pid;
        tests::expect(systemd_pid > 0, "systemd-backed client start should record the unit main pid");

        std::ifstream unit_input(unit_file);
        std::string unit_name;
        std::getline(unit_input, unit_name);
        tests::expect_contains(
            unit_name,
            ".service",
            "systemd-backed client start should persist the transient unit name"
        );

        auto systemd_status = router.dispatch(client_status.value());
        tests::expect(systemd_status.ok(), "systemd-backed client status should succeed");
        const auto systemd_status_json = output::render(systemd_status.value(), output::Format::json);
        tests::expect_contains(
            systemd_status_json,
            "\"status\":\"running\"",
            "systemd-backed client status should report running"
        );
        tests::expect_contains(
            systemd_status_json,
            "tracked transient user systemd unit",
            "systemd-backed client status should mention the tracked unit"
        );

        auto systemd_stop = router.dispatch(client_stop.value());
        tests::expect(systemd_stop.ok(), "systemd-backed client stop should succeed");
        const auto systemd_stop_json = output::render(systemd_stop.value(), output::Format::json);
        tests::expect_contains(
            systemd_stop_json,
            "stopped transient user systemd unit",
            "systemd-backed client stop should mention the tracked unit"
        );
        tests::expect(!fs::exists(pid_file), "systemd-backed client stop should remove the pid file");
        tests::expect(!fs::exists(unit_file), "systemd-backed client stop should remove the unit file");

        const fs::path stopped_unit_path = fake_systemd_state_dir / "stopped-unit";
        tests::expect(fs::exists(stopped_unit_path), "systemd-backed client stop should call systemctl stop");
        for (int attempt = 0; attempt < 20; ++attempt) {
            if (::kill(systemd_pid, 0) != 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        tests::expect(::kill(systemd_pid, 0) != 0, "systemd-backed client stop should terminate the tracked process");

        {
            EnvGuard forced_headless_env("TS_CLIENT_HEADLESS", "1");
            EnvGuard fake_xvfb_env("TS_CLIENT_XVFB", fake_systemd_xvfb_path.string());
            EnvGuard fake_display_env("TS_CLIENT_HEADLESS_DISPLAY", fake_systemd_display);

            auto headless_systemd_start = router.dispatch(client_start.value());
            tests::expect(headless_systemd_start.ok(), "headless systemd-backed client start should succeed");
            const auto headless_systemd_start_json =
                output::render(headless_systemd_start.value(), output::Format::json);
            tests::expect_contains(
                headless_systemd_start_json,
                "DISPLAY :168",
                "headless systemd-backed client start should report the selected Xvfb display"
            );
            tests::expect_contains(
                headless_systemd_start_json,
                "transient user systemd unit",
                "headless systemd-backed client start should explain the launch strategy"
            );
            tests::expect(fs::exists(pid_file), "headless systemd-backed client start should write a pid file");
            tests::expect(fs::exists(unit_file), "headless systemd-backed client start should write a unit file");

            auto headless_systemd_stop = router.dispatch(client_stop.value());
            tests::expect(headless_systemd_stop.ok(), "headless systemd-backed client stop should succeed");
            tests::expect(!fs::exists(pid_file), "headless systemd-backed client stop should remove the pid file");
            tests::expect(!fs::exists(unit_file), "headless systemd-backed client stop should remove the unit file");
        }

        fs::remove(fake_systemd_socket_path, systemd_socket_cleanup_ec);
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

    const fs::path daemon_state_dir = temp_dir / "daemon-state";
    EnvGuard daemon_state_env("TS_DAEMON_STATE_DIR", daemon_state_dir.string());

    auto hook_add = parse_command(
        router,
        {
            "events",
            "hook",
            "add",
            "--type",
            "message.received",
            "--exec",
            "printf daemon-hook",
            "--message-kind",
            "client",
        }
    );
    tests::expect(hook_add.ok(), "events hook add parse should succeed");
    auto hook_add_result = router.dispatch(hook_add.value());
    tests::expect(hook_add_result.ok(), "events hook add dispatch should succeed");
    const auto hook_add_json = output::render(hook_add_result.value(), output::Format::json);
    tests::expect_contains(hook_add_json, "\"type\":\"message.received\"", "hook add should return the event type");
    tests::expect_contains(hook_add_json, "\"message_kind\":\"client\"", "hook add should return the message kind");

    auto hook_list = parse_command(router, {"events", "hook", "list"});
    tests::expect(hook_list.ok(), "events hook list parse should succeed");
    auto hook_list_result = router.dispatch(hook_list.value());
    tests::expect(hook_list_result.ok(), "events hook list dispatch should succeed");
    const auto hook_list_table = output::render(hook_list_result.value(), output::Format::table);
    tests::expect_contains(hook_list_table, "message.received", "hook list should include the event type");
    tests::expect_contains(hook_list_table, "client", "hook list should include the message kind");
    auto wide_hook_list = parse_command(router, {"events", "hook", "list", "--wide"});
    tests::expect(wide_hook_list.ok(), "wide events hook list parse should succeed");
    auto wide_hook_list_result = router.dispatch(wide_hook_list.value());
    tests::expect(wide_hook_list_result.ok(), "wide events hook list dispatch should succeed");
    tests::expect_eq(
        output::render(wide_hook_list_result.value(), output::Format::table),
        hook_list_table,
        "wide events hook list should be a no-op because the table already exposes all hook fields"
    );

    const auto daemon_paths = daemon::state_paths_for(daemon_state_dir);
    auto appended_inbox = daemon::append_inbox_event(
        daemon_paths,
        domain::Event{
            .type = "message.received",
            .summary = "received TeamSpeak text message",
            .at = std::chrono::system_clock::now(),
            .fields =
                {
                    {"from_name", "alice"},
                    {"message_kind", "client"},
                    {"text", "hello from inbox"},
                },
        }
    );
    tests::expect(appended_inbox.ok(), "appending daemon inbox event should succeed");

    auto inbox_command = parse_command(router, {"message", "inbox", "--count", "1"});
    tests::expect(inbox_command.ok(), "message inbox parse should succeed");
    auto inbox_result = router.dispatch(inbox_command.value());
    tests::expect(inbox_result.ok(), "message inbox dispatch should succeed");
    const auto inbox_table = output::render(inbox_result.value(), output::Format::table);
    tests::expect_contains(inbox_table, "alice", "message inbox should render the sender");
    tests::expect_contains(inbox_table, "hello from inbox", "message inbox should render the message text");
    tests::expect(
        inbox_table.find("Event") == std::string::npos,
        "default message inbox table should not include wide columns"
    );
    auto wide_inbox_command = parse_command(router, {"message", "inbox", "--count", "1", "--wide"});
    tests::expect(wide_inbox_command.ok(), "wide message inbox parse should succeed");
    auto wide_inbox_result = router.dispatch(wide_inbox_command.value());
    tests::expect(wide_inbox_result.ok(), "wide message inbox dispatch should succeed");
    const auto wide_inbox_table = output::render(wide_inbox_result.value(), output::Format::table);
    tests::expect_contains(wide_inbox_table, "Event", "wide message inbox should include event type column");
    tests::expect_contains(wide_inbox_table, "Summary", "wide message inbox should include summary column");
    tests::expect_contains(wide_inbox_table, "message.received", "wide message inbox should show event types");

    auto daemon_status = parse_command(router, {"daemon", "status"});
    tests::expect(daemon_status.ok(), "daemon status parse should succeed");
    auto daemon_status_result = router.dispatch(daemon_status.value());
    tests::expect(daemon_status_result.ok(), "daemon status dispatch should succeed");
    const auto daemon_status_table = output::render(daemon_status_result.value(), output::Format::table);
    tests::expect_contains(
        daemon_status_table,
        "not running",
        "daemon status should explain when the daemon is not running"
    );

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
