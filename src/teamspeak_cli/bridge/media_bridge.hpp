#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

#include "teamspeak_cli/domain/models.hpp"
#include "teamspeak_cli/domain/result.hpp"

namespace teamspeak_cli::bridge {

inline constexpr std::string_view kMediaMagic = "tsmedia1";
inline constexpr std::string_view kMediaSampleFormat = "pcm_s16le";
inline constexpr int kMediaSampleRate = 48000;
inline constexpr int kMediaPlaybackChannels = 1;

struct MediaSpeaker {
    std::uint64_t handler_id = 0;
    domain::ClientId client_id;
    std::string unique_identity;
    std::string nickname;
    std::optional<domain::ChannelId> channel_id;
};

struct MediaStatus {
    bool consumer_connected = false;
    bool playback_active = false;
    std::size_t queued_playback_samples = 0;
    std::size_t active_speaker_count = 0;
    std::size_t dropped_audio_chunks = 0;
    std::size_t dropped_playback_chunks = 0;
    std::string last_error;
};

class MediaPlaybackControl {
  public:
    virtual ~MediaPlaybackControl() = default;

    [[nodiscard]] virtual auto activate_media_playback() -> domain::Result<void> = 0;
    [[nodiscard]] virtual auto deactivate_media_playback() -> domain::Result<void> = 0;
};

class MediaBridge {
  public:
    virtual ~MediaBridge() = default;

    [[nodiscard]] virtual auto socket_path() const -> std::string = 0;
    [[nodiscard]] virtual auto status() const -> MediaStatus = 0;

    virtual void publish_speaker_start(const MediaSpeaker& speaker) = 0;
    virtual void publish_speaker_stop(const MediaSpeaker& speaker) = 0;
    virtual void publish_audio_chunk(
        const MediaSpeaker& speaker,
        int sample_rate,
        int channels,
        const short* samples,
        int sample_count
    ) = 0;

    [[nodiscard]] virtual auto fill_playback_samples(
        int sample_rate,
        int channels,
        short* samples,
        int sample_count
    ) -> bool = 0;
};

class MediaBridgeHost {
  public:
    virtual ~MediaBridgeHost() = default;

    virtual void set_media_bridge(const std::shared_ptr<MediaBridge>& media_bridge) = 0;
};

class MediaBridgeServer final : public MediaBridge {
  public:
    MediaBridgeServer();
    ~MediaBridgeServer() override;

    [[nodiscard]] auto start(std::string socket_path, MediaPlaybackControl* playback_control)
        -> domain::Result<void>;
    [[nodiscard]] auto stop() -> domain::Result<void>;

    [[nodiscard]] auto socket_path() const -> std::string override;
    [[nodiscard]] auto status() const -> MediaStatus override;

    void publish_speaker_start(const MediaSpeaker& speaker) override;
    void publish_speaker_stop(const MediaSpeaker& speaker) override;
    void publish_audio_chunk(
        const MediaSpeaker& speaker,
        int sample_rate,
        int channels,
        const short* samples,
        int sample_count
    ) override;

    [[nodiscard]] auto fill_playback_samples(
        int sample_rate,
        int channels,
        short* samples,
        int sample_count
    ) -> bool override;

  private:
    class Impl;

    void accept_loop(std::stop_token stop_token);
    void reader_loop(int client_fd, std::stop_token stop_token);
    void writer_loop(int client_fd, std::stop_token stop_token);
    void handle_client_disconnect(int client_fd);

    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] auto media_format_description() -> std::string;

}  // namespace teamspeak_cli::bridge
