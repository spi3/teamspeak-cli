#include "teamspeak_cli/bridge/media_bridge.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "teamspeak_cli/bridge/protocol.hpp"
#include "teamspeak_cli/util/strings.hpp"

namespace teamspeak_cli::bridge {
namespace {

constexpr std::size_t kMaxOutboundFrames = 256;
constexpr std::size_t kMaxQueuedPlaybackSamples = static_cast<std::size_t>(kMediaSampleRate) * 30U;

struct OutboundFrame {
    std::string header;
    std::vector<char> payload;
    bool droppable = false;
};

struct MediaBridgeState {
    mutable std::mutex mutex;
    std::condition_variable outbound_cv;
    std::string socket_path;
    int listen_fd = -1;
    int client_fd = -1;
    bool running = false;
    bool client_connected = false;
    bool playback_active = false;
    bool playback_stop_requested = false;
    std::size_t dropped_audio_chunks = 0;
    std::size_t dropped_playback_chunks = 0;
    std::string last_error;
    std::deque<short> playback_samples;
    std::deque<OutboundFrame> outbound_frames;
    std::map<std::string, MediaSpeaker> active_speakers;
    MediaPlaybackControl* playback_control = nullptr;
};

auto now_ms() -> std::string {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch()
                          )
                              .count());
}

auto media_error(std::string code, std::string message, domain::ExitCode exit_code = domain::ExitCode::connection)
    -> domain::Error {
    return domain::make_error("media", std::move(code), std::move(message), exit_code);
}

auto speaker_key(const MediaSpeaker& speaker) -> std::string {
    return std::to_string(speaker.handler_id) + ":" + std::to_string(speaker.client_id.value);
}

auto bool_field(bool value) -> std::string {
    return value ? "1" : "0";
}

auto channel_field(const std::optional<domain::ChannelId>& channel_id) -> std::string {
    return channel_id.has_value() ? domain::to_string(*channel_id) : "0";
}

void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

void shutdown_fd(int fd) {
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
    }
}

auto unlink_socket_file(const std::string& path) -> void {
    if (path.empty()) {
        return;
    }

    struct stat file_stat {};
    if (::lstat(path.c_str(), &file_stat) != 0) {
        return;
    }

    if (S_ISSOCK(file_stat.st_mode)) {
        ::unlink(path.c_str());
    }
}

auto read_exact(int fd, char* data, std::size_t size) -> bool {
    std::size_t remaining = size;
    while (remaining > 0) {
        const auto received = ::read(fd, data, remaining);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            return false;
        }
        data += received;
        remaining -= static_cast<std::size_t>(received);
    }
    return true;
}

