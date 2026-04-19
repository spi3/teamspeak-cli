#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "teamspeak_cli/config/config_store.hpp"
#include "teamspeak_cli/output/render.hpp"
#include "teamspeak_cli/sdk/backend_factory.hpp"

namespace teamspeak_cli::cli {

struct GlobalOptions {
    output::Format format = output::Format::table;
    bool verbose = false;
    bool debug = false;
    std::string profile;
    std::optional<std::string> server;
    std::optional<std::string> nickname;
    std::optional<std::string> identity;
    std::filesystem::path config_path;
};

struct ParsedCommand {
    std::vector<std::string> path;
    std::vector<std::string> positionals;
    std::map<std::string, std::string> options;
    std::set<std::string> flags;
    GlobalOptions global;
    bool show_help = false;
};

class CommandRouter {
  public:
    CommandRouter();

    auto parse(int argc, char** argv) const -> domain::Result<ParsedCommand>;
    auto render_help(const std::vector<std::string>& path) const -> std::string;
    auto dispatch(const ParsedCommand& command) -> domain::Result<output::CommandOutput>;

  private:
    config::ConfigStore config_store_;
    sdk::BackendFactory backend_factory_;
};

}  // namespace teamspeak_cli::cli
