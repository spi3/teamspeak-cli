#include "teamspeak_cli/output/render.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>

#include "teamspeak_cli/util/strings.hpp"

namespace teamspeak_cli::output {
namespace {

auto escape_json(const std::string& value) -> std::string {
    std::ostringstream out;
    for (const char ch : value) {
        switch (ch) {
            case '\\':
                out << "\\\\";
                break;
            case '"':
                out << "\\\"";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                out << ch;
                break;
        }
    }
    return out.str();
}

auto yaml_scalar(const std::string& value) -> std::string {
    if (value.empty()) {
        return "\"\"";
    }
    const bool needs_quotes =
        value.find_first_of(":#{}[]\n\t") != std::string::npos || value == "true" ||
        value == "false" || value == "null" || value == "-" || value.find(' ') != std::string::npos;
    if (!needs_quotes) {
        return value;
    }
    std::ostringstream out;
    out << '"';
    for (const char ch : value) {
        if (ch == '"' || ch == '\\') {
            out << '\\';
        }
        out << ch;
    }
    out << '"';
    return out.str();
}

void render_json_impl(const ValueHolder& holder, std::ostringstream& out) {
    std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) {
                out << "null";
            } else if constexpr (std::is_same_v<T, bool>) {
                out << (value ? "true" : "false");
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                out << value;
            } else if constexpr (std::is_same_v<T, std::string>) {
                out << '"' << escape_json(value) << '"';
            } else if constexpr (std::is_same_v<T, std::vector<ValueHolder>>) {
                out << '[';
                for (std::size_t index = 0; index < value.size(); ++index) {
                    if (index != 0) {
                        out << ',';
                    }
                    render_json_impl(value[index], out);
                }
                out << ']';
            } else if constexpr (std::is_same_v<T, std::map<std::string, ValueHolder>>) {
                out << '{';
                std::size_t index = 0;
                for (const auto& [key, child] : value) {
                    if (index != 0) {
                        out << ',';
                    }
                    out << '"' << escape_json(key) << "\":";
                    render_json_impl(child, out);
                    ++index;
                }
                out << '}';
            }
        },
        holder.value
    );
}

void render_yaml_impl(const ValueHolder& holder, std::ostringstream& out, int indent) {
    const auto pad = std::string(static_cast<std::size_t>(indent), ' ');
    std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) {
                out << "null";
            } else if constexpr (std::is_same_v<T, bool>) {
                out << (value ? "true" : "false");
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                out << value;
            } else if constexpr (std::is_same_v<T, std::string>) {
                out << yaml_scalar(value);
            } else if constexpr (std::is_same_v<T, std::vector<ValueHolder>>) {
                if (value.empty()) {
                    out << "[]";
                    return;
                }
                for (std::size_t index = 0; index < value.size(); ++index) {
                    if (index != 0) {
                        out << '\n';
                    }
                    out << pad << "- ";
                    const bool nested =
                        std::holds_alternative<std::vector<ValueHolder>>(value[index].value) ||
                        std::holds_alternative<std::map<std::string, ValueHolder>>(value[index].value);
                    if (nested) {
                        if (std::holds_alternative<std::map<std::string, ValueHolder>>(value[index].value)) {
                            out << '\n';
                        }
                        render_yaml_impl(value[index], out, indent + 2);
                    } else {
                        render_yaml_impl(value[index], out, 0);
                    }
                }
            } else if constexpr (std::is_same_v<T, std::map<std::string, ValueHolder>>) {
                std::size_t index = 0;
                for (const auto& [key, child] : value) {
                    if (index != 0) {
                        out << '\n';
                    }
                    out << pad << key << ":";
                    const bool nested =
                        std::holds_alternative<std::vector<ValueHolder>>(child.value) ||
                        std::holds_alternative<std::map<std::string, ValueHolder>>(child.value);
                    if (nested) {
                        out << '\n';
                        render_yaml_impl(child, out, indent + 2);
                    } else {
                        out << ' ';
                        render_yaml_impl(child, out, 0);
                    }
                    ++index;
                }
            }
        },
        holder.value
    );
}