auto write_all(int fd, const char* data, std::size_t size) -> bool {
    std::size_t remaining = size;
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

auto write_frame(int fd, const OutboundFrame& frame) -> bool {
    if (!write_all(fd, frame.header.data(), frame.header.size())) {
        return false;
    }
    if (!frame.payload.empty() && !write_all(fd, frame.payload.data(), frame.payload.size())) {
        return false;
    }
    return true;
}

auto parse_u64_field(const protocol::Fields& fields, std::size_t index, std::string_view name)
    -> domain::Result<std::uint64_t> {
    if (index >= fields.size()) {
        return domain::fail<std::uint64_t>(media_error(
            "missing_field", "missing media bridge field: " + std::string(name), domain::ExitCode::usage
        ));
    }
    const auto parsed = util::parse_u64(fields[index]);
    if (!parsed.has_value()) {
        return domain::fail<std::uint64_t>(media_error(
            "invalid_field", "invalid media bridge field: " + std::string(name), domain::ExitCode::usage
        ));
    }
    return domain::ok(*parsed);
}

auto parse_string_field(const protocol::Fields& fields, std::size_t index, std::string_view name)
    -> domain::Result<std::string> {
    if (index >= fields.size()) {
        return domain::fail<std::string>(media_error(
            "missing_field", "missing media bridge field: " + std::string(name), domain::ExitCode::usage
        ));
    }
    return protocol::hex_decode(fields[index]);
}

auto base_frame(std::string_view type) -> protocol::Fields {
    return {std::string(kMediaMagic), std::string(type), now_ms()};
}

auto error_frame(std::string_view code, std::string_view message) -> OutboundFrame {
    auto fields = base_frame("error");
    fields.push_back(protocol::hex_encode(code));
    fields.push_back(protocol::hex_encode(message));
    return OutboundFrame{protocol::join_fields(fields), {}, false};
}

auto speaker_frame(std::string_view type, const MediaSpeaker& speaker) -> OutboundFrame {
    auto fields = base_frame(type);
    fields.push_back(std::to_string(speaker.handler_id));
    fields.push_back(domain::to_string(speaker.client_id));
    fields.push_back(protocol::hex_encode(speaker.unique_identity));
    fields.push_back(protocol::hex_encode(speaker.nickname));
    fields.push_back(channel_field(speaker.channel_id));
    return OutboundFrame{protocol::join_fields(fields), {}, false};
}

auto status_frame(const MediaBridgeState& state) -> OutboundFrame {
    auto fields = base_frame("status");
    fields.push_back(bool_field(state.client_connected));
    fields.push_back(bool_field(state.playback_active));
    fields.push_back(std::to_string(state.playback_samples.size()));
    fields.push_back(std::to_string(state.active_speakers.size()));
    fields.push_back(std::to_string(state.dropped_audio_chunks));
    fields.push_back(std::to_string(state.dropped_playback_chunks));
    fields.push_back(protocol::hex_encode(state.last_error));
    return OutboundFrame{protocol::join_fields(fields), {}, false};
}

auto hello_frame(const MediaBridgeState& state) -> OutboundFrame {
    auto fields = base_frame("hello");
    fields.push_back(protocol::hex_encode(media_format_description()));
    fields.push_back(std::to_string(kMediaSampleRate));
    fields.push_back(std::to_string(kMediaPlaybackChannels));
    fields.push_back(protocol::hex_encode(state.socket_path));
    fields.push_back("1");
    return OutboundFrame{protocol::join_fields(fields), {}, false};
}

auto playback_started_frame() -> OutboundFrame {
    return OutboundFrame{protocol::join_fields(base_frame("playback.started")), {}, false};
}

auto playback_stopped_frame(std::string_view reason) -> OutboundFrame {
    auto fields = base_frame("playback.stopped");
    fields.push_back(protocol::hex_encode(reason));
    return OutboundFrame{protocol::join_fields(fields), {}, false};
}

auto playback_cleared_frame() -> OutboundFrame {
    return OutboundFrame{protocol::join_fields(base_frame("playback.cleared")), {}, false};
}

void enqueue_frame_locked(MediaBridgeState& state, OutboundFrame&& frame) {
    if (frame.droppable && state.outbound_frames.size() >= kMaxOutboundFrames) {
        ++state.dropped_audio_chunks;
        return;
    }
    state.outbound_frames.push_back(std::move(frame));
    state.outbound_cv.notify_one();
}

}  // namespace
class MediaBridgeServer::Impl {
  public:
    MediaBridgeState state;
    std::jthread accept_thread;
    std::jthread reader_thread;
    std::jthread writer_thread;
};

MediaBridgeServer::MediaBridgeServer() : impl_(std::make_unique<Impl>()) {}

MediaBridgeServer::~MediaBridgeServer() {
    const auto ignored = stop();
    (void)ignored;
}

auto MediaBridgeServer::start(std::string socket_path, MediaPlaybackControl* playback_control)
    -> domain::Result<void> {
    std::lock_guard<std::mutex> lock(impl_->state.mutex);
    if (impl_->state.running) {
        return domain::fail(media_error("already_running", "media bridge is already running"));
    }

    impl_->state.socket_path = std::move(socket_path);
    impl_->state.playback_control = playback_control;

    std::error_code ec;
    if (const auto parent = std::filesystem::path(impl_->state.socket_path).parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return domain::fail(media_error(
                "mkdir_failed", "failed to create media socket directory: " + ec.message(), domain::ExitCode::config
            ));
        }
    }

    unlink_socket_file(impl_->state.socket_path);

    const int listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        return domain::fail(media_error(
            "socket_failed", "failed to create media socket: " + std::string(std::strerror(errno))
        ));
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (impl_->state.socket_path.size() >= sizeof(address.sun_path)) {
        ::close(listen_fd);
        return domain::fail(media_error(
            "socket_path_too_long", "media socket path exceeds platform limit", domain::ExitCode::config
        ));
    }
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", impl_->state.socket_path.c_str());

    if (::bind(listen_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const auto message = std::string("failed to bind media socket: ") + std::strerror(errno);
        ::close(listen_fd);
        return domain::fail(media_error("bind_failed", message));
    }
    if (::listen(listen_fd, 2) != 0) {
        const auto message = std::string("failed to listen on media socket: ") + std::strerror(errno);
        ::close(listen_fd);
        return domain::fail(media_error("listen_failed", message));
    }

    impl_->state.listen_fd = listen_fd;
    impl_->state.running = true;
    impl_->accept_thread = std::jthread([this](std::stop_token stop_token) {
        accept_loop(stop_token);
    });
    return domain::ok();
}

