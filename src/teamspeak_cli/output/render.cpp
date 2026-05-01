#include "teamspeak_cli/output/render.hpp"

#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

#include "teamspeak_cli/util/strings.hpp"

namespace teamspeak_cli::output {
namespace {

auto escape_json(const std::string& value) -> std::string {
    std::ostringstream out;
    constexpr char hex_digits[] = "0123456789abcdef";
    for (const unsigned char ch : value) {
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
            case '\r':
                out << "\\r";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (ch < 0x20) {
                    out << "\\u00" << hex_digits[ch >> 4] << hex_digits[ch & 0x0f];
                } else {
                    out << static_cast<char>(ch);
                }
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

auto is_ascii_upper(char ch) -> bool {
    return ch >= 'A' && ch <= 'Z';
}

auto is_ascii_lower(char ch) -> bool {
    return ch >= 'a' && ch <= 'z';
}

auto is_ascii_digit(char ch) -> bool {
    return ch >= '0' && ch <= '9';
}

auto ascii_lower(char ch) -> char {
    if (is_ascii_upper(ch)) {
        return static_cast<char>(ch - 'A' + 'a');
    }
    return ch;
}

auto should_split_label_word(std::string_view label, std::size_t index) -> bool {
    if (index == 0 || index >= label.size()) {
        return false;
    }

    const char current = label[index];
    const char previous = label[index - 1];
    if (!is_ascii_upper(current)) {
        return false;
    }
    if (is_ascii_lower(previous) || is_ascii_digit(previous)) {
        return true;
    }
    if (is_ascii_upper(previous) && index + 1 < label.size() && is_ascii_lower(label[index + 1])) {
        return true;
    }
    return false;
}

auto human_detail_label(std::string_view label) -> std::string {
    std::string normalized;
    normalized.reserve(label.size() + 4);
    bool after_space = false;

    for (std::size_t index = 0; index < label.size(); ++index) {
        const char ch = label[index];
        if (ch == '_' || ch == '-') {
            if (!normalized.empty() && normalized.back() != ' ') {
                normalized.push_back(' ');
            }
            after_space = true;
            continue;
        }
        if (should_split_label_word(label, index) && !normalized.empty() && normalized.back() != ' ') {
            normalized.push_back(' ');
            after_space = true;
        }

        if (after_space && is_ascii_upper(ch)) {
            normalized.push_back(ascii_lower(ch));
        } else {
            normalized.push_back(ch);
        }
        after_space = false;
    }

    return normalized;
}

auto render_details_impl(const Details& details) -> std::string {
    if (details.fields.empty()) {
        return "(empty)";
    }

    std::vector<std::pair<std::string, std::string>> fields;
    fields.reserve(details.fields.size());
    for (const auto& [key, value] : details.fields) {
        fields.emplace_back(human_detail_label(key), value);
    }

    std::size_t width = 0;
    for (const auto& [key, _] : fields) {
        width = std::max(width, key.size());
    }

    std::ostringstream out;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const auto& [key, value] = fields[index];
        out << std::left << std::setw(static_cast<int>(width)) << key << "  " << value;
        if (index + 1 != fields.size()) {
            out << '\n';
        }
    }
    return out.str();
}

auto ensure_sentence(std::string value) -> std::string {
    if (value.empty()) {
        return value;
    }
    const char last = value.back();
    if (last == '.' || last == '!' || last == '?') {
        return value;
    }
    value.push_back('.');
    return value;
}

auto endpoint_for(std::string_view server, std::uint16_t port) -> std::string {
    if (server.empty() && port == 0) {
        return "the requested server";
    }
    if (server.empty()) {
        return "port " + std::to_string(port);
    }
    if (port == 0) {
        return std::string(server);
    }
    return std::string(server) + ":" + std::to_string(port);
}

auto event_endpoint(const domain::Event& event) -> std::optional<std::string> {
    const auto server_it = event.fields.find("server");
    if (server_it == event.fields.end()) {
        return std::nullopt;
    }

    std::uint16_t port = 0;
    if (const auto port_it = event.fields.find("port"); port_it != event.fields.end()) {
        const auto parsed = util::parse_u64(port_it->second);
        if (parsed.has_value()) {
            port = static_cast<std::uint16_t>(*parsed);
        }
    }
    return endpoint_for(server_it->second, port);
}

auto size_value(std::size_t value) -> ValueHolder {
    if (value > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        return make_int(std::numeric_limits<std::int64_t>::max());
    }
    return make_int(static_cast<std::int64_t>(value));
}

auto device_binding_value(const domain::AudioDeviceBinding& binding) -> ValueHolder {
    return make_object({
        {"known", make_bool(binding.known)},
        {"mode", make_string(binding.mode)},
        {"device", make_string(binding.device)},
        {"is_default", make_bool(binding.is_default)},
    });
}

auto media_diagnostics_value(const domain::MediaDiagnostics& diagnostics) -> ValueHolder {
    return make_object({
        {"capture", device_binding_value(diagnostics.capture)},
        {"playback", device_binding_value(diagnostics.playback)},
        {"pulse_sink", make_string(diagnostics.pulse_sink)},
        {"pulse_source", make_string(diagnostics.pulse_source)},
        {"pulse_source_is_monitor", make_bool(diagnostics.pulse_source_is_monitor)},
        {"consumer_connected", make_bool(diagnostics.consumer_connected)},
        {"playback_active", make_bool(diagnostics.playback_active)},
        {"queued_playback_samples", size_value(diagnostics.queued_playback_samples)},
        {"active_speaker_count", size_value(diagnostics.active_speaker_count)},
        {"dropped_audio_chunks", size_value(diagnostics.dropped_audio_chunks)},
        {"dropped_playback_chunks", size_value(diagnostics.dropped_playback_chunks)},
        {"last_error", make_string(diagnostics.last_error)},
        {"custom_capture_device_registered", make_bool(diagnostics.custom_capture_device_registered)},
        {"custom_capture_device_id", make_string(diagnostics.custom_capture_device_id)},
        {"custom_capture_device_name", make_string(diagnostics.custom_capture_device_name)},
        {"custom_capture_path_available", make_bool(diagnostics.custom_capture_path_available)},
        {"injected_playback_attached_to_capture", make_bool(diagnostics.injected_playback_attached_to_capture)},
        {"captured_voice_edit_attached", make_bool(diagnostics.captured_voice_edit_attached)},
        {"transmit_path_ready", make_bool(diagnostics.transmit_path_ready)},
        {"transmit_path", make_string(diagnostics.transmit_path)},
    });
}

auto yes_no(bool value) -> std::string {
    return value ? "yes" : "no";
}

auto device_binding_text(const domain::AudioDeviceBinding& binding) -> std::string {
    if (!binding.known) {
        return "unknown";
    }
    std::string value = binding.mode.empty() ? "default" : binding.mode;
    value += "/";
    value += binding.device.empty() ? "default" : binding.device;
    if (binding.is_default) {
        value += " (default)";
    }
    return value;
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

auto error_hints(const domain::Error& error) -> std::vector<std::string> {
    std::vector<std::string> hints;
    constexpr std::string_view kHintPrefix = "hint_";
    for (const auto& [key, value] : error.details) {
        if (key.rfind(kHintPrefix, 0) == 0) {
            hints.push_back(value);
        }
    }
    return hints;
}

auto error_debug_details(const domain::Error& error) -> std::map<std::string, ValueHolder> {
    std::map<std::string, ValueHolder> details;
    constexpr std::string_view kHintPrefix = "hint_";
    for (const auto& [key, value] : error.details) {
        if (key.rfind(kHintPrefix, 0) == 0) {
            continue;
        }
        details.emplace(key, make_string(value));
    }
    return details;
}

auto render_error_human(
    const domain::Error& error,
    const std::vector<std::string>& hints,
    bool debug
) -> std::string {
    std::ostringstream out;
    out << error.message;
    if (debug) {
        out << " (" << error.category << ":" << error.code << ")";
    }
    if (!hints.empty()) {
        out << '\n' << "Next steps:";
        for (std::size_t index = 0; index < hints.size(); ++index) {
            out << '\n' << (index + 1) << ". " << hints[index];
        }
    }
    return out.str();
}

auto output_error(std::string code, std::string message, domain::ExitCode exit_code) -> domain::Error {
    return domain::make_error("output", std::move(code), std::move(message), exit_code);
}

auto validate_field_path(const std::string& path) -> domain::Result<std::vector<std::string>> {
    if (path.empty()) {
        return domain::fail<std::vector<std::string>>(output_error(
            "invalid_field_path",
            "--field requires a non-empty dot-separated object path",
            domain::ExitCode::usage
        ));
    }

    std::vector<std::string> parts;
    for (const auto& part : util::split(path, '.')) {
        if (part.empty()) {
            return domain::fail<std::vector<std::string>>(output_error(
                "invalid_field_path",
                "invalid --field path `" + path + "`: path segments must not be empty",
                domain::ExitCode::usage
            ));
        }
        parts.push_back(part);
    }
    return domain::ok(std::move(parts));
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
    const auto hints = error_hints(error);
    std::map<std::string, ValueHolder> payload{
        {"category", make_string(error.category)},
        {"code", make_string(error.code)},
        {"message", make_string(error.message)},
    };
    if (!hints.empty()) {
        std::vector<ValueHolder> hint_values;
        hint_values.reserve(hints.size());
        for (const auto& hint : hints) {
            hint_values.push_back(make_string(hint));
        }
        payload.emplace("hints", make_array(std::move(hint_values)));
    }
    if (debug) {
        auto details = error_debug_details(error);
        payload.emplace("details", make_object(std::move(details)));
    }

    CommandOutput output{
        .data = make_object(std::move(payload)),
        .human = HumanView{render_error_human(error, hints, debug)},
    };
    return render(output, format);
}

auto render_details_block(const Details& details) -> std::string {
    return render_details_impl(details);
}

auto extract_field(const ValueHolder& value, const std::string& path) -> domain::Result<ValueHolder> {
    auto parts = validate_field_path(path);
    if (!parts) {
        return domain::fail<ValueHolder>(parts.error());
    }

    const ValueHolder* current = &value;
    std::string prefix;
    for (const auto& part : parts.value()) {
        const auto* object = std::get_if<std::map<std::string, ValueHolder>>(&current->value);
        if (object == nullptr) {
            const std::string location = prefix.empty() ? std::string("<root>") : prefix;
            return domain::fail<ValueHolder>(output_error(
                "field_not_object",
                "field path `" + path + "` cannot descend through non-object value at `" + location + "`",
                domain::ExitCode::usage
            ));
        }

        const auto found = object->find(part);
        if (found == object->end()) {
            const std::string location = prefix.empty() ? std::string("<root>") : prefix;
            return domain::fail<ValueHolder>(output_error(
                "field_not_found",
                "field `" + path + "` was not found: missing `" + part + "` under `" + location + "`",
                domain::ExitCode::not_found
            ));
        }

        current = &found->second;
        prefix = prefix.empty() ? part : prefix + "." + part;
    }

    return domain::ok(*current);
}

auto render_extracted_field(const ValueHolder& value) -> domain::Result<std::string> {
    return std::visit(
        [&](const auto& scalar) -> domain::Result<std::string> {
            using T = std::decay_t<decltype(scalar)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) {
                return domain::ok(std::string("null"));
            } else if constexpr (std::is_same_v<T, bool>) {
                return domain::ok(std::string(scalar ? "true" : "false"));
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return domain::ok(std::to_string(scalar));
            } else if constexpr (std::is_same_v<T, std::string>) {
                return domain::ok(scalar);
            } else {
                return domain::fail<std::string>(output_error(
                    "field_not_scalar",
                    "--field can only render scalar values; the selected value is an object or array",
                    domain::ExitCode::usage
                ));
            }
        },
        value.value
    );
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
        {"media_transport", make_string(info.media_transport)},
        {"media_socket_path", make_string(info.media_socket_path)},
        {"media_format", make_string(info.media_format)},
        {"media_diagnostics", media_diagnostics_value(info.media_diagnostics)},
        {"note", make_string(info.note)},
    });
}

auto to_value(const domain::MediaDiagnostics& diagnostics) -> ValueHolder {
    return media_diagnostics_value(diagnostics);
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

auto connection_status_view(const domain::ConnectionState& state) -> std::string {
    std::ostringstream out;
    const auto target = endpoint_for(state.server, state.port);
    const std::string nickname = state.nickname.empty() ? "the configured nickname" : state.nickname;

    if (state.phase == domain::ConnectionPhase::connected) {
        out << "Connected to " << target << " as " << nickname << '.';
    } else if (state.phase == domain::ConnectionPhase::connecting) {
        out << "The TeamSpeak client is still connecting to " << target << " as " << nickname << '.';
    } else {
        out << "There is no active TeamSpeak server connection.";
    }

    out << "\n\nConnection Context\n";
    out << render_details_impl(Details{{
        {"State", domain::to_string(state.phase)},
        {"Server", target},
        {"Nickname", nickname},
        {"Profile", state.profile},
        {"Backend", state.backend},
        {"Mode", state.mode},
    }});
    return out.str();
}

auto connect_progress_message(const domain::Event& event) -> std::string {
    if (event.type == "connection.requested") {
        const auto target = event_endpoint(event).value_or("the requested server");
        return "TeamSpeak accepted the request to connect to " + target + ".";
    }
    if (event.type == "connection.connecting") {
        return "The TeamSpeak client started establishing the server connection.";
    }
    if (event.type == "connection.connected") {
        return "The TeamSpeak client reported that the connection is ready.";
    }
    if (event.type == "connection.disconnected") {
        return "The connection closed before it became ready.";
    }
    if (event.type == "connection.error") {
        return "TeamSpeak reported a connection error while establishing the server connection.";
    }
    if (event.type == "server.error") {
        if (!event.summary.empty()) {
            return ensure_sentence("TeamSpeak reported a server error: " + event.summary);
        }
        return "TeamSpeak reported a server error.";
    }
    if (!event.summary.empty()) {
        return ensure_sentence(event.summary);
    }
    return ensure_sentence("TeamSpeak reported " + event.type);
}

auto has_terminal_connect_event(const std::vector<domain::Event>& lifecycle) -> bool {
    for (const auto& event : lifecycle) {
        if (event.type == "connection.connected" || event.type == "connection.disconnected" ||
            event.type == "connection.error" || event.type == "server.error") {
            return true;
        }
    }
    return false;
}

auto connect_view(
    const domain::ConnectionState& state,
    const std::vector<domain::Event>& lifecycle,
    bool connected,
    bool timed_out,
    std::chrono::milliseconds timeout,
    bool include_lifecycle
) -> std::string {
    std::ostringstream out;
    const auto target = endpoint_for(state.server, state.port);
    const std::string nickname = state.nickname.empty() ? "the configured nickname" : state.nickname;
    if (connected) {
        out << "Connected to " << target << " as " << nickname << '.';
    } else if (timed_out) {
        out << "The TeamSpeak client did not finish connecting to " << target << " within "
            << timeout.count() << " ms.";
        if (state.backend == "plugin" && !has_terminal_connect_event(lifecycle)) {
            out << "\n\nIf this was the first headless TeamSpeak launch, hidden TeamSpeak setup dialogs may still "
                   "be waiting on the X11 display. Complete the TeamSpeak license and initial identity setup once "
                   "on a visible display, or inspect ts plugin info and ts client logs before retrying.";
        }
    } else {
        out << "The TeamSpeak client did not complete the connection to " << target << '.';
    }

    out << "\n\nConnection Context\n";
    out << render_details_impl(Details{{
        {"State", domain::to_string(state.phase)},
        {"Server", target},
        {"Nickname", nickname},
        {"Profile", state.profile},
        {"Backend", state.backend},
        {"Mode", state.mode},
    }});

    if (!include_lifecycle) {
        return out.str();
    }

    out << "\n\nWhat Happened\n";
    if (lifecycle.empty()) {
        out << "TeamSpeak did not emit any connection updates before the command finished.";
        return out.str();
    }
    for (std::size_t index = 0; index < lifecycle.size(); ++index) {
        out << "- " << connect_progress_message(lifecycle[index]);
        if (index + 1 != lifecycle.size()) {
            out << '\n';
        }
    }
    return out.str();
}

auto disconnect_progress_message(const domain::Event& event) -> std::string {
    if (event.type == "connection.disconnected") {
        return "The TeamSpeak client reported that the server connection is closed.";
    }
    if (event.type == "connection.error") {
        return "TeamSpeak reported an error while closing the server connection.";
    }
    if (event.type == "server.error") {
        if (!event.summary.empty()) {
            return ensure_sentence("TeamSpeak reported a server error while disconnecting: " + event.summary);
        }
        return "TeamSpeak reported a server error while disconnecting.";
    }
    if (event.type == "connection.connected") {
        return "The TeamSpeak client still considers the server connection active.";
    }
    if (!event.summary.empty()) {
        return ensure_sentence(event.summary);
    }
    return ensure_sentence("TeamSpeak reported " + event.type);
}

auto disconnect_view(
    const domain::ConnectionState& state,
    const std::vector<domain::Event>& lifecycle,
    bool disconnected,
    bool timed_out,
    std::chrono::milliseconds timeout,
    bool include_lifecycle
) -> std::string {
    std::ostringstream out;
    const auto target = endpoint_for(state.server, state.port);
    const std::string nickname = state.nickname.empty() ? "the configured nickname" : state.nickname;

    if (disconnected && lifecycle.empty()) {
        out << "No active TeamSpeak server connection was present.";
    } else if (disconnected && !state.server.empty()) {
        out << "Disconnected from " << target << '.';
    } else if (disconnected) {
        out << "Disconnected the TeamSpeak client from the current server.";
    } else if (timed_out) {
        out << "The TeamSpeak client did not finish disconnecting within " << timeout.count() << " ms.";
    } else {
        out << "The TeamSpeak client did not complete the disconnect request.";
    }

    out << "\n\nConnection Context\n";
    out << render_details_impl(Details{{
        {"State", domain::to_string(state.phase)},
        {"Server", target},
        {"Nickname", nickname},
        {"Profile", state.profile},
        {"Backend", state.backend},
        {"Mode", state.mode},
    }});

    if (!include_lifecycle) {
        return out.str();
    }

    out << "\n\nWhat Happened\n";
    if (lifecycle.empty()) {
        out << "TeamSpeak did not emit any disconnect updates before the command finished.";
        return out.str();
    }
    for (std::size_t index = 0; index < lifecycle.size(); ++index) {
        out << "- " << disconnect_progress_message(lifecycle[index]);
        if (index + 1 != lifecycle.size()) {
            out << '\n';
        }
    }
    return out.str();
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
        {"MediaTransport", info.media_transport},
        {"MediaSocketPath", info.media_socket_path},
        {"MediaFormat", info.media_format},
        {"EffectiveCapture", device_binding_text(info.media_diagnostics.capture)},
        {"EffectivePlayback", device_binding_text(info.media_diagnostics.playback)},
        {"PulseSink", info.media_diagnostics.pulse_sink.empty() ? "-" : info.media_diagnostics.pulse_sink},
        {"PulseSource", info.media_diagnostics.pulse_source.empty() ? "-" : info.media_diagnostics.pulse_source},
        {"PulseSourceIsMonitor", yes_no(info.media_diagnostics.pulse_source_is_monitor)},
        {"MediaConsumerConnected", yes_no(info.media_diagnostics.consumer_connected)},
        {"MediaPlaybackActive", yes_no(info.media_diagnostics.playback_active)},
        {"QueuedPlaybackSamples", std::to_string(info.media_diagnostics.queued_playback_samples)},
        {"DroppedPlaybackChunks", std::to_string(info.media_diagnostics.dropped_playback_chunks)},
        {"InjectedPlaybackAttached", yes_no(info.media_diagnostics.injected_playback_attached_to_capture)},
        {"CapturedVoiceEditAttached", yes_no(info.media_diagnostics.captured_voice_edit_attached)},
        {"TransmitPath", info.media_diagnostics.transmit_path.empty() ? "-" : info.media_diagnostics.transmit_path},
        {"TransmitPathReady", yes_no(info.media_diagnostics.transmit_path_ready)},
        {"Note", info.note},
    }};
}

