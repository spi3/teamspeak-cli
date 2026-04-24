#include "teamspeak_cli/bridge/media_client.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "teamspeak_cli/bridge/media_bridge.hpp"
#include "teamspeak_cli/bridge/protocol.hpp"
#include "teamspeak_cli/util/strings.hpp"

namespace teamspeak_cli::bridge {
namespace {

constexpr std::size_t kPlaybackChunkFrames = static_cast<std::size_t>(kMediaSampleRate) / 100U;

struct AudioPayload {
    std::vector<char> pcm;
    std::size_t frames = 0;
};

struct MediaFrame {
    protocol::Fields fields;
    std::string type;
};

class FileDescriptor {
  public:
    explicit FileDescriptor(int fd = -1) : fd_(fd) {}
    ~FileDescriptor() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    auto operator=(const FileDescriptor&) -> FileDescriptor& = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    auto operator=(FileDescriptor&& other) noexcept -> FileDescriptor& {
        if (this != &other) {
            if (fd_ >= 0) {
                ::close(fd_);
            }
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    [[nodiscard]] auto get() const -> int { return fd_; }

  private:
    int fd_;
};

auto media_client_error(
    std::string code,
    std::string message,
    domain::ExitCode exit_code = domain::ExitCode::connection
) -> domain::Error {
    return domain::make_error("media", std::move(code), std::move(message), exit_code);
}

auto timeout_error(std::string_view stage, std::string_view socket_path) -> domain::Error {
    auto error = media_client_error(
        "socket_timeout",
        "timed out while " + std::string(stage) + " on the media socket"
    );
    error.details.emplace("stage", stage);
    error.details.emplace("socket_path", socket_path);
    return error;
}

auto socket_io_error(std::string_view stage, std::string_view socket_path, int error_number) -> domain::Error {
    if (error_number == EAGAIN || error_number == EWOULDBLOCK) {
        return timeout_error(stage, socket_path);
    }
    auto error = media_client_error(
        "socket_" + std::string(stage) + "_failed",
        "failed to " + std::string(stage) + " media socket" +
            (error_number == 0 ? std::string{} : ": " + std::string(std::strerror(error_number)))
    );
    error.details.emplace("stage", stage);
    error.details.emplace("socket_path", socket_path);
    if (error_number != 0) {
        error.details.emplace("os_error", std::string(std::strerror(error_number)));
    }
    return error;
}

auto remaining_timeout(
    const std::chrono::steady_clock::time_point& deadline,
    std::string_view stage,
    std::string_view socket_path
) -> domain::Result<std::chrono::milliseconds> {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return domain::fail<std::chrono::milliseconds>(timeout_error(stage, socket_path));
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    return domain::ok(std::max(remaining, std::chrono::milliseconds(1)));
}

auto duration_to_timeval(std::chrono::milliseconds timeout) -> timeval {
    if (timeout <= std::chrono::milliseconds::zero()) {
        timeout = std::chrono::milliseconds(1);
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(timeout - seconds);
    return timeval{
        .tv_sec = static_cast<decltype(timeval::tv_sec)>(seconds.count()),
        .tv_usec = static_cast<decltype(timeval::tv_usec)>(micros.count()),
    };
}

auto set_socket_timeout(int fd, int option_name, std::chrono::milliseconds timeout) -> domain::Result<void> {
    const auto value = duration_to_timeval(timeout);
    if (::setsockopt(fd, SOL_SOCKET, option_name, &value, sizeof(value)) != 0) {
        return domain::fail(media_client_error(
            "socket_option_failed",
            "failed to configure media socket timeout: " + std::string(std::strerror(errno)),
            domain::ExitCode::internal
        ));
    }
    return domain::ok();
}

auto configure_socket_for_stage(
    int fd,
    int option_name,
    const std::chrono::steady_clock::time_point& deadline,
    std::string_view stage,
    std::string_view socket_path
) -> domain::Result<void> {
    auto timeout = remaining_timeout(deadline, stage, socket_path);
    if (!timeout) {
        return domain::fail(timeout.error());
    }
    return set_socket_timeout(fd, option_name, timeout.value());
}

auto read_u16_le(const char* data) -> std::uint16_t {
    const auto lo = static_cast<std::uint16_t>(static_cast<unsigned char>(data[0]));
    const auto hi = static_cast<std::uint16_t>(static_cast<unsigned char>(data[1]));
    return static_cast<std::uint16_t>(lo | static_cast<std::uint16_t>(hi << 8U));
}

auto read_u32_le(const char* data) -> std::uint32_t {
    const auto b0 = static_cast<std::uint32_t>(static_cast<unsigned char>(data[0]));
    const auto b1 = static_cast<std::uint32_t>(static_cast<unsigned char>(data[1]));
    const auto b2 = static_cast<std::uint32_t>(static_cast<unsigned char>(data[2]));
    const auto b3 = static_cast<std::uint32_t>(static_cast<unsigned char>(data[3]));
    return b0 | (b1 << 8U) | (b2 << 16U) | (b3 << 24U);
}

auto read_exact_stream(std::ifstream& input, char* data, std::size_t size) -> bool {
    input.read(data, static_cast<std::streamsize>(size));
    return input.good() || (size == 0 && !input.bad());
}

auto skip_chunk_padding(std::ifstream& input, std::uint32_t chunk_size) -> bool {
    if ((chunk_size & 1U) == 0U) {
        return true;
    }
    char ignored = '\0';
    input.read(&ignored, 1);
    return input.good();
}

auto invalid_wav(std::string message) -> domain::Error {
    return media_client_error("invalid_wav", std::move(message), domain::ExitCode::usage);
}

auto unsupported_wav(std::string message) -> domain::Error {
    return media_client_error("unsupported_audio_format", std::move(message), domain::ExitCode::usage);
}

auto read_wav_payload(const std::filesystem::path& path) -> domain::Result<AudioPayload> {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return domain::fail<AudioPayload>(media_client_error(
            "file_open_failed",
            "failed to open playback file: " + path.string(),
            domain::ExitCode::not_found
        ));
    }

    std::array<char, 12> riff_header{};
    if (!read_exact_stream(input, riff_header.data(), riff_header.size()) ||
        std::memcmp(riff_header.data(), "RIFF", 4) != 0 ||
        std::memcmp(riff_header.data() + 8, "WAVE", 4) != 0) {
        return domain::fail<AudioPayload>(invalid_wav(
            "playback file must be a WAV file containing pcm_s16le @ 48000 Hz mono"
        ));
    }

    bool saw_fmt = false;
    bool saw_data = false;
    std::uint16_t audio_format = 0;
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t bits_per_sample = 0;
    AudioPayload payload;

    while (input) {
        std::array<char, 8> chunk_header{};
        input.read(chunk_header.data(), static_cast<std::streamsize>(chunk_header.size()));
        if (input.eof() && input.gcount() == 0) {
            break;
        }
        if (input.gcount() != static_cast<std::streamsize>(chunk_header.size())) {
            return domain::fail<AudioPayload>(invalid_wav("WAV file ended inside a chunk header"));
        }

        const std::string_view chunk_id(chunk_header.data(), 4);
        const auto chunk_size = read_u32_le(chunk_header.data() + 4);

        if (chunk_id == "fmt ") {
            std::vector<char> data(static_cast<std::size_t>(chunk_size));
            if (!read_exact_stream(input, data.data(), data.size())) {
                return domain::fail<AudioPayload>(invalid_wav("WAV file ended inside the fmt chunk"));
            }
            if (data.size() < 16U) {
                return domain::fail<AudioPayload>(invalid_wav("WAV fmt chunk is too short"));
            }
            audio_format = read_u16_le(data.data());
            channels = read_u16_le(data.data() + 2);
            sample_rate = read_u32_le(data.data() + 4);
            bits_per_sample = read_u16_le(data.data() + 14);
            saw_fmt = true;
            if (!skip_chunk_padding(input, chunk_size)) {
                return domain::fail<AudioPayload>(invalid_wav("WAV file ended after the fmt chunk"));
            }
            continue;
        }

        if (chunk_id == "data") {
            payload.pcm.resize(static_cast<std::size_t>(chunk_size));
            if (!read_exact_stream(input, payload.pcm.data(), payload.pcm.size())) {
                return domain::fail<AudioPayload>(invalid_wav("WAV file ended inside the data chunk"));
            }
            saw_data = true;
            if (!skip_chunk_padding(input, chunk_size)) {
                return domain::fail<AudioPayload>(invalid_wav("WAV file ended after the data chunk"));
            }
            continue;
        }

        input.seekg(static_cast<std::streamoff>(chunk_size), std::ios::cur);
        if (!input) {
            return domain::fail<AudioPayload>(invalid_wav("WAV file ended inside an unknown chunk"));
        }
        if (!skip_chunk_padding(input, chunk_size)) {
            return domain::fail<AudioPayload>(invalid_wav("WAV file ended after an unknown chunk"));
        }
    }

    if (!saw_fmt) {
        return domain::fail<AudioPayload>(invalid_wav("WAV file is missing a fmt chunk"));
    }
    if (!saw_data || payload.pcm.empty()) {
        return domain::fail<AudioPayload>(invalid_wav("WAV file is missing audio samples"));
    }
    if (audio_format != 1U || channels != static_cast<std::uint16_t>(kMediaPlaybackChannels) ||
        sample_rate != static_cast<std::uint32_t>(kMediaSampleRate) || bits_per_sample != 16U) {
        return domain::fail<AudioPayload>(unsupported_wav(
            "playback WAV must be pcm_s16le @ 48000 Hz mono"
        ));
    }
    if ((payload.pcm.size() % sizeof(short)) != 0U) {
        return domain::fail<AudioPayload>(invalid_wav(
            "WAV data chunk is not aligned to 16-bit samples"
        ));
    }

    payload.frames = payload.pcm.size() / sizeof(short);
    return domain::ok(std::move(payload));
}

auto connect_media_socket(
    const std::string& socket_path,
    const std::chrono::steady_clock::time_point& deadline
) -> domain::Result<FileDescriptor> {
    FileDescriptor fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (fd.get() < 0) {
        return domain::fail<FileDescriptor>(media_client_error(
            "socket_failed", "failed to create media socket: " + std::string(std::strerror(errno))
        ));
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(address.sun_path)) {
        return domain::fail<FileDescriptor>(media_client_error(
            "socket_path_too_long", "media socket path exceeds platform limit", domain::ExitCode::config
        ));
    }
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path.c_str());

    auto timeout = configure_socket_for_stage(fd.get(), SO_SNDTIMEO, deadline, "connect", socket_path);
    if (!timeout) {
        return domain::fail<FileDescriptor>(timeout.error());
    }
    if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        auto error = media_client_error(
            "socket_connect_failed",
            "failed to connect to media socket: " + std::string(std::strerror(errno))
        );
        error.details.emplace("socket_path", socket_path);
        error.details.emplace("os_error", std::string(std::strerror(errno)));
        return domain::fail<FileDescriptor>(std::move(error));
    }
    return domain::ok(std::move(fd));
}

auto write_all(
    int fd,
    const char* data,
    std::size_t size,
    const std::string& socket_path,
    const std::chrono::steady_clock::time_point& deadline
) -> domain::Result<void> {
    auto configured = configure_socket_for_stage(fd, SO_SNDTIMEO, deadline, "write", socket_path);
    if (!configured) {
        return configured;
    }

    std::size_t remaining = size;
    while (remaining > 0) {
        errno = 0;
        const auto written = ::write(fd, data, remaining);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return domain::fail(socket_io_error("write", socket_path, errno));
        }
        const auto written_size = static_cast<std::size_t>(written);
        data += written_size;
        remaining -= written_size;
    }
    return domain::ok();
}

auto write_header(
    int fd,
    const protocol::Fields& fields,
    const std::string& socket_path,
    const std::chrono::steady_clock::time_point& deadline
) -> domain::Result<void> {
    const std::string line = protocol::join_fields(fields);
    return write_all(fd, line.data(), line.size(), socket_path, deadline);
}

auto read_exact_socket(
    int fd,
    char* data,
    std::size_t size,
    const std::string& socket_path,
    const std::chrono::steady_clock::time_point& deadline
) -> domain::Result<void> {
    auto configured = configure_socket_for_stage(fd, SO_RCVTIMEO, deadline, "read", socket_path);
    if (!configured) {
        return configured;
    }

    std::size_t remaining = size;
    while (remaining > 0) {
        errno = 0;
        const auto received = ::read(fd, data, remaining);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            return domain::fail(socket_io_error("read", socket_path, errno));
        }
        const auto received_size = static_cast<std::size_t>(received);
        data += received_size;
        remaining -= received_size;
    }
    return domain::ok();
}

