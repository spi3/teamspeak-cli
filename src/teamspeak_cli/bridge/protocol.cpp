#include "teamspeak_cli/bridge/protocol.hpp"

#include <chrono>
#include <cstring>
#include <sstream>

#include <unistd.h>

#include "teamspeak_cli/util/strings.hpp"

namespace teamspeak_cli::bridge::protocol {
namespace {

auto protocol_error(std::string code, std::string message) -> domain::Error {
    return domain::make_error("bridge", std::move(code), std::move(message), domain::ExitCode::internal);
}

auto parse_bool(const std::string& value) -> domain::Result<bool> {
    if (value == "1") {
        return domain::ok(true);
    }
    if (value == "0") {
        return domain::ok(false);
    }
    return domain::fail<bool>(protocol_error("invalid_bool", "invalid bridge boolean"));
}

auto parse_required_u64(const std::string& value, std::string_view field_name) -> domain::Result<std::uint64_t> {
    const auto parsed = util::parse_u64(value);
    if (!parsed.has_value()) {
        return domain::fail<std::uint64_t>(protocol_error(
            "invalid_number", "invalid bridge integer for " + std::string(field_name)
        ));
    }
    return domain::ok(*parsed);
}

auto parse_phase(const std::string& value) -> domain::Result<domain::ConnectionPhase> {
    if (value == "connected") {
        return domain::ok(domain::ConnectionPhase::connected);
    }
    if (value == "connecting") {
        return domain::ok(domain::ConnectionPhase::connecting);
    }
    if (value == "disconnected") {
        return domain::ok(domain::ConnectionPhase::disconnected);
    }
    return domain::fail<domain::ConnectionPhase>(protocol_error(
        "invalid_phase", "invalid bridge connection phase"
    ));
}

auto parse_optional_channel(const std::string& value) -> domain::Result<std::optional<domain::ChannelId>> {
    if (value == "0") {
        return domain::ok(std::optional<domain::ChannelId>{});
    }
    auto parsed = parse_required_u64(value, "channel_id");
    if (!parsed) {
        return domain::fail<std::optional<domain::ChannelId>>(parsed.error());
    }
    return domain::ok(std::optional<domain::ChannelId>{domain::ChannelId{parsed.value()}});
}

auto decode_string_field(
    const Fields& fields,
    std::size_t index,
    std::string_view field_name
) -> domain::Result<std::string> {
    if (index >= fields.size()) {
        return domain::fail<std::string>(protocol_error(
            "missing_field", "missing bridge field: " + std::string(field_name)
        ));
    }
    return hex_decode(fields[index]);
}

auto encode_event_line(const domain::Event& event) -> Fields {
    Fields fields{
        "event",
        hex_encode(event.type),
        hex_encode(event.summary),
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                           event.at.time_since_epoch()
                       )
                           .count()),
        std::to_string(event.fields.size()),
    };
    for (const auto& [key, value] : event.fields) {
        fields.push_back(hex_encode(key));
        fields.push_back(hex_encode(value));
    }
    return fields;
}

}  // namespace

auto split_fields(std::string_view line) -> Fields {
    Fields fields;
    std::string current;
    for (const char ch : line) {
        if (ch == '\t') {
            fields.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    fields.push_back(current);
    return fields;
}

auto join_fields(const Fields& fields) -> std::string {
    std::ostringstream out;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index != 0) {
            out << '\t';
        }
        out << fields[index];
    }
    out << '\n';
    return out.str();
}

auto hex_encode(std::string_view value) -> std::string {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(value.size() * 2);
    for (const unsigned char ch : value) {
        out.push_back(digits[(ch >> 4U) & 0x0FU]);
        out.push_back(digits[ch & 0x0FU]);
    }
    return out;
}

