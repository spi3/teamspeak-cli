#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "teamspeak_cli/bridge/media_bridge.hpp"
#include "teamspeak_cli/bridge/protocol.hpp"
#include "teamspeak_cli/bridge/socket_server.hpp"
#include "teamspeak_cli/sdk/mock_backend.hpp"
#include "teamspeak_cli/util/strings.hpp"
#include "test_support.hpp"

namespace {

namespace fs = std::filesystem;
namespace bridge = teamspeak_cli::bridge;
namespace protocol = teamspeak_cli::bridge::protocol;

struct MediaFrame {
    protocol::Fields fields;
    std::vector<char> payload;
};

void write_all(int fd, const char* data, std::size_t size) {
    std::size_t remaining = size;
    while (remaining > 0) {
        const auto written = ::write(fd, data, remaining);
        teamspeak_cli::tests::expect(written >= 0, "media test should write to the socket");
        data += written;
        remaining -= static_cast<std::size_t>(written);
    }
}

void read_exact(int fd, char* data, std::size_t size) {
    std::size_t remaining = size;
    while (remaining > 0) {
        const auto received = ::read(fd, data, remaining);
        teamspeak_cli::tests::expect(received > 0, "media test should read the requested payload bytes");
        data += received;
        remaining -= static_cast<std::size_t>(received);
    }
}

auto connect_socket(const fs::path& path) -> int {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    teamspeak_cli::tests::expect(fd >= 0, "media test should create a unix socket");

    timeval timeout{
        .tv_sec = 2,
        .tv_usec = 0,
    };
    teamspeak_cli::tests::expect(
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0,
        "media test should configure a receive timeout"
    );
    teamspeak_cli::tests::expect(
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0,
        "media test should configure a send timeout"
    );

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", path.c_str());
    teamspeak_cli::tests::expect(
        ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
        "media test should connect to the media socket"
    );
    return fd;
}

auto decode_u64(std::string_view value, const std::string& message) -> std::uint64_t {
    const auto parsed = teamspeak_cli::util::parse_u64(value);
    teamspeak_cli::tests::expect(parsed.has_value(), message);
    return *parsed;
}

auto decode_hex(const protocol::Fields& fields, std::size_t index, const std::string& message) -> std::string {
    teamspeak_cli::tests::expect(index < fields.size(), message);
    const auto decoded = protocol::hex_decode(fields[index]);
    teamspeak_cli::tests::expect(decoded.ok(), message);
    return decoded.value();
}

auto read_frame(int fd) -> MediaFrame {
    std::string line;
    teamspeak_cli::tests::expect(protocol::read_line(fd, line), "media test should read a frame header");
    auto fields = protocol::split_fields(line);
    teamspeak_cli::tests::expect(fields.size() >= 2, "media frame should include a type");
    teamspeak_cli::tests::expect_eq(fields[0], std::string(bridge::kMediaMagic), "media frame magic should match");

    MediaFrame frame{
        .fields = std::move(fields),
        .payload = {},
    };
    if (frame.fields[1] == "audio.chunk") {
        const auto payload_bytes = decode_u64(frame.fields.back(), "audio chunk should include a payload size");
        frame.payload.resize(payload_bytes);
        read_exact(fd, frame.payload.data(), frame.payload.size());
    }
    return frame;
}

auto wait_for_frame(int fd, std::string_view type, const std::string& message) -> MediaFrame {
    for (int attempt = 0; attempt < 64; ++attempt) {
        auto frame = read_frame(fd);
        if (frame.fields[1] == "error") {
            const auto code = decode_hex(frame.fields, 3, "error frame should include an error code");
            const auto detail = decode_hex(frame.fields, 4, "error frame should include an error message");
            teamspeak_cli::tests::expect(false, "unexpected media bridge error: " + code + ": " + detail);
        }
        if (frame.fields[1] == type) {
            return frame;
        }
    }
    teamspeak_cli::tests::expect(false, message);
    return MediaFrame{};
}

void write_header(int fd, const protocol::Fields& fields) {
    teamspeak_cli::tests::expect(protocol::write_line(fd, fields), "media test should write a frame header");
}

void start_playback(int fd) {
    write_header(
        fd,
        {
            std::string(bridge::kMediaMagic),
            "playback.start",
            protocol::hex_encode(bridge::kMediaSampleFormat),
            std::to_string(bridge::kMediaSampleRate),
            std::to_string(bridge::kMediaPlaybackChannels),
        }
    );
}

void write_playback_chunk(int fd, const std::vector<short>& samples) {
    write_header(
        fd,
        {
            std::string(bridge::kMediaMagic),
            "playback.chunk",
            std::to_string(samples.size()),
            std::to_string(samples.size() * sizeof(short)),
        }
    );
    write_all(fd, reinterpret_cast<const char*>(samples.data()), samples.size() * sizeof(short));
}

auto queued_samples(const MediaFrame& frame) -> std::uint64_t {
    teamspeak_cli::tests::expect_eq(frame.fields[1], std::string("status"), "status frame type should match");
    return decode_u64(frame.fields[5], "status frame should report queued playback samples");
}

auto playback_active(const MediaFrame& frame) -> bool {
    teamspeak_cli::tests::expect_eq(frame.fields[1], std::string("status"), "status frame type should match");
    return frame.fields[4] == "1";
}

}  // namespace