auto render_table_impl(const Table& table) -> std::string {
    if (table.rows.empty()) {
        return "(none)";
    }

    std::vector<std::size_t> widths(table.columns.size(), 0);
    for (std::size_t index = 0; index < table.columns.size(); ++index) {
        widths[index] = table.columns[index].size();
    }

    for (const auto& row : table.rows) {
        for (std::size_t index = 0; index < row.size(); ++index) {
            widths[index] = std::max(widths[index], row[index].size());
        }
    }

    std::ostringstream out;
    for (std::size_t index = 0; index < table.columns.size(); ++index) {
        if (index != 0) {
            out << "  ";
        }
        out << std::left << std::setw(static_cast<int>(widths[index])) << table.columns[index];
    }
    out << '\n';

    for (std::size_t row_index = 0; row_index < table.rows.size(); ++row_index) {
        const auto& row = table.rows[row_index];
        for (std::size_t index = 0; index < row.size(); ++index) {
            if (index != 0) {
                out << "  ";
            }
            out << std::left << std::setw(static_cast<int>(widths[index])) << row[index];
        }
        if (row_index + 1 != table.rows.size()) {
            out << '\n';
        }
    }
    return out.str();
}

auto render_details_impl(const Details& details) -> std::string {
    if (details.fields.empty()) {
        return "(empty)";
    }
    std::size_t width = 0;
    for (const auto& [key, _] : details.fields) {
        width = std::max(width, key.size());
    }

    std::ostringstream out;
    for (std::size_t index = 0; index < details.fields.size(); ++index) {
        const auto& [key, value] = details.fields[index];
        out << std::left << std::setw(static_cast<int>(width)) << key << "  " << value;
        if (index + 1 != details.fields.size()) {
            out << '\n';
        }
    }
    return out.str();
}

auto format_time(std::chrono::system_clock::time_point point) -> std::string {
    const auto time = std::chrono::system_clock::to_time_t(point);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

auto maybe_channel(const std::optional<domain::ChannelId>& channel_id) -> ValueHolder {
    if (!channel_id.has_value()) {
        return ValueHolder{nullptr};
    }
    return make_string(domain::to_string(*channel_id));
}

}  // namespace

auto parse_format(const std::string& name) -> domain::Result<Format> {
    if (name == "table") {
        return domain::ok(Format::table);
    }
    if (name == "json") {
        return domain::ok(Format::json);
    }
    if (name == "yaml") {
        return domain::ok(Format::yaml);
    }
    return domain::fail<Format>(domain::make_error(
        "cli", "invalid_format", "invalid output format: " + name, domain::ExitCode::usage
    ));
}

auto render(const CommandOutput& output, Format format) -> std::string {
    if (format == Format::json) {
        std::ostringstream out;
        render_json_impl(output.data, out);
        return out.str();
    }
    if (format == Format::yaml) {
        std::ostringstream out;
        render_yaml_impl(output.data, out, 0);
        return out.str();
    }

    if (const auto* table = std::get_if<Table>(&output.human)) {
        return render_table_impl(*table);
    }
    if (const auto* details = std::get_if<Details>(&output.human)) {
        return render_details_impl(*details);
    }
    if (const auto* text = std::get_if<std::string>(&output.human)) {
        return *text;
    }

    std::ostringstream out;
    render_json_impl(output.data, out);
    return out.str();
}

auto render_error(const domain::Error& error, Format format, bool debug) -> std::string {
    std::map<std::string, ValueHolder> payload{
        {"category", make_string(error.category)},
        {"code", make_string(error.code)},
        {"message", make_string(error.message)},
    };
    if (debug) {
        std::map<std::string, ValueHolder> details;
        for (const auto& [key, value] : error.details) {
            details.emplace(key, make_string(value));
        }
        payload.emplace("details", make_object(std::move(details)));
    }

    CommandOutput output{
        .data = make_object(std::move(payload)),
        .human =
            debug ? HumanView{std::string(error.message + " (" + error.category + ":" + error.code + ")")}
                  : HumanView{std::string(error.message)},
    };
    return render(output, format);
}

auto make_string(std::string value) -> ValueHolder {
    return ValueHolder{std::move(value)};
}

auto make_bool(bool value) -> ValueHolder {
    return ValueHolder{value};
}

auto make_int(std::int64_t value) -> ValueHolder {
    return ValueHolder{value};
}