auto hex_decode(std::string_view value) -> domain::Result<std::string> {
    if (value.size() % 2 != 0) {
        return domain::fail<std::string>(protocol_error(
            "invalid_hex", "bridge hex field has odd length"
        ));
    }

    std::string out;
    out.reserve(value.size() / 2);
    for (std::size_t index = 0; index < value.size(); index += 2) {
        const auto hi = std::tolower(static_cast<unsigned char>(value[index]));
        const auto lo = std::tolower(static_cast<unsigned char>(value[index + 1]));
        const auto from_hex = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') {
                return ch - '0';
            }
            if (ch >= 'a' && ch <= 'f') {
                return ch - 'a' + 10;
            }
            return -1;
        };
        const int hi_value = from_hex(static_cast<char>(hi));
        const int lo_value = from_hex(static_cast<char>(lo));
        if (hi_value < 0 || lo_value < 0) {
            return domain::fail<std::string>(protocol_error(
                "invalid_hex", "bridge hex field contains a non-hex character"
            ));
        }
        out.push_back(static_cast<char>((hi_value << 4) | lo_value));
    }
    return domain::ok(std::move(out));
}

auto read_line(int fd, std::string& line) -> bool {
    line.clear();
    char ch = '\0';
    while (true) {
        const auto received = ::read(fd, &ch, 1);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            return false;
        }
        if (ch == '\n') {
            return true;
        }
        line.push_back(ch);
    }
}

auto write_line(int fd, const Fields& fields) -> bool {
    const std::string line = join_fields(fields);
    const char* data = line.data();
    std::size_t remaining = line.size();
    while (remaining > 0) {
        const auto written = ::write(fd, data, remaining);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        data += written;
        remaining -= static_cast<std::size_t>(written);
    }
    return true;
}

auto encode_error(const domain::Error& error) -> Fields {
    return Fields{
        "error",
        hex_encode(error.category),
        hex_encode(error.code),
        hex_encode(error.message),
        std::to_string(static_cast<int>(error.exit_code)),
    };
}

auto decode_error(const Fields& fields) -> domain::Error {
    if (fields.size() != 5 || fields[0] != "error") {
        return protocol_error("invalid_error", "invalid bridge error envelope");
    }
    auto category = hex_decode(fields[1]);
    auto code = hex_decode(fields[2]);
    auto message = hex_decode(fields[3]);
    auto exit_code = parse_required_u64(fields[4], "exit_code");
    if (!category || !code || !message || !exit_code) {
        return protocol_error("invalid_error", "invalid bridge error envelope");
    }
    return domain::Error{
        .category = category.value(),
        .code = code.value(),
        .message = message.value(),
        .exit_code = static_cast<domain::ExitCode>(exit_code.value()),
        .details = {},
    };
}

auto encode(const domain::PluginInfo& info) -> std::vector<Fields> {
    return {{
        "plugin_info",
        hex_encode(info.backend),
        hex_encode(info.transport),
        hex_encode(info.plugin_name),
        hex_encode(info.plugin_version),
        info.plugin_available ? "1" : "0",
        hex_encode(info.socket_path),
        hex_encode(info.note),
    }};
}

auto decode_plugin_info(const std::vector<Fields>& lines) -> domain::Result<domain::PluginInfo> {
    if (lines.size() != 1 || lines[0].size() != 8 || lines[0][0] != "plugin_info") {
        return domain::fail<domain::PluginInfo>(protocol_error(
            "invalid_plugin_info", "invalid plugin info payload"
        ));
    }
    auto backend = decode_string_field(lines[0], 1, "backend");
    auto transport = decode_string_field(lines[0], 2, "transport");
    auto plugin_name = decode_string_field(lines[0], 3, "plugin_name");
    auto plugin_version = decode_string_field(lines[0], 4, "plugin_version");
    auto available = parse_bool(lines[0][5]);
    auto socket_path = decode_string_field(lines[0], 6, "socket_path");
    auto note = decode_string_field(lines[0], 7, "note");
    if (!backend) {
        return domain::fail<domain::PluginInfo>(backend.error());
    }
    if (!transport) {
        return domain::fail<domain::PluginInfo>(transport.error());
    }
    if (!plugin_name) {
        return domain::fail<domain::PluginInfo>(plugin_name.error());
    }
    if (!plugin_version) {
        return domain::fail<domain::PluginInfo>(plugin_version.error());
    }
    if (!available) {
        return domain::fail<domain::PluginInfo>(available.error());
    }
    if (!socket_path) {
        return domain::fail<domain::PluginInfo>(socket_path.error());
    }
    if (!note) {
        return domain::fail<domain::PluginInfo>(note.error());
    }
    return domain::ok(domain::PluginInfo{
        .backend = backend.value(),
        .transport = transport.value(),
        .plugin_name = plugin_name.value(),
        .plugin_version = plugin_version.value(),
        .plugin_available = available.value(),
        .socket_path = socket_path.value(),
        .note = note.value(),
    });
}

