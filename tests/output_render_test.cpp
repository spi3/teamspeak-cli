#include <string>

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
