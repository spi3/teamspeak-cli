#include <chrono>
#include <string>
#include <vector>

#include "teamspeak_cli/output/render.hpp"
#include "test_support.hpp"

int main() {
    using namespace teamspeak_cli;

    const domain::Channel channel{
        .id = {7},
        .name = "Lobby",
        .parent_id = std::nullopt,
        .client_count = 3,
        .is_default = true,
    };

    const output::CommandOutput rendered{
        .data = output::to_value(channel),
        .human = output::channel_details(channel),
    };

    const std::string table = output::render(rendered, output::Format::table);
    teamspeak_cli::tests::expect(table.find("Lobby") != std::string::npos, "table output should include channel name");

    const std::string json = output::render(rendered, output::Format::json);
    teamspeak_cli::tests::expect(json.find("\"name\":\"Lobby\"") != std::string::npos, "json output should include name");

    const std::string control_value =
        std::string{"A"} + '\0' + '\x01' + '\b' + '\t' + '\n' + '\v' + '\f' + '\r' + '\x1f' + '"' + '\\' + 'Z';
    const output::CommandOutput control_rendered{
        .data = output::make_object({{"control", output::make_string(control_value)}}),
        .human = std::monostate{},
    };
    const std::string control_json = output::render(control_rendered, output::Format::json);
    const std::string expected_control_json =
        "{\"control\":\"A\\u0000\\u0001\\b\\t\\n\\u000b\\f\\r\\u001f\\\"\\\\Z\"}";
    teamspeak_cli::tests::expect_eq(
        control_json,
        expected_control_json,
        "json output should escape all control characters below 0x20"
    );

    const std::string yaml = output::render(rendered, output::Format::yaml);
    teamspeak_cli::tests::expect(yaml.find("name: Lobby") != std::string::npos, "yaml output should include name");

    const domain::ServerInfo server_info{
        .name = "Example",
        .host = "voice.example.com",
        .port = 9987,
        .backend = "mock",
        .current_channel = domain::ChannelId{7},
        .channel_count = 4,
        .client_count = 12,
    };
    const output::CommandOutput server_rendered{
        .data = output::to_value(server_info),
        .human = output::server_view(server_info),
    };
    const std::string server_table = output::render(server_rendered, output::Format::table);
    teamspeak_cli::tests::expect_contains(
        server_table,
        "Current channel",
        "server details should render readable current channel label"
    );
    teamspeak_cli::tests::expect(
        server_table.find("CurrentChannel") == std::string::npos,
        "server details should not expose PascalCase current channel label"
    );
    const std::string server_json = output::render(server_rendered, output::Format::json);
    teamspeak_cli::tests::expect_contains(
        server_json,
        "\"current_channel\":\"7\"",
        "server json should keep current_channel key unchanged"
    );

    domain::PluginInfo plugin_info{
        .backend = "plugin",
        .transport = "unix",
        .plugin_name = "ts3cli",
        .plugin_version = "1.2.3",
        .plugin_available = true,
        .socket_path = "/tmp/ts3cli.sock",
        .media_transport = "unix",
        .media_socket_path = "/tmp/ts3cli-media.sock",
        .media_format = "pcm_s16le",
        .media_diagnostics = {},
        .note = "ready",
    };
    plugin_info.media_diagnostics.queued_playback_samples = 128;
    plugin_info.media_diagnostics.transmit_path_ready = true;
    const output::CommandOutput plugin_rendered{
        .data = output::to_value(plugin_info),
        .human = output::plugin_info_view(plugin_info),
    };
    const std::string plugin_table = output::render(plugin_rendered, output::Format::table);
    teamspeak_cli::tests::expect_contains(
        plugin_table,
        "Socket path",
        "plugin details should render readable socket path label"
    );
    teamspeak_cli::tests::expect_contains(
        plugin_table,
        "Media transport",
        "plugin details should render readable media transport label"
    );
    teamspeak_cli::tests::expect_contains(
        plugin_table,
        "Queued playback samples",
        "plugin details should render readable queued playback samples label"
    );
    teamspeak_cli::tests::expect_contains(
        plugin_table,
        "Transmit path ready",
        "plugin details should render readable transmit path ready label"
    );
    teamspeak_cli::tests::expect(
        plugin_table.find("SocketPath") == std::string::npos &&
            plugin_table.find("QueuedPlaybackSamples") == std::string::npos &&
            plugin_table.find("TransmitPathReady") == std::string::npos,
        "plugin details should not expose PascalCase labels"
    );
    const std::string plugin_json = output::render(plugin_rendered, output::Format::json);
    teamspeak_cli::tests::expect_contains(
        plugin_json,
        "\"socket_path\":\"/tmp/ts3cli.sock\"",
        "plugin json should keep socket_path key unchanged"
    );
    teamspeak_cli::tests::expect_contains(
        plugin_json,
        "\"media_transport\":\"unix\"",
        "plugin json should keep media_transport key unchanged"
    );
    teamspeak_cli::tests::expect_contains(
        plugin_json,
        "\"queued_playback_samples\":128",
        "plugin json should keep queued_playback_samples key unchanged"
    );
    teamspeak_cli::tests::expect_contains(
        plugin_json,
        "\"transmit_path_ready\":true",
        "plugin json should keep transmit_path_ready key unchanged"
    );

    const std::string process_details = output::render_details_block(output::Details{{
        {"PIDFile", "/tmp/ts3client.pid"},
        {"LogFile", "/tmp/ts3client.log"},
    }});
    teamspeak_cli::tests::expect_contains(
        process_details,
        "PID file",
        "ad hoc details should preserve acronym labels while lowercasing words"
    );
    teamspeak_cli::tests::expect_contains(
        process_details,
        "Log file",
        "ad hoc details should render readable log file label"
    );

    const domain::ConnectionState state{
        .phase = domain::ConnectionPhase::connected,
        .backend = "plugin",
        .connection = {42},
        .server = "voice.example.com",
        .port = 9987,
        .nickname = "terminal",
        .identity = "identity",
        .profile = "plugin-local",
        .mode = "plugin-control",
    };
    const std::vector<domain::Event> lifecycle = {
        domain::Event{
            .type = "connection.requested",
            .summary = "requested new TeamSpeak client connection",
            .at = std::chrono::system_clock::now(),
            .fields = {{"server", "voice.example.com"}, {"port", "9987"}},
        },
        domain::Event{
            .type = "connection.connecting",
            .summary = "connection is starting",
            .at = std::chrono::system_clock::now(),
            .fields = {},
        },
    };
    const std::string connect_human = output::connect_view(
        state, lifecycle, true, false, std::chrono::seconds(15), true
    );
    teamspeak_cli::tests::expect_contains(
        connect_human,
        "Connected to voice.example.com:9987 as terminal.",
        "connect view should open with a human summary"
    );
    teamspeak_cli::tests::expect_contains(
        connect_human,
        "TeamSpeak accepted the request to connect to voice.example.com:9987.",
        "connect view should narrate the lifecycle in prose"
    );
    teamspeak_cli::tests::expect(
        connect_human.find("connection.requested") == std::string::npos,
        "connect view should not expose raw lifecycle event codes"
    );
    teamspeak_cli::tests::expect(
        connect_human.find("202") == std::string::npos,
        "connect view should not render machine-style timestamps"
    );
    const domain::ConnectionState stalled_state{
        .phase = domain::ConnectionPhase::disconnected,
        .backend = "plugin",
        .connection = {0},
        .server = "voice.example.com",
        .port = 9987,
        .nickname = "terminal",
        .identity = "identity",
        .profile = "plugin-local",
        .mode = "plugin-control",
    };
    const std::string stalled_connect_human = output::connect_view(
        stalled_state, lifecycle, false, true, std::chrono::seconds(15), true
    );
    teamspeak_cli::tests::expect_contains(
        stalled_connect_human,
        "first headless TeamSpeak launch",
        "timed-out plugin connect view should mention hidden first-run dialogs"
    );
    teamspeak_cli::tests::expect_contains(
        stalled_connect_human,
        "ts client logs",
        "timed-out plugin connect view should point at client logs for diagnosis"
    );

    auto error = domain::make_error(
        "bridge",
        "socket_connect_failed",
        "Unable to read TeamSpeak status because the TeamSpeak client is not running or the ts3cli plugin is unavailable.",
        domain::ExitCode::connection
    );
    error.details.emplace("hint_001", "Run `ts client start` to launch the local TeamSpeak client.");
    error.details.emplace("hint_002", "Run `ts plugin info` to verify the ts3cli plugin bridge is available.");
    error.details.emplace("socket_path", "/tmp/ts3cli.sock");

    const std::string error_table = output::render_error(error, output::Format::table, false);
    teamspeak_cli::tests::expect(
        error_table.find("Next steps:") != std::string::npos,
        "table error output should render next steps when hints are available"
    );
    teamspeak_cli::tests::expect(
        error_table.find("ts client start") != std::string::npos,
        "table error output should include the first hint"
    );

    const std::string error_json = output::render_error(error, output::Format::json, false);
    teamspeak_cli::tests::expect(
        error_json.find("\"hints\":[") != std::string::npos,
        "json error output should include structured hints"
    );
    teamspeak_cli::tests::expect(
        error_json.find("\"details\"") == std::string::npos,
        "json error output should hide debug details by default"
    );

    const std::string error_debug_json = output::render_error(error, output::Format::json, true);
    teamspeak_cli::tests::expect(
        error_debug_json.find("\"details\":") != std::string::npos,
        "debug json error output should include debug details"
    );
    teamspeak_cli::tests::expect(
        error_debug_json.find("\"socket_path\":\"/tmp/ts3cli.sock\"") != std::string::npos,
        "debug json error output should include non-hint details"
    );
    teamspeak_cli::tests::expect(
        error_debug_json.find("\"hint_001\"") == std::string::npos,
        "debug json error output should not duplicate hints inside the details payload"
    );

    return 0;
}