auto discard_payload(
    int fd,
    std::size_t payload_bytes,
    const std::string& socket_path,
    const std::chrono::steady_clock::time_point& deadline
) -> domain::Result<void> {
    std::array<char, 4096> buffer{};
    while (payload_bytes > 0) {
        const auto next = std::min(payload_bytes, buffer.size());
        auto read = read_exact_socket(fd, buffer.data(), next, socket_path, deadline);
        if (!read) {
            return read;
        }
        payload_bytes -= next;
    }
    return domain::ok();
}

auto decode_media_field(const protocol::Fields& fields, std::size_t index, std::string_view fallback)
    -> std::string {
    if (index >= fields.size()) {
        return std::string(fallback);
    }
    auto decoded = protocol::hex_decode(fields[index]);
    if (!decoded) {
        return std::string(fallback);
    }
    return decoded.value();
}

auto media_bridge_frame_error(const protocol::Fields& fields) -> domain::Error {
    const auto code = decode_media_field(fields, 3, "error");
    const auto message = decode_media_field(fields, 4, "media bridge returned an error");
    return media_client_error(code, message);
}

auto parse_payload_size(const protocol::Fields& fields, std::size_t index) -> domain::Result<std::size_t> {
    if (index >= fields.size()) {
        return domain::fail<std::size_t>(media_client_error(
            "invalid_frame", "media bridge frame is missing a payload size", domain::ExitCode::internal
        ));
    }
    const auto parsed = util::parse_u64(fields[index]);
    if (!parsed.has_value() || *parsed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return domain::fail<std::size_t>(media_client_error(
            "invalid_frame", "media bridge frame has an invalid payload size", domain::ExitCode::internal
        ));
    }
    return domain::ok(static_cast<std::size_t>(*parsed));
}