auto MediaBridgeServer::stop() -> domain::Result<void> {
    MediaPlaybackControl* playback_control = nullptr;
    int listen_fd = -1;
    int client_fd = -1;
    std::string socket_path;
    {
        std::lock_guard<std::mutex> lock(impl_->state.mutex);
        if (!impl_->state.running) {
            return domain::ok();
        }
        impl_->state.running = false;
        playback_control = impl_->state.playback_control;
        listen_fd = impl_->state.listen_fd;
        client_fd = impl_->state.client_fd;
        socket_path = impl_->state.socket_path;
        impl_->state.outbound_cv.notify_all();
    }

    if (impl_->accept_thread.joinable()) {
        impl_->accept_thread.request_stop();
    }
    if (impl_->reader_thread.joinable()) {
        impl_->reader_thread.request_stop();
    }
    if (impl_->writer_thread.joinable()) {
        impl_->writer_thread.request_stop();
    }

    shutdown_fd(client_fd);
    shutdown_fd(listen_fd);

    if (impl_->accept_thread.joinable()) {
        impl_->accept_thread.join();
    }
    if (impl_->reader_thread.joinable()) {
        impl_->reader_thread.join();
    }
    if (impl_->writer_thread.joinable()) {
        impl_->writer_thread.join();
    }

    close_fd(client_fd);
    close_fd(listen_fd);
    unlink_socket_file(socket_path);

    {
        std::lock_guard<std::mutex> lock(impl_->state.mutex);
        impl_->state.listen_fd = -1;
        impl_->state.client_fd = -1;
        impl_->state.client_connected = false;
        impl_->state.playback_samples.clear();
        impl_->state.playback_active = false;
        impl_->state.playback_stop_requested = false;
        impl_->state.outbound_frames.clear();
        impl_->state.active_speakers.clear();
        impl_->state.socket_path.clear();
        impl_->state.playback_control = nullptr;
    }

    if (playback_control != nullptr) {
        const auto ignored = playback_control->deactivate_media_playback();
        (void)ignored;
    }
    return domain::ok();
}

auto MediaBridgeServer::socket_path() const -> std::string {
    std::lock_guard<std::mutex> lock(impl_->state.mutex);
    return impl_->state.socket_path;
}

auto MediaBridgeServer::status() const -> MediaStatus {
    std::lock_guard<std::mutex> lock(impl_->state.mutex);
    return MediaStatus{
        .consumer_connected = impl_->state.client_connected,
        .playback_active = impl_->state.playback_active,
        .queued_playback_samples = impl_->state.playback_samples.size(),
        .active_speaker_count = impl_->state.active_speakers.size(),
        .dropped_audio_chunks = impl_->state.dropped_audio_chunks,
        .dropped_playback_chunks = impl_->state.dropped_playback_chunks,
        .last_error = impl_->state.last_error,
    };
}

void MediaBridgeServer::publish_speaker_start(const MediaSpeaker& speaker) {
    std::lock_guard<std::mutex> lock(impl_->state.mutex);
    impl_->state.active_speakers[speaker_key(speaker)] = speaker;
    if (impl_->state.client_connected) {
        enqueue_frame_locked(impl_->state, speaker_frame("speaker.start", speaker));
    }
}

void MediaBridgeServer::publish_speaker_stop(const MediaSpeaker& speaker) {
    std::lock_guard<std::mutex> lock(impl_->state.mutex);
    impl_->state.active_speakers.erase(speaker_key(speaker));
    if (impl_->state.client_connected) {
        enqueue_frame_locked(impl_->state, speaker_frame("speaker.stop", speaker));
    }
}

