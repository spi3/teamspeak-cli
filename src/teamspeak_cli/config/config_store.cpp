#include "teamspeak_cli/config/config_store.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "teamspeak_cli/util/strings.hpp"

namespace teamspeak_cli::config {
namespace {

constexpr auto kMockProfileName = "mock-local";
constexpr auto kMockIdentity = "mock-local-identity";

auto config_error(std::string code, std::string message) -> domain::Error {
    return domain::make_error("config", std::move(code), std::move(message), domain::ExitCode::config);
}

auto render_config(const domain::AppConfig& config) -> std::string {
    std::ostringstream out;
    out << "version=" << config.version << '\n';
    out << "active_profile=" << config.active_profile << '\n';
    for (const auto& profile : config.profiles) {
        out << '\n';
        out << "[profile." << profile.name << "]\n";
        out << "backend=" << profile.backend << '\n';
        out << "host=" << profile.host << '\n';
        out << "port=" << profile.port << '\n';
        out << "nickname=" << profile.nickname << '\n';
        out << "identity=" << profile.identity << '\n';
        out << "server_password=" << profile.server_password << '\n';
        out << "channel_password=" << profile.channel_password << '\n';
        out << "default_channel=" << profile.default_channel << '\n';
        out << "control_socket_path=" << profile.control_socket_path << '\n';
    }
    return out.str();
}

auto apply_root_key(domain::AppConfig& config, const std::string& key, const std::string& value)
    -> domain::Result<void> {
    if (key == "version") {
        const auto parsed = util::parse_u64(value);
        if (!parsed.has_value()) {
            return domain::fail(config_error("invalid_version", "invalid config version"));
        }
        config.version = static_cast<int>(*parsed);
        return domain::ok();
    }
    if (key == "active_profile") {
        config.active_profile = value;
        return domain::ok();
    }
    return domain::fail(config_error("unknown_key", "unknown config key: " + key));
}

auto apply_profile_key(domain::Profile& profile, const std::string& key, const std::string& value)
    -> domain::Result<void> {
    if (key == "backend") {
        profile.backend = util::iequals(value, "fake") ? "mock" : value;
    } else if (key == "host") {
        profile.host = value;
    } else if (key == "port") {
        const auto parsed = util::parse_u16(value);
        if (!parsed.has_value()) {
            return domain::fail(config_error("invalid_port", "invalid port in profile " + profile.name));
        }
        profile.port = *parsed;
    } else if (key == "nickname") {
        profile.nickname = value;
    } else if (key == "identity") {
        profile.identity = value;
    } else if (key == "server_password") {
        profile.server_password = value;
    } else if (key == "channel_password") {
        profile.channel_password = value;
    } else if (key == "default_channel") {
        profile.default_channel = value;
    } else if (key == "control_socket_path") {
        profile.control_socket_path = value;
    } else if (key == "headless_audio" || key == "sdk_resources_dir" || key == "sdk_library_dir") {
        // Legacy ClientLib settings are ignored after the plugin pivot.
    } else {
        return domain::fail(config_error("unknown_profile_key", "unknown profile key: " + key));
    }
    return domain::ok();
}

}  // namespace

auto ConfigStore::default_path() const -> std::filesystem::path {
    if (const char* explicit_path = std::getenv("TS_CONFIG_PATH")) {
        return explicit_path;
    }
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        return std::filesystem::path(xdg) / "ts" / "config.ini";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".config" / "ts" / "config.ini";
    }
    return std::filesystem::path("config.ini");
}

