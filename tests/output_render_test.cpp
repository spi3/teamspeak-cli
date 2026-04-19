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

    return 0;
}
