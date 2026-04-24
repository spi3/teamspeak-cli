#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>

#include "teamspeak_cli/domain/result.hpp"

namespace teamspeak_cli::bridge {

struct PlaybackSendRequest {
    std::string socket_path;
    std::filesystem::path file_path;
    std::chrono::milliseconds timeout = std::chrono::seconds(60);
    bool clear_first = false;
};

struct PlaybackSendResult {
    std::filesystem::path file_path;
    std::string socket_path;
    std::string media_format;
    std::size_t frames_sent = 0;
    std::size_t bytes_sent = 0;
    bool cleared_first = false;
    std::string stop_reason;
};

[[nodiscard]] auto send_playback_file(const PlaybackSendRequest& request)
    -> domain::Result<PlaybackSendResult>;

}  // namespace teamspeak_cli::bridge
