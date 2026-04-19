#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "teamspeak_cli/domain/models.hpp"
#include "teamspeak_cli/domain/result.hpp"

namespace teamspeak_cli::output {

enum class Format {
    table,
    json,
    yaml,
};

using Value = std::variant<
    std::nullptr_t,
    bool,
    std::int64_t,
    std::string,
    std::vector<struct ValueHolder>,
    std::map<std::string, struct ValueHolder>>;

struct ValueHolder {
    Value value;
};

struct Table {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
};

struct Details {
    std::vector<std::pair<std::string, std::string>> fields;
};

using HumanView = std::variant<std::monostate, Table, Details, std::string>;

struct CommandOutput {
    ValueHolder data;
    HumanView human;
};

auto parse_format(const std::string& name) -> domain::Result<Format>;
auto render(const CommandOutput& output, Format format) -> std::string;
auto render_error(const domain::Error& error, Format format, bool debug) -> std::string;

auto make_string(std::string value) -> ValueHolder;
auto make_bool(bool value) -> ValueHolder;
auto make_int(std::int64_t value) -> ValueHolder;
auto make_array(std::vector<ValueHolder> value) -> ValueHolder;
auto make_object(std::map<std::string, ValueHolder> value) -> ValueHolder;

auto to_value(const domain::PluginInfo& info) -> ValueHolder;
auto to_value(const domain::ConnectionState& state) -> ValueHolder;
auto to_value(const domain::ServerInfo& info) -> ValueHolder;
auto to_value(const domain::Channel& channel) -> ValueHolder;
auto to_value(const domain::Client& client) -> ValueHolder;
auto to_value(const domain::Event& event) -> ValueHolder;
auto to_value(const domain::Profile& profile) -> ValueHolder;
auto to_value(const std::vector<domain::Channel>& channels) -> ValueHolder;
auto to_value(const std::vector<domain::Client>& clients) -> ValueHolder;
auto to_value(const std::vector<domain::Event>& events) -> ValueHolder;
auto to_value(const std::vector<domain::Profile>& profiles) -> ValueHolder;

auto status_view(const domain::ConnectionState& state) -> Details;
auto server_view(const domain::ServerInfo& info) -> Details;
auto plugin_info_view(const domain::PluginInfo& info) -> Details;
auto profile_table(const std::vector<domain::Profile>& profiles, const std::string& active_profile) -> Table;
auto channel_table(const std::vector<domain::Channel>& channels) -> Table;
auto client_table(const std::vector<domain::Client>& clients) -> Table;
auto channel_details(const domain::Channel& channel) -> Details;
auto client_details(const domain::Client& client) -> Details;
auto event_table(const std::vector<domain::Event>& events) -> Table;

}  // namespace teamspeak_cli::output