auto ConfigStore::load(const std::filesystem::path& path) const -> domain::Result<domain::AppConfig> {
    std::ifstream input(path);
    if (!input) {
        return domain::fail<domain::AppConfig>(config_error(
            "missing_config", "config file not found: " + path.string()
        ));
    }

    domain::AppConfig config = default_config();
    config.profiles.clear();

    domain::Profile* current_profile = nullptr;
    std::string line;
    std::size_t line_number = 0;

    while (std::getline(input, line)) {
        ++line_number;
        std::string trimmed = util::trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }

        if (trimmed.front() == '[' && trimmed.back() == ']') {
            const std::string section = trimmed.substr(1, trimmed.size() - 2);
            const std::string prefix = "profile.";
            if (section.rfind(prefix, 0) != 0 || section.size() == prefix.size()) {
                return domain::fail<domain::AppConfig>(config_error(
                    "invalid_section",
                    "invalid section on line " + std::to_string(line_number) + ": " + section
                ));
            }
            config.profiles.push_back(domain::Profile{
                .name = section.substr(prefix.size()),
                .backend = "mock",
                .host = "127.0.0.1",
                .port = 9987,
                .nickname = "terminal",
                .identity = "",
                .server_password = "",
                .channel_password = "",
                .default_channel = "Lobby",
                .control_socket_path = "",
            });
            current_profile = &config.profiles.back();
            continue;
        }

        const auto separator = trimmed.find('=');
        if (separator == std::string::npos) {
            return domain::fail<domain::AppConfig>(config_error(
                "invalid_line", "expected key=value on line " + std::to_string(line_number)
            ));
        }

        const std::string key = util::trim(trimmed.substr(0, separator));
        const std::string value = util::trim(trimmed.substr(separator + 1));
        const auto applied =
            current_profile == nullptr ? apply_root_key(config, key, value) : apply_profile_key(*current_profile, key, value);
        if (!applied) {
            return domain::fail<domain::AppConfig>(applied.error());
        }
    }

    if (config.profiles.empty()) {
        config = default_config();
    }

    return domain::ok(std::move(config));
}

auto ConfigStore::load_or_default(const std::filesystem::path& path) const -> domain::Result<domain::AppConfig> {
    if (!std::filesystem::exists(path)) {
        return domain::ok(default_config());
    }
    return load(path);
}

auto ConfigStore::save(const std::filesystem::path& path, const domain::AppConfig& config) const
    -> domain::Result<void> {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return domain::fail(config_error(
            "mkdir_failed", "failed to create config directory: " + ec.message()
        ));
    }

    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        return domain::fail(config_error(
            "write_failed", "failed to write config file: " + path.string()
        ));
    }

    output << render_config(config);
    return domain::ok();
}

auto ConfigStore::init(const std::filesystem::path& path, bool force) const
    -> domain::Result<domain::AppConfig> {
    if (std::filesystem::exists(path) && !force) {
        return load(path);
    }

    domain::AppConfig config = default_config();
    const auto saved = save(path, config);
    if (!saved) {
        return domain::fail<domain::AppConfig>(saved.error());
    }
    return domain::ok(std::move(config));
}

auto ConfigStore::find_profile(domain::AppConfig& config, const std::string& name) const
    -> domain::Result<domain::Profile*> {
    for (auto& profile : config.profiles) {
        if (profile.name == name) {
            return domain::ok(&profile);
        }
    }
    return domain::fail<domain::Profile*>(config_error("profile_not_found", "profile not found: " + name));
}

auto ConfigStore::find_profile(const domain::AppConfig& config, const std::string& name) const
    -> domain::Result<const domain::Profile*> {
    for (const auto& profile : config.profiles) {
        if (profile.name == name) {
            return domain::ok(&profile);
        }
    }
    return domain::fail<const domain::Profile*>(config_error(
        "profile_not_found", "profile not found: " + name
    ));
}

auto ConfigStore::default_config() const -> domain::AppConfig {
    return domain::AppConfig{
        .version = 1,
        .active_profile = "plugin-local",
        .profiles =
            {
                domain::Profile{
                    .name = kMockProfileName,
                    .backend = "mock",
                    .host = "127.0.0.1",
                    .port = 9987,
                    .nickname = "terminal",
                    .identity = kMockIdentity,
                    .server_password = "",
                    .channel_password = "",
                    .default_channel = "Lobby",
                    .control_socket_path = "",
                },
                domain::Profile{
                    .name = "plugin-local",
                    .backend = "plugin",
                    .host = "127.0.0.1",
                    .port = 9987,
                    .nickname = "terminal",
                    .identity = "",
                    .server_password = "",
                    .channel_password = "",
                    .default_channel = "",
                    .control_socket_path = "",
                },
            },
    };
}

}  // namespace teamspeak_cli::config