auto encode(const domain::ConnectionState& state) -> std::vector<Fields> {
    return {{
        "connection_state",
        domain::to_string(state.phase),
        hex_encode(state.backend),
        std::to_string(state.connection.value),
        hex_encode(state.server),
        std::to_string(state.port),
        hex_encode(state.nickname),
        hex_encode(state.identity),
        hex_encode(state.profile),
        hex_encode(state.mode),
    }};
}

auto decode_connection_state(const std::vector<Fields>& lines) -> domain::Result<domain::ConnectionState> {
    if (lines.size() != 1 || lines[0].size() != 10 || lines[0][0] != "connection_state") {
        return domain::fail<domain::ConnectionState>(protocol_error(
            "invalid_connection_state", "invalid connection state payload"
        ));
    }
    auto phase = parse_phase(lines[0][1]);
    auto backend = decode_string_field(lines[0], 2, "backend");
    auto connection = parse_required_u64(lines[0][3], "connection");
    auto server = decode_string_field(lines[0], 4, "server");
    auto port = parse_required_u64(lines[0][5], "port");
    auto nickname = decode_string_field(lines[0], 6, "nickname");
    auto identity = decode_string_field(lines[0], 7, "identity");
    auto profile = decode_string_field(lines[0], 8, "profile");
    auto mode = decode_string_field(lines[0], 9, "mode");
    if (!phase) {
        return domain::fail<domain::ConnectionState>(phase.error());
    }
    if (!backend) {
        return domain::fail<domain::ConnectionState>(backend.error());
    }
    if (!connection) {
        return domain::fail<domain::ConnectionState>(connection.error());
    }
    if (!server) {
        return domain::fail<domain::ConnectionState>(server.error());
    }
    if (!port) {
        return domain::fail<domain::ConnectionState>(port.error());
    }
    if (!nickname) {
        return domain::fail<domain::ConnectionState>(nickname.error());
    }
    if (!identity) {
        return domain::fail<domain::ConnectionState>(identity.error());
    }
    if (!profile) {
        return domain::fail<domain::ConnectionState>(profile.error());
    }
    if (!mode) {
        return domain::fail<domain::ConnectionState>(mode.error());
    }
    return domain::ok(domain::ConnectionState{
        .phase = phase.value(),
        .backend = backend.value(),
        .connection = domain::ConnectionHandle{connection.value()},
        .server = server.value(),
        .port = static_cast<std::uint16_t>(port.value()),
        .nickname = nickname.value(),
        .identity = identity.value(),
        .profile = profile.value(),
        .mode = mode.value(),
    });
}

auto encode(const domain::ServerInfo& info) -> std::vector<Fields> {
    return {{
        "server_info",
        hex_encode(info.name),
        hex_encode(info.host),
        std::to_string(info.port),
        hex_encode(info.backend),
        info.current_channel.has_value() ? std::to_string(info.current_channel->value) : "0",
        std::to_string(info.channel_count),
        std::to_string(info.client_count),
    }};
}

auto decode_server_info(const std::vector<Fields>& lines) -> domain::Result<domain::ServerInfo> {
    if (lines.size() != 1 || lines[0].size() != 8 || lines[0][0] != "server_info") {
        return domain::fail<domain::ServerInfo>(protocol_error(
            "invalid_server_info", "invalid server info payload"
        ));
    }
    auto name = decode_string_field(lines[0], 1, "name");
    auto host = decode_string_field(lines[0], 2, "host");
    auto port = parse_required_u64(lines[0][3], "port");
    auto backend = decode_string_field(lines[0], 4, "backend");
    auto channel = parse_optional_channel(lines[0][5]);
    auto channel_count = parse_required_u64(lines[0][6], "channel_count");
    auto client_count = parse_required_u64(lines[0][7], "client_count");
    if (!name) {
        return domain::fail<domain::ServerInfo>(name.error());
    }
    if (!host) {
        return domain::fail<domain::ServerInfo>(host.error());
    }
    if (!port) {
        return domain::fail<domain::ServerInfo>(port.error());
    }
    if (!backend) {
        return domain::fail<domain::ServerInfo>(backend.error());
    }
    if (!channel) {
        return domain::fail<domain::ServerInfo>(channel.error());
    }
    if (!channel_count) {
        return domain::fail<domain::ServerInfo>(channel_count.error());
    }
    if (!client_count) {
        return domain::fail<domain::ServerInfo>(client_count.error());
    }
    return domain::ok(domain::ServerInfo{
        .name = name.value(),
        .host = host.value(),
        .port = static_cast<std::uint16_t>(port.value()),
        .backend = backend.value(),
        .current_channel = channel.value(),
        .channel_count = static_cast<std::size_t>(channel_count.value()),
        .client_count = static_cast<std::size_t>(client_count.value()),
    });
}

