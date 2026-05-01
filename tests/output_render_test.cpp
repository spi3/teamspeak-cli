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
    teamspeak_cli::tests::expect_eq(
        json,
        std::string("{\"client_count\":3,\"id\":\"7\",\"is_default\":true,\"name\":\"Lobby\",\"parent_id\":null,\"subscribed\":true}"),
        "channel json should keep the documented object structure"
    );

    const std::vector<domain::Channel> channel_list{
        channel,
        domain::Channel{
            .id = {8},
            .name = "Breakout",
            .parent_id = domain::ChannelId{7},
            .client_count = 0,
            .is_default = false,
            .subscribed = false,
        },
    };
    teamspeak_cli::tests::expect_eq(
        output::render(
            output::CommandOutput{.data = output::to_value(channel_list), .human = output::channel_table(channel_list)},
            output::Format::json
        ),
        std::string("[{\"client_count\":3,\"id\":\"7\",\"is_default\":true,\"name\":\"Lobby\",\"parent_id\":null,\"subscribed\":true},{\"client_count\":0,\"id\":\"8\",\"is_default\":false,\"name\":\"Breakout\",\"parent_id\":\"7\",\"subscribed\":false}]"),
        "channel list json should keep the documented array structure and nullable parent_id"
    );
    const output::CommandOutput channel_list_output{
        .data = output::to_value(channel_list),
        .human = output::channel_table(channel_list),
    };
    const std::string default_channel_table = output::render(channel_list_output, output::Format::table);
    teamspeak_cli::tests::expect_contains(
        default_channel_table,
        "ID  Name      Parent  Clients  Default",
        "default channel table should keep its existing headers"
    );
    teamspeak_cli::tests::expect(
        default_channel_table.find("Subscribed") == std::string::npos,
        "default channel table should not include wide columns"
    );
    const std::string no_header_channel_table = output::render(
        channel_list_output,
        output::Format::table,
        output::TableRenderOptions{.show_headers = false}
    );
    teamspeak_cli::tests::expect(
        no_header_channel_table.find("ID  Name") == std::string::npos,
        "no-header channel table should omit only the header row"
    );
    teamspeak_cli::tests::expect(
        no_header_channel_table.rfind("7   Lobby", 0) == 0,
        "no-header channel table should keep data rows"
    );

    const std::vector<domain::Client> client_list{
        domain::Client{
            .id = {1},
            .nickname = "terminal",
            .unique_identity = "identity",
            .channel_id = domain::ChannelId{7},
            .self = true,
            .talking = false,
        },
        domain::Client{
            .id = {2},
            .nickname = "detached",
            .unique_identity = "",
            .channel_id = std::nullopt,
            .self = false,
            .talking = true,
        },
    };
    teamspeak_cli::tests::expect_eq(
        output::render(
            output::CommandOutput{.data = output::to_value(client_list), .human = output::client_table(client_list)},
            output::Format::json
        ),
        std::string("[{\"channel_id\":\"7\",\"id\":\"1\",\"nickname\":\"terminal\",\"self\":true,\"talking\":false,\"unique_identity\":\"identity\"},{\"channel_id\":null,\"id\":\"2\",\"nickname\":\"detached\",\"self\":false,\"talking\":true,\"unique_identity\":\"\"}]"),
        "client list json should keep the documented array structure and nullable channel_id"
    );

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
    teamspeak_cli::tests::expect_eq(
        plugin_json,
        std::string("{\"backend\":\"plugin\",\"media_diagnostics\":{\"active_speaker_count\":0,\"capture\":{\"device\":\"\",\"is_default\":false,\"known\":false,\"mode\":\"\"},\"captured_voice_edit_attached\":false,\"consumer_connected\":false,\"custom_capture_device_id\":\"\",\"custom_capture_device_name\":\"\",\"custom_capture_device_registered\":false,\"custom_capture_path_available\":false,\"dropped_audio_chunks\":0,\"dropped_playback_chunks\":0,\"injected_playback_attached_to_capture\":false,\"last_error\":\"\",\"playback\":{\"device\":\"\",\"is_default\":false,\"known\":false,\"mode\":\"\"},\"playback_active\":false,\"pulse_sink\":\"\",\"pulse_source\":\"\",\"pulse_source_is_monitor\":false,\"queued_playback_samples\":128,\"transmit_path\":\"\",\"transmit_path_ready\":true},\"media_format\":\"pcm_s16le\",\"media_socket_path\":\"/tmp/ts3cli-media.sock\",\"media_transport\":\"unix\",\"note\":\"ready\",\"plugin_available\":true,\"plugin_name\":\"ts3cli\",\"plugin_version\":\"1.2.3\",\"socket_path\":\"/tmp/ts3cli.sock\",\"transport\":\"unix\"}"),
        "plugin info json should keep the documented object structure"
    );
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
    auto transmit_ready = output::extract_field(
        plugin_rendered.data, "media_diagnostics.transmit_path_ready"
    );
    teamspeak_cli::tests::expect(transmit_ready.ok(), "field extraction should find nested bool fields");
    auto rendered_transmit_ready = output::render_extracted_field(transmit_ready.value());
    teamspeak_cli::tests::expect(rendered_transmit_ready.ok(), "field rendering should accept bools");
    teamspeak_cli::tests::expect_eq(
        rendered_transmit_ready.value(),
        std::string("true"),
        "extracted bool fields should render as shell-friendly literals"
    );

    auto socket_path = output::extract_field(plugin_rendered.data, "socket_path");
    teamspeak_cli::tests::expect(socket_path.ok(), "field extraction should find top-level string fields");
    auto rendered_socket_path = output::render_extracted_field(socket_path.value());
    teamspeak_cli::tests::expect(rendered_socket_path.ok(), "field rendering should accept strings");
    teamspeak_cli::tests::expect_eq(
        rendered_socket_path.value(),
        std::string("/tmp/ts3cli.sock"),
        "extracted string fields should render as raw text"
    );

    auto parent_id = output::extract_field(rendered.data, "parent_id");
    teamspeak_cli::tests::expect(parent_id.ok(), "field extraction should find null fields");
    auto rendered_parent_id = output::render_extracted_field(parent_id.value());
    teamspeak_cli::tests::expect(rendered_parent_id.ok(), "field rendering should accept null");
    teamspeak_cli::tests::expect_eq(
        rendered_parent_id.value(),
        std::string("null"),
        "extracted null fields should render as the literal null"
    );

    auto missing_field = output::extract_field(plugin_rendered.data, "media_diagnostics.missing");
    teamspeak_cli::tests::expect(!missing_field.ok(), "missing field extraction should fail");
    teamspeak_cli::tests::expect_contains(
        missing_field.error().message,
        "field `media_diagnostics.missing` was not found",
        "missing field errors should name the requested path"
    );

    auto complex_field = output::extract_field(plugin_rendered.data, "media_diagnostics");
    teamspeak_cli::tests::expect(complex_field.ok(), "field extraction should allow selecting objects");
    auto rendered_complex_field = output::render_extracted_field(complex_field.value());
    teamspeak_cli::tests::expect(!rendered_complex_field.ok(), "complex selected fields should fail to render");
    teamspeak_cli::tests::expect_contains(
        rendered_complex_field.error().message,
        "--field can only render scalar values",
        "complex field errors should explain the scalar-only contract"
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
    teamspeak_cli::tests::expect_eq(
        output::render(
            output::CommandOutput{.data = output::to_value(state), .human = output::connection_status_view(state)},
            output::Format::json
        ),
        std::string("{\"backend\":\"plugin\",\"connection\":\"42\",\"identity\":\"identity\",\"mode\":\"plugin-control\",\"nickname\":\"terminal\",\"phase\":\"connected\",\"port\":9987,\"profile\":\"plugin-local\",\"server\":\"voice.example.com\"}"),
        "status json should keep the documented connection state structure"
    );

    const auto fixed_event_time = std::chrono::system_clock::from_time_t(1700000000);
    const std::vector<domain::Event> lifecycle = {
        domain::Event{
            .type = "connection.requested",
            .summary = "requested new TeamSpeak client connection",
            .at = fixed_event_time,
            .fields = {{"server", "voice.example.com"}, {"port", "9987"}},
        },
        domain::Event{
            .type = "connection.connecting",
            .summary = "connection is starting",
            .at = fixed_event_time,
            .fields = {},
        },
        domain::Event{
            .type = "connection.connected",
            .summary = "connected to TeamSpeak server",
            .at = fixed_event_time,
            .fields = {{"server", "voice.example.com"}, {"port", "9987"}},
        },
    };
    teamspeak_cli::tests::expect_eq(
        output::render(
            output::CommandOutput{.data = output::to_value(lifecycle), .human = output::event_table(lifecycle)},
            output::Format::json
        ),
        std::string("[{\"fields\":{\"port\":\"9987\",\"server\":\"voice.example.com\"},\"summary\":\"requested new TeamSpeak client connection\",\"timestamp\":\"2023-11-14T22:13:20Z\",\"type\":\"connection.requested\"},{\"fields\":{},\"summary\":\"connection is starting\",\"timestamp\":\"2023-11-14T22:13:20Z\",\"type\":\"connection.connecting\"},{\"fields\":{\"port\":\"9987\",\"server\":\"voice.example.com\"},\"summary\":\"connected to TeamSpeak server\",\"timestamp\":\"2023-11-14T22:13:20Z\",\"type\":\"connection.connected\"}]"),
        "events watch json should keep the documented event array structure"
    );
    teamspeak_cli::tests::expect_eq(
        output::render(
            output::CommandOutput{.data = output::to_value(lifecycle), .human = output::event_table(lifecycle)},
            output::Format::ndjson
        ),
        std::string(
            "{\"fields\":{\"port\":\"9987\",\"server\":\"voice.example.com\"},\"summary\":\"requested new "
            "TeamSpeak client connection\",\"timestamp\":\"2023-11-14T22:13:20Z\",\"type\":\"connection."
            "requested\"}\n"
            "{\"fields\":{},\"summary\":\"connection is starting\",\"timestamp\":\"2023-11-14T22:13:20Z\","
            "\"type\":\"connection.connecting\"}\n"
            "{\"fields\":{\"port\":\"9987\",\"server\":\"voice.example.com\"},\"summary\":\"connected to "
            "TeamSpeak server\",\"timestamp\":\"2023-11-14T22:13:20Z\",\"type\":\"connection.connected\"}"
        ),
        "events watch ndjson should render one event object per line"
    );
    const output::CommandOutput connect_output{
        .data = output::make_object({
            {"result", output::make_string("connected")},
            {"connected", output::make_bool(true)},
            {"timed_out", output::make_bool(false)},
            {"timeout_ms", output::make_int(15000)},
            {"state", output::to_value(state)},
            {"lifecycle", output::to_value(lifecycle)},
        }),
        .human = output::connect_view(state, lifecycle, true, false, std::chrono::seconds(15), true),
    };
    teamspeak_cli::tests::expect_eq(
        output::render(connect_output, output::Format::json),
        std::string("{\"connected\":true,\"lifecycle\":[{\"fields\":{\"port\":\"9987\",\"server\":\"voice.example.com\"},\"summary\":\"requested new TeamSpeak client connection\",\"timestamp\":\"2023-11-14T22:13:20Z\",\"type\":\"connection.requested\"},{\"fields\":{},\"summary\":\"connection is starting\",\"timestamp\":\"2023-11-14T22:13:20Z\",\"type\":\"connection.connecting\"},{\"fields\":{\"port\":\"9987\",\"server\":\"voice.example.com\"},\"summary\":\"connected to TeamSpeak server\",\"timestamp\":\"2023-11-14T22:13:20Z\",\"type\":\"connection.connected\"}],\"result\":\"connected\",\"state\":{\"backend\":\"plugin\",\"connection\":\"42\",\"identity\":\"identity\",\"mode\":\"plugin-control\",\"nickname\":\"terminal\",\"phase\":\"connected\",\"port\":9987,\"profile\":\"plugin-local\",\"server\":\"voice.example.com\"},\"timed_out\":false,\"timeout_ms\":15000}"),
        "connect json should keep the documented object structure"
    );
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
    const std::vector<domain::Event> disconnect_lifecycle = {
        domain::Event{
            .type = "connection.disconnected",
            .summary = "ts disconnect",
            .at = fixed_event_time,
            .fields = {},
        },
    };
    const output::CommandOutput disconnect_output{
        .data = output::make_object({
            {"result", output::make_string("disconnected")},
            {"disconnected", output::make_bool(true)},
            {"timed_out", output::make_bool(false)},
            {"timeout_ms", output::make_int(10000)},
            {"state", output::to_value(stalled_state)},
            {"lifecycle", output::to_value(disconnect_lifecycle)},
        }),
        .human = output::disconnect_view(
            stalled_state, disconnect_lifecycle, true, false, std::chrono::seconds(10), true
        ),
    };
    teamspeak_cli::tests::expect_eq(
        output::render(disconnect_output, output::Format::json),
        std::string("{\"disconnected\":true,\"lifecycle\":[{\"fields\":{},\"summary\":\"ts disconnect\",\"timestamp\":\"2023-11-14T22:13:20Z\",\"type\":\"connection.disconnected\"}],\"result\":\"disconnected\",\"state\":{\"backend\":\"plugin\",\"connection\":\"0\",\"identity\":\"identity\",\"mode\":\"plugin-control\",\"nickname\":\"terminal\",\"phase\":\"disconnected\",\"port\":9987,\"profile\":\"plugin-local\",\"server\":\"voice.example.com\"},\"timed_out\":false,\"timeout_ms\":10000}"),
        "disconnect json should keep the documented object structure"
    );
    const std::vector<domain::Event> stalled_lifecycle{lifecycle[0], lifecycle[1]};
    const std::string stalled_connect_human = output::connect_view(
        stalled_state, stalled_lifecycle, false, true, std::chrono::seconds(15), true
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
    teamspeak_cli::tests::expect_eq(
        error_json,
        std::string("{\"category\":\"bridge\",\"code\":\"socket_connect_failed\",\"hints\":[\"Run `ts client start` to launch the local TeamSpeak client.\",\"Run `ts plugin info` to verify the ts3cli plugin bridge is available.\"],\"message\":\"Unable to read TeamSpeak status because the TeamSpeak client is not running or the ts3cli plugin is unavailable.\"}"),
        "json error output should keep the documented structured error shape"
    );
    teamspeak_cli::tests::expect(
        error_json.find("\"hints\":[") != std::string::npos,
        "json error output should include structured hints"
    );
    teamspeak_cli::tests::expect(
        error_json.find("\"details\"") == std::string::npos,
        "json error output should hide debug details by default"
    );

    const std::string error_debug_json = output::render_error(error, output::Format::json, true);
    teamspeak_cli::tests::expect_eq(
        error_debug_json,
        std::string("{\"category\":\"bridge\",\"code\":\"socket_connect_failed\",\"details\":{\"socket_path\":\"/tmp/ts3cli.sock\"},\"hints\":[\"Run `ts client start` to launch the local TeamSpeak client.\",\"Run `ts plugin info` to verify the ts3cli plugin bridge is available.\"],\"message\":\"Unable to read TeamSpeak status because the TeamSpeak client is not running or the ts3cli plugin is unavailable.\"}"),
        "debug json error output should add only debug details to the structured error shape"
    );
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