int main() {
    using namespace teamspeak_cli;

    const fs::path root = tests::make_temp_path("ts-media-bridge-test");
    fs::create_directories(root);
    const fs::path control_socket_path = root / "plugin.sock";

    bridge::SocketBridgeServer server(std::make_unique<sdk::MockBackend>());
    auto started = server.start(sdk::InitOptions{.socket_path = control_socket_path.string()});
    tests::expect(started.ok(), "media bridge host should start");

    const fs::path media_socket_path = server.media_socket_path();
    tests::expect(!media_socket_path.empty(), "media bridge should expose a media socket path");
    tests::expect(fs::exists(media_socket_path), "media bridge should create the media socket");

    const int media_fd = connect_socket(media_socket_path);

    auto hello = wait_for_frame(media_fd, "hello", "media bridge should send a hello frame");
    tests::expect_eq(
        decode_hex(hello.fields, 3, "hello frame should advertise the media format"),
        bridge::media_format_description(),
        "hello frame should advertise the accepted playback format"
    );
    tests::expect_eq(
        decode_hex(hello.fields, 6, "hello frame should advertise the media socket path"),
        media_socket_path.string(),
        "hello frame should report the bound media socket path"
    );

    auto initial_status = wait_for_frame(media_fd, "status", "media bridge should send an initial status frame");
    tests::expect(!playback_active(initial_status), "initial status should report inactive playback");
    tests::expect_eq(queued_samples(initial_status), static_cast<std::uint64_t>(0), "initial queue should be empty");

    write_header(media_fd, {std::string(bridge::kMediaMagic), "status.request"});
    auto requested_status = wait_for_frame(media_fd, "status", "media bridge should answer status requests");
    tests::expect_eq(requested_status.fields[1], std::string("status"), "status.request should return status");

    auto speaker_start = wait_for_frame(media_fd, "speaker.start", "media bridge should publish speaker.start");
    tests::expect_eq(
        decode_hex(speaker_start.fields, 5, "speaker.start should include a unique identity"),
        std::string("sdk-bob"),
        "speaker.start should identify the active speaker"
    );
    tests::expect_eq(
        decode_hex(speaker_start.fields, 6, "speaker.start should include a nickname"),
        std::string("bob"),
        "speaker.start should include the speaker nickname"
    );

    auto audio_chunk = wait_for_frame(media_fd, "audio.chunk", "media bridge should publish audio chunks");
    tests::expect_eq(
        decode_u64(audio_chunk.fields[8], "audio chunk should report the sample rate"),
        static_cast<std::uint64_t>(bridge::kMediaSampleRate),
        "audio chunk sample rate should match the documented media format"
    );
    tests::expect_eq(
        decode_u64(audio_chunk.fields[9], "audio chunk should report the channel count"),
        static_cast<std::uint64_t>(1),
        "mock ingress audio should be mono"
    );
    tests::expect(!audio_chunk.payload.empty(), "audio chunk payload should not be empty");

    start_playback(media_fd);
    auto playback_started = wait_for_frame(
        media_fd,
        "playback.started",
        "media bridge should acknowledge playback.start"
    );
    tests::expect_eq(
        playback_started.fields[1],
        std::string("playback.started"),
        "playback.start should produce a playback.started frame"
    );

    std::vector<short> queued_audio(bridge::kMediaSampleRate / 2, static_cast<short>(2048));
    write_playback_chunk(media_fd, queued_audio);
    write_header(media_fd, {std::string(bridge::kMediaMagic), "status.request"});
    auto queued_status = wait_for_frame(media_fd, "status", "media bridge should report queued playback");
    tests::expect(playback_active(queued_status), "status should report active playback after playback.start");
    tests::expect(
        queued_samples(queued_status) > 0,
        "status should report queued playback samples after playback.chunk"
    );

    write_header(media_fd, {std::string(bridge::kMediaMagic), "playback.clear"});
    auto playback_cleared = wait_for_frame(
        media_fd,
        "playback.cleared",
        "media bridge should acknowledge playback.clear"
    );
    tests::expect_eq(
        playback_cleared.fields[1],
        std::string("playback.cleared"),
        "playback.clear should produce a playback.cleared frame"
    );

    write_header(media_fd, {std::string(bridge::kMediaMagic), "status.request"});
    auto cleared_status = wait_for_frame(media_fd, "status", "media bridge should report cleared playback");
    tests::expect(!playback_active(cleared_status), "status should report inactive playback after clear");
    tests::expect_eq(queued_samples(cleared_status), static_cast<std::uint64_t>(0), "clear should empty the queue");

    start_playback(media_fd);
    (void)wait_for_frame(media_fd, "playback.started", "media bridge should allow a second playback session");
    write_playback_chunk(media_fd, std::vector<short>(9600, static_cast<short>(1024)));
    write_header(media_fd, {std::string(bridge::kMediaMagic), "playback.stop"});
    auto playback_stopped = wait_for_frame(
        media_fd,
        "playback.stopped",
        "media bridge should emit playback.stopped after draining playback"
    );
    tests::expect_eq(
        decode_hex(playback_stopped.fields, 3, "playback.stopped should include a stop reason"),
        std::string("drained"),
        "playback.stop should finish once the queued samples are drained"
    );

    ::close(media_fd);

    auto stopped = server.stop();
    tests::expect(stopped.ok(), "media bridge host should stop");
    tests::expect(!fs::exists(media_socket_path), "media bridge stop should remove the media socket");
    return 0;
}