auto encode(const domain::Channel& channel) -> Fields {
    return Fields{
        "channel",
        std::to_string(channel.id.value),
        hex_encode(channel.name),
        channel.parent_id.has_value() ? std::to_string(channel.parent_id->value) : "0",
        std::to_string(channel.client_count),
        channel.is_default ? "1" : "0",
        channel.subscribed ? "1" : "0",
    };
}

auto encode_channels(const std::vector<domain::Channel>& channels) -> std::vector<Fields> {
    std::vector<Fields> lines;
    lines.reserve(channels.size());
    for (const auto& channel : channels) {
        lines.push_back(encode(channel));
    }
    return lines;
}

auto decode_channel(const Fields& fields) -> domain::Result<domain::Channel> {
    if (fields.size() != 7 || fields[0] != "channel") {
        return domain::fail<domain::Channel>(protocol_error(
            "invalid_channel", "invalid channel payload"
        ));
    }
    auto id = parse_required_u64(fields[1], "channel_id");
    auto name = decode_string_field(fields, 2, "channel_name");
    auto parent = parse_optional_channel(fields[3]);
    auto client_count = parse_required_u64(fields[4], "client_count");
    auto is_default = parse_bool(fields[5]);
    auto subscribed = parse_bool(fields[6]);
    if (!id) {
        return domain::fail<domain::Channel>(id.error());
    }
    if (!name) {
        return domain::fail<domain::Channel>(name.error());
    }
    if (!parent) {
        return domain::fail<domain::Channel>(parent.error());
    }
    if (!client_count) {
        return domain::fail<domain::Channel>(client_count.error());
    }
    if (!is_default) {
        return domain::fail<domain::Channel>(is_default.error());
    }
    if (!subscribed) {
        return domain::fail<domain::Channel>(subscribed.error());
    }
    return domain::ok(domain::Channel{
        .id = domain::ChannelId{id.value()},
        .name = name.value(),
        .parent_id = parent.value(),
        .client_count = static_cast<std::size_t>(client_count.value()),
        .is_default = is_default.value(),
        .subscribed = subscribed.value(),
    });
}

auto decode_channels(const std::vector<Fields>& lines) -> domain::Result<std::vector<domain::Channel>> {
    std::vector<domain::Channel> channels;
    channels.reserve(lines.size());
    for (const auto& line : lines) {
        auto decoded = decode_channel(line);
        if (!decoded) {
            return domain::fail<std::vector<domain::Channel>>(decoded.error());
        }
        channels.push_back(decoded.value());
    }
    return domain::ok(std::move(channels));
}

auto encode(const domain::Client& client) -> Fields {
    return Fields{
        "client",
        std::to_string(client.id.value),
        hex_encode(client.nickname),
        hex_encode(client.unique_identity),
        client.channel_id.has_value() ? std::to_string(client.channel_id->value) : "0",
        client.self ? "1" : "0",
        client.talking ? "1" : "0",
    };
}

auto encode_clients(const std::vector<domain::Client>& clients) -> std::vector<Fields> {
    std::vector<Fields> lines;
    lines.reserve(clients.size());
    for (const auto& client : clients) {
        lines.push_back(encode(client));
    }
    return lines;
}