auto read_media_frame(
    int fd,
    const std::string& socket_path,
    const std::chrono::steady_clock::time_point& deadline
) -> domain::Result<MediaFrame> {
    auto configured = configure_socket_for_stage(fd, SO_RCVTIMEO, deadline, "read", socket_path);
    if (!configured) {
        return domain::fail<MediaFrame>(configured.error());
    }

    std::string line;
    errno = 0;
    if (!protocol::read_line(fd, line)) {
        return domain::fail<MediaFrame>(socket_io_error("read", socket_path, errno));
    }
    auto fields = protocol::split_fields(line);
    if (fields.size() < 2 || fields[0] != kMediaMagic) {
        return domain::fail<MediaFrame>(media_client_error(
            "invalid_frame", "invalid media bridge frame", domain::ExitCode::internal
        ));
    }

    if (fields[1] == "audio.chunk") {
        auto payload_bytes = parse_payload_size(fields, fields.size() - 1U);
        if (!payload_bytes) {
            return domain::fail<MediaFrame>(payload_bytes.error());
        }
        auto discarded = discard_payload(fd, payload_bytes.value(), socket_path, deadline);
        if (!discarded) {
            return domain::fail<MediaFrame>(discarded.error());
        }
    }

    if (fields[1] == "error") {
        return domain::fail<MediaFrame>(media_bridge_frame_error(fields));
    }

    std::string type = fields[1];
    return domain::ok(MediaFrame{.fields = std::move(fields), .type = std::move(type)});
}