void MediaBridgeServer::publish_audio_chunk(
    const MediaSpeaker& speaker,
    int sample_rate,
    int channels,
    const short* samples,
    int sample_count
) {
    {
        std::lock_guard<std::mutex> lock(impl_->state.mutex);
        if (!impl_->state.client_connected || samples == nullptr || sample_count <= 0 || channels <= 0) {
            return;
        }
    }

    const std::size_t total_samples =
        static_cast<std::size_t>(sample_count) * static_cast<std::size_t>(channels);
    std::vector<char> payload(total_samples * sizeof(short));
    std::memcpy(payload.data(), samples, payload.size());

    auto fields = base_frame("audio.chunk");
    fields.push_back(std::to_string(speaker.handler_id));
    fields.push_back(domain::to_string(speaker.client_id));
    fields.push_back(protocol::hex_encode(speaker.unique_identity));
    fields.push_back(protocol::hex_encode(speaker.nickname));
    fields.push_back(channel_field(speaker.channel_id));
    fields.push_back(std::to_string(sample_rate));
    fields.push_back(std::to_string(channels));
    fields.push_back(std::to_string(sample_count));
    fields.push_back(std::to_string(payload.size()));

    std::lock_guard<std::mutex> lock(impl_->state.mutex);
    if (!impl_->state.client_connected) {
        return;
    }
    enqueue_frame_locked(
        impl_->state,
        OutboundFrame{protocol::join_fields(fields), std::move(payload), true}
    );
}

auto MediaBridgeServer::fill_playback_samples(
    int sample_rate,
    int channels,
    short* samples,
    int sample_count
) -> bool {
    if (sample_rate != kMediaSampleRate || channels <= 0 || samples == nullptr || sample_count <= 0) {
        return false;
    }

    bool playback_active = false;
    bool should_stop = false;
    MediaPlaybackControl* playback_control = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->state.mutex);
        playback_active = impl_->state.playback_active;
        if (!playback_active) {
            return false;
        }

        for (int frame = 0; frame < sample_count; ++frame) {
            const short value = impl_->state.playback_samples.empty() ? 0 : impl_->state.playback_samples.front();
            if (!impl_->state.playback_samples.empty()) {
                impl_->state.playback_samples.pop_front();
            }
            for (int channel = 0; channel < channels; ++channel) {
                samples[frame * channels + channel] = value;
            }
        }

        should_stop = impl_->state.playback_stop_requested && impl_->state.playback_samples.empty();
        if (should_stop) {
            impl_->state.playback_active = false;
            impl_->state.playback_stop_requested = false;
            playback_control = impl_->state.playback_control;
            if (impl_->state.client_connected) {
                enqueue_frame_locked(impl_->state, playback_stopped_frame("drained"));
            }
        }
    }

    if (should_stop && playback_control != nullptr) {
        const auto result = playback_control->deactivate_media_playback();
        if (!result) {
            std::lock_guard<std::mutex> lock(impl_->state.mutex);
            impl_->state.last_error = result.error().message;
        }
    }
    return playback_active;
}

void MediaBridgeServer::accept_loop(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        const int client_fd = ::accept(impl_->state.listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (stop_token.stop_requested()) {
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EBADF || errno == EINVAL) {
                return;
            }
            continue;
        }

        bool busy = false;
        {
            std::lock_guard<std::mutex> lock(impl_->state.mutex);
            busy = impl_->state.client_connected;
        }
        if (busy) {
            const auto frame = error_frame("client_busy", "media bridge already has an active consumer");
            (void)write_frame(client_fd, frame);
            ::close(client_fd);
            continue;
        }

        if (impl_->reader_thread.joinable()) {
            impl_->reader_thread.join();
        }
        if (impl_->writer_thread.joinable()) {
            impl_->writer_thread.join();
        }

        {
            std::lock_guard<std::mutex> lock(impl_->state.mutex);
            impl_->state.client_fd = client_fd;
            impl_->state.client_connected = true;
            impl_->state.outbound_frames.clear();
            enqueue_frame_locked(impl_->state, hello_frame(impl_->state));
            enqueue_frame_locked(impl_->state, status_frame(impl_->state));
            for (const auto& [_, speaker] : impl_->state.active_speakers) {
                enqueue_frame_locked(impl_->state, speaker_frame("speaker.start", speaker));
            }
        }

        impl_->reader_thread = std::jthread([this, client_fd](std::stop_token client_stop_token) {
            reader_loop(client_fd, client_stop_token);
        });
        impl_->writer_thread = std::jthread([this, client_fd](std::stop_token client_stop_token) {
            writer_loop(client_fd, client_stop_token);
        });
    }
}