auto make_array(std::vector<ValueHolder> value) -> ValueHolder {
    return ValueHolder{std::move(value)};
}

auto make_object(std::map<std::string, ValueHolder> value) -> ValueHolder {
    return ValueHolder{std::move(value)};
}

auto to_value(const domain::PluginInfo& info) -> ValueHolder {
    return make_object({
        {"backend", make_string(info.backend)},
        {"transport", make_string(info.transport)},
        {"plugin_name", make_string(info.plugin_name)},
        {"plugin_version", make_string(info.plugin_version)},
        {"plugin_available", make_bool(info.plugin_available)},
        {"socket_path", make_string(info.socket_path)},
        {"note", make_string(info.note)},
    });
}

auto to_value(const domain::ConnectionState& state) -> ValueHolder {
    return make_object({
        {"phase", make_string(domain::to_string(state.phase))},
        {"backend", make_string(state.backend)},
        {"connection", make_string(domain::to_string(state.connection))},
        {"server", make_string(state.server)},
        {"port", make_int(state.port)},
        {"nickname", make_string(state.nickname)},
        {"identity", make_string(state.identity)},
        {"profile", make_string(state.profile)},
        {"mode", make_string(state.mode)},
    });
}

auto to_value(const domain::ServerInfo& info) -> ValueHolder {
    return make_object({
        {"name", make_string(info.name)},
        {"host", make_string(info.host)},
        {"port", make_int(info.port)},
        {"backend", make_string(info.backend)},
        {"current_channel", maybe_channel(info.current_channel)},
        {"channel_count", make_int(static_cast<std::int64_t>(info.channel_count))},
        {"client_count", make_int(static_cast<std::int64_t>(info.client_count))},
    });
}

auto to_value(const domain::Channel& channel) -> ValueHolder {
    return make_object({
        {"id", make_string(domain::to_string(channel.id))},
        {"name", make_string(channel.name)},
        {"parent_id", maybe_channel(channel.parent_id)},
        {"client_count", make_int(static_cast<std::int64_t>(channel.client_count))},
        {"is_default", make_bool(channel.is_default)},
        {"subscribed", make_bool(channel.subscribed)},
    });
}

auto to_value(const domain::Client& client) -> ValueHolder {
    return make_object({
        {"id", make_string(domain::to_string(client.id))},
        {"nickname", make_string(client.nickname)},
        {"unique_identity", make_string(client.unique_identity)},
        {"channel_id", maybe_channel(client.channel_id)},
        {"self", make_bool(client.self)},
        {"talking", make_bool(client.talking)},
    });
}

auto to_value(const domain::Event& event) -> ValueHolder {
    std::map<std::string, ValueHolder> fields;
    for (const auto& [key, value] : event.fields) {
        fields.emplace(key, make_string(value));
    }

    return make_object({
        {"type", make_string(event.type)},
        {"summary", make_string(event.summary)},
        {"timestamp", make_string(format_time(event.at))},
        {"fields", make_object(std::move(fields))},
    });
}

auto to_value(const domain::Profile& profile) -> ValueHolder {
    return make_object({
        {"name", make_string(profile.name)},
        {"backend", make_string(profile.backend)},
        {"host", make_string(profile.host)},
        {"port", make_int(profile.port)},
        {"nickname", make_string(profile.nickname)},
        {"identity", make_string(profile.identity)},
        {"default_channel", make_string(profile.default_channel)},
        {"control_socket_path", make_string(profile.control_socket_path)},
    });
}

auto to_value(const std::vector<domain::Channel>& channels) -> ValueHolder {
    std::vector<ValueHolder> items;
    items.reserve(channels.size());
    for (const auto& channel : channels) {
        items.push_back(to_value(channel));
    }
    return make_array(std::move(items));
}

auto to_value(const std::vector<domain::Client>& clients) -> ValueHolder {
    std::vector<ValueHolder> items;
    items.reserve(clients.size());
    for (const auto& client : clients) {
        items.push_back(to_value(client));
    }
    return make_array(std::move(items));
}

auto to_value(const std::vector<domain::Event>& events) -> ValueHolder {
    std::vector<ValueHolder> items;
    items.reserve(events.size());
    for (const auto& event : events) {
        items.push_back(to_value(event));
    }
    return make_array(std::move(items));
}

