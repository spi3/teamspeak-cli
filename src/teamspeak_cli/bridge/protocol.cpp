#include "teamspeak_cli/bridge/protocol.hpp"

#include <chrono>
#include <cstring>
#include <limits>
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

auto parse_required_size(const std::string& value, std::string_view field_name) -> domain::Result<std::size_t> {
    auto parsed = parse_required_u64(value, field_name);
    if (!parsed) {
        return domain::fail<std::size_t>(parsed.error());
    }
    if (parsed.value() > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return domain::fail<std::size_t>(protocol_error(
            "invalid_number", "bridge integer is too large for " + std::string(field_name)
        ));
    }
    return domain::ok(static_cast<std::size_t>(parsed.value()));
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

auto encode_bool(bool value) -> std::string {
    return value ? "1" : "0";
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
    for (const char ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        out.push_back(digits[(byte >> 4U) & 0x0FU]);
        out.push_back(digits[byte & 0x0FU]);
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
        hex_encode(info.media_transport),
        hex_encode(info.media_socket_path),
        hex_encode(info.media_format),
        hex_encode(info.note),
        encode_bool(info.media_diagnostics.capture.known),
        hex_encode(info.media_diagnostics.capture.mode),
        hex_encode(info.media_diagnostics.capture.device),
        encode_bool(info.media_diagnostics.capture.is_default),
        encode_bool(info.media_diagnostics.playback.known),
        hex_encode(info.media_diagnostics.playback.mode),
        hex_encode(info.media_diagnostics.playback.device),
        encode_bool(info.media_diagnostics.playback.is_default),
        hex_encode(info.media_diagnostics.pulse_sink),
        hex_encode(info.media_diagnostics.pulse_source),
        encode_bool(info.media_diagnostics.pulse_source_is_monitor),
        encode_bool(info.media_diagnostics.consumer_connected),
        encode_bool(info.media_diagnostics.playback_active),
        std::to_string(info.media_diagnostics.queued_playback_samples),
        std::to_string(info.media_diagnostics.active_speaker_count),
        std::to_string(info.media_diagnostics.dropped_audio_chunks),
        std::to_string(info.media_diagnostics.dropped_playback_chunks),
        hex_encode(info.media_diagnostics.last_error),
        encode_bool(info.media_diagnostics.custom_capture_device_registered),
        hex_encode(info.media_diagnostics.custom_capture_device_id),
        hex_encode(info.media_diagnostics.custom_capture_device_name),
        encode_bool(info.media_diagnostics.custom_capture_path_available),
        encode_bool(info.media_diagnostics.injected_playback_attached_to_capture),
        encode_bool(info.media_diagnostics.captured_voice_edit_attached),
        encode_bool(info.media_diagnostics.transmit_path_ready),
        hex_encode(info.media_diagnostics.transmit_path),
    }};
}