void MediaBridgeServer::reader_loop(int client_fd, std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        std::string line;
        if (!protocol::read_line(client_fd, line)) {
            break;
        }

        const auto fields = protocol::split_fields(line);
        if (fields.size() < 2 || fields[0] != kMediaMagic) {
            std::lock_guard<std::mutex> lock(impl_->state.mutex);
            impl_->state.last_error = "invalid media bridge request";
            enqueue_frame_locked(impl_->state, error_frame("invalid_request", impl_->state.last_error));
            continue;
        }

        const std::string& command = fields[1];
        if (command == "status.request") {
            std::lock_guard<std::mutex> lock(impl_->state.mutex);
            enqueue_frame_locked(impl_->state, status_frame(impl_->state));
            continue;
        }

        if (command == "playback.start") {
            const auto format = parse_string_field(fields, 2, "format");
            const auto sample_rate = parse_u64_field(fields, 3, "sample_rate");
            const auto channels = parse_u64_field(fields, 4, "channels");
            if (!format || !sample_rate || !channels) {
                std::lock_guard<std::mutex> lock(impl_->state.mutex);
                impl_->state.last_error = !format ? format.error().message
                                                  : (!sample_rate ? sample_rate.error().message
                                                                  : channels.error().message);
                enqueue_frame_locked(impl_->state, error_frame("invalid_request", impl_->state.last_error));
                continue;
            }
            if (format.value() != kMediaSampleFormat || sample_rate.value() != kMediaSampleRate ||
                channels.value() != kMediaPlaybackChannels) {
                std::lock_guard<std::mutex> lock(impl_->state.mutex);
                impl_->state.last_error = "unsupported playback format";
                enqueue_frame_locked(impl_->state, error_frame("unsupported_format", impl_->state.last_error));
                continue;
            }

            MediaPlaybackControl* playback_control = nullptr;
            {
                std::lock_guard<std::mutex> lock(impl_->state.mutex);
                playback_control = impl_->state.playback_control;
            }
            if (playback_control != nullptr) {
                const auto activated = playback_control->activate_media_playback();
                if (!activated) {
                    std::lock_guard<std::mutex> lock(impl_->state.mutex);
                    impl_->state.last_error = activated.error().message;
                    enqueue_frame_locked(
                        impl_->state,
                        error_frame(activated.error().code, activated.error().message)
                    );
                    continue;
                }
            }

            std::lock_guard<std::mutex> lock(impl_->state.mutex);
            impl_->state.playback_active = true;
            impl_->state.playback_stop_requested = false;
            impl_->state.playback_samples.clear();
            enqueue_frame_locked(impl_->state, playback_started_frame());
            continue;
        }

        if (command == "playback.chunk") {
            const auto frame_count = parse_u64_field(fields, 2, "frame_count");
            const auto payload_bytes = parse_u64_field(fields, 3, "payload_bytes");
            if (!frame_count || !payload_bytes) {
                std::lock_guard<std::mutex> lock(impl_->state.mutex);
                impl_->state.last_error =
                    !frame_count ? frame_count.error().message : payload_bytes.error().message;
                enqueue_frame_locked(impl_->state, error_frame("invalid_request", impl_->state.last_error));
                continue;
            }

            std::vector<char> payload(payload_bytes.value());
            if (!read_exact(client_fd, payload.data(), payload.size())) {
                break;
            }

            if (payload.size() != frame_count.value() * sizeof(short)) {
                std::lock_guard<std::mutex> lock(impl_->state.mutex);
                impl_->state.last_error = "playback chunk payload size does not match the declared frame count";
                enqueue_frame_locked(impl_->state, error_frame("invalid_chunk", impl_->state.last_error));
                continue;
            }

            std::lock_guard<std::mutex> lock(impl_->state.mutex);
            if (!impl_->state.playback_active) {
                impl_->state.last_error = "playback.start is required before playback.chunk";
                enqueue_frame_locked(impl_->state, error_frame("playback_inactive", impl_->state.last_error));
                continue;
            }
            const std::size_t payload_samples = payload.size() / sizeof(short);
            if (impl_->state.playback_samples.size() + payload_samples > kMaxQueuedPlaybackSamples) {
                ++impl_->state.dropped_playback_chunks;
                impl_->state.last_error = "playback queue is full";
                enqueue_frame_locked(impl_->state, error_frame("queue_full", impl_->state.last_error));
                continue;
            }
            const short* sample_data = reinterpret_cast<const short*>(payload.data());
            for (std::size_t index = 0; index < payload_samples; ++index) {
                impl_->state.playback_samples.push_back(sample_data[index]);
            }
            continue;
        }

        if (command == "playback.stop") {
            MediaPlaybackControl* playback_control = nullptr;
            bool stop_immediately = false;
            {
                std::lock_guard<std::mutex> lock(impl_->state.mutex);
                if (!impl_->state.playback_active) {
                    enqueue_frame_locked(impl_->state, playback_stopped_frame("inactive"));
                    continue;
                }
                impl_->state.playback_stop_requested = true;
                stop_immediately = impl_->state.playback_samples.empty();
                if (stop_immediately) {
                    impl_->state.playback_active = false;
                    impl_->state.playback_stop_requested = false;
                    playback_control = impl_->state.playback_control;
                    enqueue_frame_locked(impl_->state, playback_stopped_frame("drained"));
                }
            }
            if (stop_immediately && playback_control != nullptr) {
                const auto result = playback_control->deactivate_media_playback();
                if (!result) {
                    std::lock_guard<std::mutex> lock(impl_->state.mutex);
                    impl_->state.last_error = result.error().message;
                }
            }
            continue;
        }

        if (command == "playback.clear") {
            MediaPlaybackControl* playback_control = nullptr;
            {
                std::lock_guard<std::mutex> lock(impl_->state.mutex);
                impl_->state.playback_samples.clear();
                impl_->state.playback_active = false;
                impl_->state.playback_stop_requested = false;
                playback_control = impl_->state.playback_control;
                enqueue_frame_locked(impl_->state, playback_cleared_frame());
            }
            if (playback_control != nullptr) {
                const auto result = playback_control->deactivate_media_playback();
                if (!result) {
                    std::lock_guard<std::mutex> lock(impl_->state.mutex);
                    impl_->state.last_error = result.error().message;
                }
            }
            continue;
        }

        std::lock_guard<std::mutex> lock(impl_->state.mutex);
        impl_->state.last_error = "unsupported media bridge request";
        enqueue_frame_locked(impl_->state, error_frame("unsupported_request", impl_->state.last_error));
    }

    handle_client_disconnect(client_fd);
}