auto to_value(const std::vector<domain::Profile>& profiles) -> ValueHolder {
    std::vector<ValueHolder> items;
    items.reserve(profiles.size());
    for (const auto& profile : profiles) {
        items.push_back(to_value(profile));
    }
    return make_array(std::move(items));
}

auto status_view(const domain::ConnectionState& state) -> Details {
    return Details{{
        {"State", domain::to_string(state.phase)},
        {"Backend", state.backend},
        {"Server", state.server + ":" + std::to_string(state.port)},
        {"Nickname", state.nickname},
        {"Profile", state.profile},
        {"Mode", state.mode},
    }};
}

auto server_view(const domain::ServerInfo& info) -> Details {
    return Details{{
        {"Name", info.name},
        {"Host", info.host},
        {"Port", std::to_string(info.port)},
        {"Backend", info.backend},
        {"CurrentChannel", info.current_channel.has_value() ? domain::to_string(*info.current_channel) : "-"},
        {"Channels", std::to_string(info.channel_count)},
        {"Clients", std::to_string(info.client_count)},
    }};
}

auto plugin_info_view(const domain::PluginInfo& info) -> Details {
    return Details{{
        {"Backend", info.backend},
        {"Transport", info.transport},
        {"Plugin", info.plugin_name},
        {"Version", info.plugin_version},
        {"Available", info.plugin_available ? "yes" : "no"},
        {"SocketPath", info.socket_path},
        {"Note", info.note},
    }};
}

auto profile_table(const std::vector<domain::Profile>& profiles, const std::string& active_profile) -> Table {
    Table table{{"Name", "Backend", "Host", "Port", "Nickname", "Active"}, {}};
    for (const auto& profile : profiles) {
        table.rows.push_back({
            profile.name,
            profile.backend,
            profile.host,
            std::to_string(profile.port),
            profile.nickname,
            profile.name == active_profile ? "yes" : "no",
        });
    }
    return table;
}

auto channel_table(const std::vector<domain::Channel>& channels) -> Table {
    Table table{{"ID", "Name", "Parent", "Clients", "Default"}, {}};
    for (const auto& channel : channels) {
        table.rows.push_back({
            domain::to_string(channel.id),
            channel.name,
            channel.parent_id.has_value() ? domain::to_string(*channel.parent_id) : "-",
            std::to_string(channel.client_count),
            channel.is_default ? "yes" : "no",
        });
    }
    return table;
}

auto client_table(const std::vector<domain::Client>& clients) -> Table {
    Table table{{"ID", "Nickname", "Channel", "Self", "Talking"}, {}};
    for (const auto& client : clients) {
        table.rows.push_back({
            domain::to_string(client.id),
            client.nickname,
            client.channel_id.has_value() ? domain::to_string(*client.channel_id) : "-",
            client.self ? "yes" : "no",
            client.talking ? "yes" : "no",
        });
    }
    return table;
}

auto channel_details(const domain::Channel& channel) -> Details {
    return Details{{
        {"ID", domain::to_string(channel.id)},
        {"Name", channel.name},
        {"Parent", channel.parent_id.has_value() ? domain::to_string(*channel.parent_id) : "-"},
        {"Clients", std::to_string(channel.client_count)},
        {"Default", channel.is_default ? "yes" : "no"},
        {"Subscribed", channel.subscribed ? "yes" : "no"},
    }};
}

auto client_details(const domain::Client& client) -> Details {
    return Details{{
        {"ID", domain::to_string(client.id)},
        {"Nickname", client.nickname},
        {"UniqueIdentity", client.unique_identity},
        {"Channel", client.channel_id.has_value() ? domain::to_string(*client.channel_id) : "-"},
        {"Self", client.self ? "yes" : "no"},
        {"Talking", client.talking ? "yes" : "no"},
    }};
}

auto event_table(const std::vector<domain::Event>& events) -> Table {
    Table table{{"Time", "Type", "Summary"}, {}};
    for (const auto& event : events) {
        table.rows.push_back({format_time(event.at), event.type, event.summary});
    }
    return table;
}

}  // namespace teamspeak_cli::output