auto decode_plugin_info(const std::vector<Fields>& lines) -> domain::Result<domain::PluginInfo> {
    if (lines.size() != 1 || (lines[0].size() != 11 && lines[0].size() != 37) ||
        lines[0][0] != "plugin_info") {
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
    auto media_transport = decode_string_field(lines[0], 7, "media_transport");
    auto media_socket_path = decode_string_field(lines[0], 8, "media_socket_path");
    auto media_format = decode_string_field(lines[0], 9, "media_format");
    auto note = decode_string_field(lines[0], 10, "note");
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
    if (!media_transport) {
        return domain::fail<domain::PluginInfo>(media_transport.error());
    }
    if (!media_socket_path) {
        return domain::fail<domain::PluginInfo>(media_socket_path.error());
    }
    if (!media_format) {
        return domain::fail<domain::PluginInfo>(media_format.error());
    }
    if (!note) {
        return domain::fail<domain::PluginInfo>(note.error());
    }

    domain::MediaDiagnostics diagnostics;
    if (lines[0].size() == 37) {
        auto capture_known = parse_bool(lines[0][11]);
        auto capture_mode = decode_string_field(lines[0], 12, "capture_mode");
        auto capture_device = decode_string_field(lines[0], 13, "capture_device");
        auto capture_default = parse_bool(lines[0][14]);
        auto playback_known = parse_bool(lines[0][15]);
        auto playback_mode = decode_string_field(lines[0], 16, "playback_mode");
        auto playback_device = decode_string_field(lines[0], 17, "playback_device");
        auto playback_default = parse_bool(lines[0][18]);
        auto pulse_sink = decode_string_field(lines[0], 19, "pulse_sink");
        auto pulse_source = decode_string_field(lines[0], 20, "pulse_source");
        auto pulse_source_monitor = parse_bool(lines[0][21]);
        auto consumer_connected = parse_bool(lines[0][22]);
        auto playback_active = parse_bool(lines[0][23]);
        auto queued_playback_samples = parse_required_size(lines[0][24], "queued_playback_samples");
        auto active_speaker_count = parse_required_size(lines[0][25], "active_speaker_count");
        auto dropped_audio_chunks = parse_required_size(lines[0][26], "dropped_audio_chunks");
        auto dropped_playback_chunks = parse_required_size(lines[0][27], "dropped_playback_chunks");
        auto last_error = decode_string_field(lines[0], 28, "last_error");
        auto custom_capture_registered = parse_bool(lines[0][29]);
        auto custom_capture_device_id = decode_string_field(lines[0], 30, "custom_capture_device_id");
        auto custom_capture_device_name = decode_string_field(lines[0], 31, "custom_capture_device_name");
        auto custom_capture_path_available = parse_bool(lines[0][32]);
        auto injected_attached = parse_bool(lines[0][33]);
        auto captured_voice_edit_attached = parse_bool(lines[0][34]);
        auto transmit_path_ready = parse_bool(lines[0][35]);
        auto transmit_path = decode_string_field(lines[0], 36, "transmit_path");
        if (!capture_known) {
            return domain::fail<domain::PluginInfo>(capture_known.error());
        }
        if (!capture_mode) {
            return domain::fail<domain::PluginInfo>(capture_mode.error());
        }
        if (!capture_device) {
            return domain::fail<domain::PluginInfo>(capture_device.error());
        }
        if (!capture_default) {
            return domain::fail<domain::PluginInfo>(capture_default.error());
        }
        if (!playback_known) {
            return domain::fail<domain::PluginInfo>(playback_known.error());
        }
        if (!playback_mode) {
            return domain::fail<domain::PluginInfo>(playback_mode.error());
        }
        if (!playback_device) {
            return domain::fail<domain::PluginInfo>(playback_device.error());
        }
        if (!playback_default) {
            return domain::fail<domain::PluginInfo>(playback_default.error());
        }
        if (!pulse_sink) {
            return domain::fail<domain::PluginInfo>(pulse_sink.error());
        }
        if (!pulse_source) {
            return domain::fail<domain::PluginInfo>(pulse_source.error());
        }
        if (!pulse_source_monitor) {
            return domain::fail<domain::PluginInfo>(pulse_source_monitor.error());
        }
        if (!consumer_connected) {
            return domain::fail<domain::PluginInfo>(consumer_connected.error());
        }
        if (!playback_active) {
            return domain::fail<domain::PluginInfo>(playback_active.error());
        }
        if (!queued_playback_samples) {
            return domain::fail<domain::PluginInfo>(queued_playback_samples.error());
        }
        if (!active_speaker_count) {
            return domain::fail<domain::PluginInfo>(active_speaker_count.error());
        }
        if (!dropped_audio_chunks) {
            return domain::fail<domain::PluginInfo>(dropped_audio_chunks.error());
        }
        if (!dropped_playback_chunks) {
            return domain::fail<domain::PluginInfo>(dropped_playback_chunks.error());
        }
        if (!last_error) {
            return domain::fail<domain::PluginInfo>(last_error.error());
        }
        if (!custom_capture_registered) {
            return domain::fail<domain::PluginInfo>(custom_capture_registered.error());
        }
        if (!custom_capture_device_id) {
            return domain::fail<domain::PluginInfo>(custom_capture_device_id.error());
        }
        if (!custom_capture_device_name) {
            return domain::fail<domain::PluginInfo>(custom_capture_device_name.error());
        }
        if (!custom_capture_path_available) {
            return domain::fail<domain::PluginInfo>(custom_capture_path_available.error());
        }
        if (!injected_attached) {
            return domain::fail<domain::PluginInfo>(injected_attached.error());
        }
        if (!captured_voice_edit_attached) {
            return domain::fail<domain::PluginInfo>(captured_voice_edit_attached.error());
        }
        if (!transmit_path_ready) {
            return domain::fail<domain::PluginInfo>(transmit_path_ready.error());
        }
        if (!transmit_path) {
            return domain::fail<domain::PluginInfo>(transmit_path.error());
        }
        diagnostics = domain::MediaDiagnostics{
            .capture = domain::AudioDeviceBinding{
                .known = capture_known.value(),
                .mode = capture_mode.value(),
                .device = capture_device.value(),
                .is_default = capture_default.value(),
            },
            .playback = domain::AudioDeviceBinding{
                .known = playback_known.value(),
                .mode = playback_mode.value(),
                .device = playback_device.value(),
                .is_default = playback_default.value(),
            },
            .pulse_sink = pulse_sink.value(),
            .pulse_source = pulse_source.value(),
            .pulse_source_is_monitor = pulse_source_monitor.value(),
            .consumer_connected = consumer_connected.value(),
            .playback_active = playback_active.value(),
            .queued_playback_samples = queued_playback_samples.value(),
            .active_speaker_count = active_speaker_count.value(),
            .dropped_audio_chunks = dropped_audio_chunks.value(),
            .dropped_playback_chunks = dropped_playback_chunks.value(),
            .last_error = last_error.value(),
            .custom_capture_device_registered = custom_capture_registered.value(),
            .custom_capture_device_id = custom_capture_device_id.value(),
            .custom_capture_device_name = custom_capture_device_name.value(),
            .custom_capture_path_available = custom_capture_path_available.value(),
            .injected_playback_attached_to_capture = injected_attached.value(),
            .captured_voice_edit_attached = captured_voice_edit_attached.value(),
            .transmit_path_ready = transmit_path_ready.value(),
            .transmit_path = transmit_path.value(),
        };
    }

    return domain::ok(domain::PluginInfo{
        .backend = backend.value(),
        .transport = transport.value(),
        .plugin_name = plugin_name.value(),
        .plugin_version = plugin_version.value(),
        .plugin_available = available.value(),
        .socket_path = socket_path.value(),
        .media_transport = media_transport.value(),
        .media_socket_path = media_socket_path.value(),
        .media_format = media_format.value(),
        .media_diagnostics = std::move(diagnostics),
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

auto encode(const domain::ServerGroupApplication& application) -> std::vector<Fields> {
    std::vector<Fields> lines{{
        "server_group_application",
        std::to_string(application.server_group.id.value),
        hex_encode(application.server_group.name),
        std::to_string(application.client_database_id.value),
        application.client.has_value() ? "1" : "0",
    }};
    if (application.client.has_value()) {
        lines.push_back(encode(*application.client));
    }
    return lines;
}

auto decode_server_group_application(const std::vector<Fields>& lines)
    -> domain::Result<domain::ServerGroupApplication> {
    if (lines.empty() || lines.size() > 2 || lines[0].size() != 5 ||
        lines[0][0] != "server_group_application") {
        return domain::fail<domain::ServerGroupApplication>(protocol_error(
            "invalid_server_group_application", "invalid server group application payload"
        ));
    }

    auto group_id = parse_required_u64(lines[0][1], "server_group_id");
    auto group_name = decode_string_field(lines[0], 2, "server_group_name");
    auto client_database_id = parse_required_u64(lines[0][3], "client_database_id");
    auto client_present = parse_bool(lines[0][4]);
    if (!group_id) {
        return domain::fail<domain::ServerGroupApplication>(group_id.error());
    }
    if (!group_name) {
        return domain::fail<domain::ServerGroupApplication>(group_name.error());
    }
    if (!client_database_id) {
        return domain::fail<domain::ServerGroupApplication>(client_database_id.error());
    }
    if (!client_present) {
        return domain::fail<domain::ServerGroupApplication>(client_present.error());
    }
    if ((client_present.value() && lines.size() != 2) || (!client_present.value() && lines.size() != 1)) {
        return domain::fail<domain::ServerGroupApplication>(protocol_error(
            "invalid_server_group_application", "server group application client payload mismatch"
        ));
    }

    std::optional<domain::Client> client;
    if (client_present.value()) {
        auto decoded_client = decode_client(lines[1]);
        if (!decoded_client) {
            return domain::fail<domain::ServerGroupApplication>(decoded_client.error());
        }
        client = decoded_client.value();
    }

    return domain::ok(domain::ServerGroupApplication{
        .server_group =
            domain::ServerGroup{.id = domain::ServerGroupId{group_id.value()}, .name = group_name.value()},
        .client = std::move(client),
        .client_database_id = domain::ClientDatabaseId{client_database_id.value()},
    });
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
