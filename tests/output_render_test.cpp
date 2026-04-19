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

    const std::string yaml = output::render(rendered, output::Format::yaml);
    teamspeak_cli::tests::expect(yaml.find("name: Lobby") != std::string::npos, "yaml output should include name");

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