void MediaBridgeServer::writer_loop(int client_fd, std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        OutboundFrame frame;
        {
            std::unique_lock<std::mutex> lock(impl_->state.mutex);
            impl_->state.outbound_cv.wait(lock, [&] {
                return stop_token.stop_requested() || !impl_->state.outbound_frames.empty() ||
                       !impl_->state.client_connected;
            });
            if (stop_token.stop_requested() || !impl_->state.client_connected) {
                return;
            }
            frame = std::move(impl_->state.outbound_frames.front());
            impl_->state.outbound_frames.pop_front();
        }

        if (!write_frame(client_fd, frame)) {
            handle_client_disconnect(client_fd);
            return;
        }
    }
}

void MediaBridgeServer::handle_client_disconnect(int client_fd) {
    MediaPlaybackControl* playback_control = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->state.mutex);
        if (impl_->state.client_fd != client_fd) {
            return;
        }
        impl_->state.client_connected = false;
        impl_->state.client_fd = -1;
        impl_->state.outbound_frames.clear();
        impl_->state.outbound_cv.notify_all();
        impl_->state.playback_samples.clear();
        impl_->state.playback_active = false;
        impl_->state.playback_stop_requested = false;
        playback_control = impl_->state.playback_control;
    }

    shutdown_fd(client_fd);
    ::close(client_fd);

    if (playback_control != nullptr) {
        const auto result = playback_control->deactivate_media_playback();
        if (!result) {
            std::lock_guard<std::mutex> lock(impl_->state.mutex);
            impl_->state.last_error = result.error().message;
        }
    }
}

auto media_format_description() -> std::string {
    return std::string(kMediaSampleFormat) + " @" + std::to_string(kMediaSampleRate) + " Hz mono";
}

}  // namespace teamspeak_cli::bridge