auto wait_for_frame(
    int fd,
    std::string_view expected_type,
    const std::string& socket_path,
    const std::chrono::steady_clock::time_point& deadline
) -> domain::Result<MediaFrame> {
    while (true) {
        auto frame = read_media_frame(fd, socket_path, deadline);
        if (!frame) {
            return domain::fail<MediaFrame>(frame.error());
        }
        if (frame.value().type == expected_type) {
            return frame;
        }
    }
}

auto drain_ready_frames(
    int fd,
    const std::string& socket_path,
    const std::chrono::steady_clock::time_point& deadline
) -> domain::Result<void> {
    while (true) {
        pollfd state{
            .fd = fd,
            .events = POLLIN,
            .revents = 0,
        };
        const int ready = ::poll(&state, 1, 0);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready < 0) {
            return domain::fail(media_client_error(
                "socket_poll_failed",
                "failed to poll media socket: " + std::string(std::strerror(errno)),
                domain::ExitCode::internal
            ));
        }
        if (ready == 0) {
            return domain::ok();
        }
        if ((state.revents & POLLIN) == 0) {
            return domain::fail(media_client_error(
                "socket_read_failed", "media socket closed while playback was active"
            ));
        }
        auto frame = read_media_frame(fd, socket_path, deadline);
        if (!frame) {
            return domain::fail(frame.error());
        }
    }
}

auto send_playback_start(
    int fd,
    const std::string& socket_path,
    const std::chrono::steady_clock::time_point& deadline
) -> domain::Result<void> {
    auto written = write_header(
        fd,
        {
            std::string(kMediaMagic),
            "playback.start",
            protocol::hex_encode(kMediaSampleFormat),
            std::to_string(kMediaSampleRate),
            std::to_string(kMediaPlaybackChannels),
        },
        socket_path,
        deadline
    );
    if (!written) {
        return written;
    }
    auto started = wait_for_frame(fd, "playback.started", socket_path, deadline);
    if (!started) {
        return domain::fail(started.error());
    }
    return domain::ok();
}