auto media_diagnostics_view(const domain::MediaDiagnostics& diagnostics) -> Details {
    return Details{{
        {"EffectiveCapture", device_binding_text(diagnostics.capture)},
        {"EffectivePlayback", device_binding_text(diagnostics.playback)},
        {"PulseSink", diagnostics.pulse_sink.empty() ? "-" : diagnostics.pulse_sink},
        {"PulseSource", diagnostics.pulse_source.empty() ? "-" : diagnostics.pulse_source},
        {"PulseSourceIsMonitor", yes_no(diagnostics.pulse_source_is_monitor)},
        {"MediaConsumerConnected", yes_no(diagnostics.consumer_connected)},
        {"MediaPlaybackActive", yes_no(diagnostics.playback_active)},
        {"QueuedPlaybackSamples", std::to_string(diagnostics.queued_playback_samples)},
        {"ActiveSpeakers", std::to_string(diagnostics.active_speaker_count)},
        {"DroppedIngressAudioChunks", std::to_string(diagnostics.dropped_audio_chunks)},
        {"DroppedPlaybackChunks", std::to_string(diagnostics.dropped_playback_chunks)},
        {"LastMediaError", diagnostics.last_error.empty() ? "-" : diagnostics.last_error},
        {"CustomCaptureRegistered", yes_no(diagnostics.custom_capture_device_registered)},
        {"CustomCaptureDevice", diagnostics.custom_capture_device_id.empty() ? "-" : diagnostics.custom_capture_device_id},
        {"CustomCapturePathAvailable", yes_no(diagnostics.custom_capture_path_available)},
        {"InjectedPlaybackAttached", yes_no(diagnostics.injected_playback_attached_to_capture)},
        {"CapturedVoiceEditAttached", yes_no(diagnostics.captured_voice_edit_attached)},
        {"TransmitPath", diagnostics.transmit_path.empty() ? "-" : diagnostics.transmit_path},
        {"TransmitPathReady", yes_no(diagnostics.transmit_path_ready)},
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