auto decode_client(const Fields& fields) -> domain::Result<domain::Client> {
    if (fields.size() != 7 || fields[0] != "client") {
        return domain::fail<domain::Client>(protocol_error(
            "invalid_client", "invalid client payload"
        ));
    }
    auto id = parse_required_u64(fields[1], "client_id");
    auto nickname = decode_string_field(fields, 2, "nickname");
    auto unique_identity = decode_string_field(fields, 3, "unique_identity");
    auto channel_id = parse_optional_channel(fields[4]);
    auto self = parse_bool(fields[5]);
    auto talking = parse_bool(fields[6]);
    if (!id) {
        return domain::fail<domain::Client>(id.error());
    }
    if (!nickname) {
        return domain::fail<domain::Client>(nickname.error());
    }
    if (!unique_identity) {
        return domain::fail<domain::Client>(unique_identity.error());
    }
    if (!channel_id) {
        return domain::fail<domain::Client>(channel_id.error());
    }
    if (!self) {
        return domain::fail<domain::Client>(self.error());
    }
    if (!talking) {
        return domain::fail<domain::Client>(talking.error());
    }
    return domain::ok(domain::Client{
        .id = domain::ClientId{id.value()},
        .nickname = nickname.value(),
        .unique_identity = unique_identity.value(),
        .channel_id = channel_id.value(),
        .self = self.value(),
        .talking = talking.value(),
    });
}

auto decode_clients(const std::vector<Fields>& lines) -> domain::Result<std::vector<domain::Client>> {
    std::vector<domain::Client> clients;
    clients.reserve(lines.size());
    for (const auto& line : lines) {
        auto decoded = decode_client(line);
        if (!decoded) {
            return domain::fail<std::vector<domain::Client>>(decoded.error());
        }
        clients.push_back(decoded.value());
    }
    return domain::ok(std::move(clients));
}

auto encode_event(const std::optional<domain::Event>& event) -> std::vector<Fields> {
    if (!event.has_value()) {
        return {{"no_event"}};
    }
    return {encode_event_line(*event)};
}

auto decode_event(const std::vector<Fields>& lines) -> domain::Result<std::optional<domain::Event>> {
    if (lines.size() != 1) {
        return domain::fail<std::optional<domain::Event>>(protocol_error(
            "invalid_event_payload", "invalid event payload"
        ));
    }
    if (lines[0].size() == 1 && lines[0][0] == "no_event") {
        return domain::ok(std::optional<domain::Event>{});
    }
    const auto& fields = lines[0];
    if (fields.size() < 5 || fields[0] != "event") {
        return domain::fail<std::optional<domain::Event>>(protocol_error(
            "invalid_event", "invalid event payload"
        ));
    }
    auto type = decode_string_field(fields, 1, "event_type");
    auto summary = decode_string_field(fields, 2, "event_summary");
    auto timestamp = parse_required_u64(fields[3], "event_timestamp");
    auto field_count = parse_required_u64(fields[4], "field_count");
    if (!type) {
        return domain::fail<std::optional<domain::Event>>(type.error());
    }
    if (!summary) {
        return domain::fail<std::optional<domain::Event>>(summary.error());
    }
    if (!timestamp) {
        return domain::fail<std::optional<domain::Event>>(timestamp.error());
    }
    if (!field_count) {
        return domain::fail<std::optional<domain::Event>>(field_count.error());
    }
    if (fields.size() != 5 + field_count.value() * 2) {
        return domain::fail<std::optional<domain::Event>>(protocol_error(
            "invalid_event", "bridge event field count does not match payload"
        ));
    }
    std::map<std::string, std::string> event_fields;
    for (std::size_t index = 0; index < field_count.value(); ++index) {
        auto key = decode_string_field(fields, 5 + index * 2, "event_field_key");
        auto value = decode_string_field(fields, 6 + index * 2, "event_field_value");
        if (!key) {
            return domain::fail<std::optional<domain::Event>>(key.error());
        }
        if (!value) {
            return domain::fail<std::optional<domain::Event>>(value.error());
        }
        event_fields.emplace(key.value(), value.value());
    }
    return domain::ok(std::optional<domain::Event>{domain::Event{
        .type = type.value(),
        .summary = summary.value(),
        .at = std::chrono::system_clock::time_point(std::chrono::milliseconds(timestamp.value())),
        .fields = std::move(event_fields),
    }});
}

}  // namespace teamspeak_cli::bridge::protocol
