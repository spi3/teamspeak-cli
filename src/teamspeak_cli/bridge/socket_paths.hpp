#pragma once

#include <cstdlib>
#include <string>
#include <string_view>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace teamspeak_cli::bridge {

inline auto default_socket_path() -> std::string {
    if (const char* explicit_path = std::getenv("TS_CONTROL_SOCKET_PATH")) {
        return explicit_path;
    }

#if defined(_WIN32)
    if (const char* temp = std::getenv("TEMP")) {
        return std::string(temp) + "\\ts3cli-plugin.sock";
    }
    return "ts3cli-plugin.sock";
#else
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR")) {
        return std::string(runtime) + "/ts3cli.sock";
    }
    if (const char* tmpdir = std::getenv("TMPDIR")) {
        return std::string(tmpdir) + "/ts3cli-" + std::to_string(getuid()) + ".sock";
    }
    return "/tmp/ts3cli-" + std::to_string(getuid()) + ".sock";
#endif
}

inline auto resolve_socket_path(std::string_view configured_path) -> std::string {
    if (!configured_path.empty()) {
        return std::string(configured_path);
    }
    return default_socket_path();
}

inline auto derive_media_socket_path(std::string_view control_socket_path) -> std::string {
    std::string control = control_socket_path.empty() ? default_socket_path() : std::string(control_socket_path);
    constexpr std::string_view suffix = ".sock";
    if (control.size() >= suffix.size() &&
        control.compare(control.size() - suffix.size(), suffix.size(), suffix) == 0) {
        control.resize(control.size() - suffix.size());
        control += "-media.sock";
        return control;
    }
    control += ".media";
    return control;
}

inline auto resolve_media_socket_path(std::string_view configured_control_path) -> std::string {
    if (const char* explicit_path = std::getenv("TS_MEDIA_SOCKET_PATH")) {
        return explicit_path;
    }
    return derive_media_socket_path(resolve_socket_path(configured_control_path));
}

}  // namespace teamspeak_cli::bridge