auto send_playback_clear(
    int fd,
    const std::string& socket_path,
    const std::chrono::steady_clock::time_point& deadline
) -> domain::Result<void> {
    auto written = write_header(fd, {std::string(kMediaMagic), "playback.clear"}, socket_path, deadline);
    if (!written) {
        return written;
    }
    auto cleared = wait_for_frame(fd, "playback.cleared", socket_path, deadline);
    if (!cleared) {
        return domain::fail(cleared.error());
    }
    return domain::ok();
}

auto send_playback_chunk(
    int fd,
    const char* data,
    std::size_t bytes,
    const std::string& socket_path,
    const std::chrono::steady_clock::time_point& deadline
) -> domain::Result<void> {
    const auto frames = bytes / sizeof(short);
    auto header = write_header(
        fd,
        {
            std::string(kMediaMagic),
            "playback.chunk",
            std::to_string(frames),
            std::to_string(bytes),
        },
        socket_path,
        deadline
    );
    if (!header) {
        return header;
    }
    return write_all(fd, data, bytes, socket_path, deadline);
}

auto send_playback_stop(
    int fd,
    const std::string& socket_path,
    const std::chrono::steady_clock::time_point& deadline
) -> domain::Result<std::string> {
    auto written = write_header(fd, {std::string(kMediaMagic), "playback.stop"}, socket_path, deadline);
    if (!written) {
        return domain::fail<std::string>(written.error());
    }
    auto stopped = wait_for_frame(fd, "playback.stopped", socket_path, deadline);
    if (!stopped) {
        return domain::fail<std::string>(stopped.error());
    }
    return domain::ok(decode_media_field(stopped.value().fields, 3, "unknown"));
}

auto chunk_duration(std::size_t frames) -> std::chrono::milliseconds {
    const std::size_t ms =
        std::max<std::size_t>(1U, (frames * 1000U) / static_cast<std::size_t>(kMediaSampleRate));
    return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(ms));
}

}  // namespace

auto send_playback_file(const PlaybackSendRequest& request) -> domain::Result<PlaybackSendResult> {
    if (request.socket_path.empty()) {
        return domain::fail<PlaybackSendResult>(media_client_error(
            "missing_media_socket", "plugin info did not report a media socket path"
        ));
    }
    if (request.timeout <= std::chrono::milliseconds::zero()) {
        return domain::fail<PlaybackSendResult>(media_client_error(
            "invalid_timeout", "--timeout-ms must be greater than 0", domain::ExitCode::usage
        ));
    }

    auto audio = read_wav_payload(request.file_path);
    if (!audio) {
        return domain::fail<PlaybackSendResult>(audio.error());
    }

    const auto deadline = std::chrono::steady_clock::now() + request.timeout;
    auto fd = connect_media_socket(request.socket_path, deadline);
    if (!fd) {
        return domain::fail<PlaybackSendResult>(fd.error());
    }

    if (request.clear_first) {
        auto cleared = send_playback_clear(fd.value().get(), request.socket_path, deadline);
        if (!cleared) {
            return domain::fail<PlaybackSendResult>(cleared.error());
        }
    }

    auto started = send_playback_start(fd.value().get(), request.socket_path, deadline);
    if (!started) {
        return domain::fail<PlaybackSendResult>(started.error());
    }

    std::size_t offset = 0;
    while (offset < audio.value().pcm.size()) {
        const auto remaining = audio.value().pcm.size() - offset;
        const auto chunk_bytes = std::min(remaining, kPlaybackChunkFrames * sizeof(short));
        auto chunk = send_playback_chunk(
            fd.value().get(),
            audio.value().pcm.data() + offset,
            chunk_bytes,
            request.socket_path,
            deadline
        );
        if (!chunk) {
            return domain::fail<PlaybackSendResult>(chunk.error());
        }

        auto drained = drain_ready_frames(fd.value().get(), request.socket_path, deadline);
        if (!drained) {
            return domain::fail<PlaybackSendResult>(drained.error());
        }

        offset += chunk_bytes;
        if (offset < audio.value().pcm.size()) {
            std::this_thread::sleep_for(chunk_duration(chunk_bytes / sizeof(short)));
        }
    }

    auto stop_reason = send_playback_stop(fd.value().get(), request.socket_path, deadline);
    if (!stop_reason) {
        return domain::fail<PlaybackSendResult>(stop_reason.error());
    }

    return domain::ok(PlaybackSendResult{
        .file_path = request.file_path,
        .socket_path = request.socket_path,
        .media_format = media_format_description(),
        .frames_sent = audio.value().frames,
        .bytes_sent = audio.value().pcm.size(),
        .cleared_first = request.clear_first,
        .stop_reason = stop_reason.value(),
    });
}

}  // namespace teamspeak_cli::bridge
