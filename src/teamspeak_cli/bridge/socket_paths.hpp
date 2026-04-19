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

}  // namespace teamspeak_cli::bridge
