#include <iostream>

#include "teamspeak_cli/cli/command_router.hpp"

int main(int argc, char** argv) {
    teamspeak_cli::cli::CommandRouter router;
    auto parsed = router.parse(argc, argv);
    if (!parsed) {
        std::cerr << teamspeak_cli::output::render_error(
                         parsed.error(), teamspeak_cli::output::Format::table, false
                     )
                  << '\n';
        return static_cast<int>(parsed.error().exit_code);
    }

    if (parsed.value().show_help) {
        std::cout << router.render_help(parsed.value().path) << '\n';
        return 0;
    }

    const auto progress = parsed.value().global.format == teamspeak_cli::output::Format::table
        ? teamspeak_cli::cli::CommandRouter::ProgressSink([](std::string_view message) {
              std::cerr << message << '\n';
              std::cerr.flush();
          })
        : teamspeak_cli::cli::CommandRouter::ProgressSink{};

    auto result = router.dispatch(parsed.value(), progress);
    if (!result) {
        std::cerr << teamspeak_cli::output::render_error(
                         result.error(), parsed.value().global.format, parsed.value().global.debug
                     )
                  << '\n';
        return static_cast<int>(result.error().exit_code);
    }

    std::cout << teamspeak_cli::output::render(result.value(), parsed.value().global.format) << '\n';
    return static_cast<int>(result.value().exit_code);
}
