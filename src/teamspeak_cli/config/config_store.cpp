#include "teamspeak_cli/config/config_store.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "teamspeak_cli/util/strings.hpp"

namespace teamspeak_cli::config {
namespace {

constexpr auto kSupportedConfigVersion = 1;
constexpr auto kMockProfileName = "mock-local";
constexpr auto kMockIdentity = "mock-local-identity";

struct ProfileLineInfo {
    std::size_t section_line = 0;
    std::map<std::string, std::size_t> key_lines;
};

struct ConfigLineInfo {
    std::map<std::string, std::size_t> root_key_lines;
    std::vector<ProfileLineInfo> profiles;
};

auto config_error(std::string code, std::string message) -> domain::Error {
    return domain::make_error("config", std::move(code), std::move(message), domain::ExitCode::config);
}

auto errno_message(int error_number) -> std::string {
    return std::error_code(error_number, std::generic_category()).message();
}

auto line_suffix(std::size_t line_number) -> std::string {
    if (line_number == 0) {
        return "";
    }
    return " on line " + std::to_string(line_number);
}

auto root_key_line(const ConfigLineInfo* lines, const std::string& key) -> std::size_t {
    if (lines == nullptr) {
        return 0;
    }
    const auto found = lines->root_key_lines.find(key);
    if (found == lines->root_key_lines.end()) {
        return 0;
    }
    return found->second;
}

auto profile_section_line(const ConfigLineInfo* lines, std::size_t profile_index) -> std::size_t {
    if (lines == nullptr || profile_index >= lines->profiles.size()) {
        return 0;
    }
    return lines->profiles[profile_index].section_line;
}

auto profile_key_line(const ConfigLineInfo* lines, std::size_t profile_index, const std::string& key)
    -> std::size_t {
    if (lines == nullptr || profile_index >= lines->profiles.size()) {
        return 0;
    }
    const auto found = lines->profiles[profile_index].key_lines.find(key);
    if (found == lines->profiles[profile_index].key_lines.end()) {
        return 0;
    }
    return found->second;
}

auto is_supported_backend(const std::string& backend) -> bool {
    return util::iequals(backend, "mock") || util::iequals(backend, "fake") ||
           util::iequals(backend, "plugin");
}

auto canonical_backend(const std::string& backend) -> std::string {
    if (util::iequals(backend, "fake") || util::iequals(backend, "mock")) {
        return "mock";
    }
    if (util::iequals(backend, "plugin")) {
        return "plugin";
    }
    return backend;
}

auto validate_config(const domain::AppConfig& config, const ConfigLineInfo* lines = nullptr)
    -> domain::Result<void> {
    if (config.version != kSupportedConfigVersion) {
        return domain::fail(config_error(
            "unsupported_version",
            "unsupported config version" + line_suffix(root_key_line(lines, "version")) + ": " +
                std::to_string(config.version)
        ));
    }

    std::map<std::string, std::size_t> seen_profiles;
    for (std::size_t index = 0; index < config.profiles.size(); ++index) {
        const auto& profile = config.profiles[index];
        if (util::trim(profile.name).empty()) {
            return domain::fail(config_error(
                "invalid_profile_name",
                "profile name cannot be empty" + line_suffix(profile_section_line(lines, index))
            ));
        }

        if (!seen_profiles.emplace(profile.name, index).second) {
            return domain::fail(config_error(
                "duplicate_profile",
                "duplicate profile name" + line_suffix(profile_section_line(lines, index)) + ": " +
                    profile.name
            ));
        }

        if (!is_supported_backend(profile.backend)) {
            return domain::fail(config_error(
                "invalid_backend",
                "unsupported backend in profile " + profile.name +
                    line_suffix(profile_key_line(lines, index, "backend")) + ": " + profile.backend
            ));
        }

        if (util::trim(profile.host).empty()) {
            return domain::fail(config_error(
                "invalid_host",
                "host cannot be empty in profile " + profile.name +
                    line_suffix(profile_key_line(lines, index, "host"))
            ));
        }

        if (profile.port == 0) {
            return domain::fail(config_error(
                "invalid_port",
                "port cannot be 0 in profile " + profile.name +
                    line_suffix(profile_key_line(lines, index, "port"))
            ));
        }
    }

    if (seen_profiles.find(config.active_profile) == seen_profiles.end()) {
        return domain::fail(config_error(
            "invalid_active_profile",
            "active_profile references missing profile" +
                line_suffix(root_key_line(lines, "active_profile")) + ": " + config.active_profile
        ));
    }

    return domain::ok();
}

auto render_config(const domain::AppConfig& config) -> std::string {
    std::ostringstream out;
    out << "version=" << config.version << '\n';
    out << "active_profile=" << config.active_profile << '\n';
    for (const auto& profile : config.profiles) {
        out << '\n';
        out << "[profile." << profile.name << "]\n";
        out << "backend=" << canonical_backend(profile.backend) << '\n';
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

auto apply_root_key(
    domain::AppConfig& config,
    const std::string& key,
    const std::string& value,
    std::size_t line_number
)
    -> domain::Result<void> {
    if (key == "version") {
        const auto parsed = util::parse_u64(value);
        if (!parsed.has_value()) {
            return domain::fail(config_error(
                "invalid_version", "invalid config version" + line_suffix(line_number)
            ));
        }
        if (*parsed > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            return domain::fail(config_error(
                "unsupported_version",
                "unsupported config version" + line_suffix(line_number) + ": " + value
            ));
        }
        config.version = static_cast<int>(*parsed);
        return domain::ok();
    }
    if (key == "active_profile") {
        config.active_profile = value;
        return domain::ok();
    }
    return domain::fail(config_error(
        "unknown_key", "unknown config key" + line_suffix(line_number) + ": " + key
    ));
}

auto apply_profile_key(
    domain::Profile& profile,
    const std::string& key,
    const std::string& value,
    std::size_t line_number
)
    -> domain::Result<void> {
    if (key == "backend") {
        profile.backend = util::iequals(value, "fake") ? "mock" : value;
    } else if (key == "host") {
        profile.host = value;
    } else if (key == "port") {
        const auto parsed = util::parse_u16(value);
        if (!parsed.has_value()) {
            return domain::fail(config_error(
                "invalid_port",
                "invalid port in profile " + profile.name + line_suffix(line_number)
            ));
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
        return domain::fail(config_error(
            "unknown_profile_key",
            "unknown profile key in profile " + profile.name + line_suffix(line_number) + ": " + key
        ));
    }
    return domain::ok();
}

#if defined(_WIN32)
auto make_temp_path(const std::filesystem::path& target, std::size_t attempt)
    -> std::filesystem::path {
    const auto parent = target.parent_path();
    const auto filename = target.filename().empty() ? std::string("config") : target.filename().string();
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temp_name =
        "." + filename + ".tmp." + std::to_string(nonce) + "." + std::to_string(attempt);
    return parent.empty() ? std::filesystem::path(temp_name) : parent / temp_name;
}

auto write_temp_config(const std::filesystem::path& target, const std::string& contents)
    -> domain::Result<std::filesystem::path> {
    for (std::size_t attempt = 0; attempt < 100; ++attempt) {
        const auto temp_path = make_temp_path(target, attempt);
        std::error_code exists_ec;
        if (std::filesystem::exists(temp_path, exists_ec)) {
            continue;
        }

        std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            return domain::fail<std::filesystem::path>(config_error(
                "temp_write_failed",
                "failed to open temporary config file " + temp_path.string()
            ));
        }

        output << contents;
        output.flush();
        if (!output) {
            std::error_code remove_ec;
            std::filesystem::remove(temp_path, remove_ec);
            return domain::fail<std::filesystem::path>(config_error(
                "temp_write_failed",
                "failed to write temporary config file " + temp_path.string()
            ));
        }
        output.close();
        if (!output) {
            std::error_code remove_ec;
            std::filesystem::remove(temp_path, remove_ec);
            return domain::fail<std::filesystem::path>(config_error(
                "temp_write_failed",
                "failed to close temporary config file " + temp_path.string()
            ));
        }

        return domain::ok(temp_path);
    }

    return domain::fail<std::filesystem::path>(config_error(
        "temp_write_failed", "failed to choose temporary config file near " + target.string()
    ));
}
#else
auto make_temp_template(const std::filesystem::path& target) -> std::string {
    const auto parent = target.parent_path();
    const auto filename = target.filename().empty() ? std::string("config") : target.filename().string();
    const auto temp_name = "." + filename + ".tmp.XXXXXX";
    return (parent.empty() ? std::filesystem::path(temp_name) : parent / temp_name).string();
}

auto write_temp_config(const std::filesystem::path& target, const std::string& contents)
    -> domain::Result<std::filesystem::path> {
    auto temp_template = make_temp_template(target);
    std::vector<char> temp_path_buffer(temp_template.begin(), temp_template.end());
    temp_path_buffer.push_back('\0');

    int fd = ::mkstemp(temp_path_buffer.data());
    if (fd == -1) {
        return domain::fail<std::filesystem::path>(config_error(
            "temp_write_failed",
            "failed to create temporary config file near " + target.string() + ": " +
                errno_message(errno)
        ));
    }

    const auto temp_path = std::filesystem::path(temp_path_buffer.data());
    const auto private_mode = static_cast<mode_t>(S_IRUSR | S_IWUSR);

    auto fail_after_temp_created =
        [&](std::string code, std::string message, int error_number)
        -> domain::Result<std::filesystem::path> {
        if (fd >= 0) {
            static_cast<void>(::close(fd));
            fd = -1;
        }
        std::error_code remove_ec;
        std::filesystem::remove(temp_path, remove_ec);
        return domain::fail<std::filesystem::path>(config_error(
            std::move(code), std::move(message) + ": " + errno_message(error_number)
        ));
    };

    if (::fchmod(fd, private_mode) != 0) {
        return fail_after_temp_created(
            "temp_write_failed", "failed to set permissions on temporary config file " + temp_path.string(), errno
        );
    }

    const char* next = contents.data();
    std::size_t remaining = contents.size();
    while (remaining > 0) {
        const auto written = ::write(fd, next, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail_after_temp_created(
                "temp_write_failed", "failed to write temporary config file " + temp_path.string(), errno
            );
        }
        if (written == 0) {
            return fail_after_temp_created(
                "temp_write_failed", "failed to write temporary config file " + temp_path.string(), EIO
            );
        }
        const auto written_size = static_cast<std::size_t>(written);
        next += written_size;
        remaining -= written_size;
    }

    if (::fsync(fd) != 0) {
        return fail_after_temp_created(
            "temp_write_failed", "failed to flush temporary config file " + temp_path.string(), errno
        );
    }

    if (::close(fd) != 0) {
        const auto saved_errno = errno;
        fd = -1;
        std::error_code remove_ec;
        std::filesystem::remove(temp_path, remove_ec);
        return domain::fail<std::filesystem::path>(config_error(
            "temp_write_failed",
            "failed to close temporary config file " + temp_path.string() + ": " +
                errno_message(saved_errno)
        ));
    }
    fd = -1;

    return domain::ok(temp_path);
}
#endif

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
    ConfigLineInfo line_info;

    constexpr auto kNoProfile = static_cast<std::size_t>(-1);
    std::size_t current_profile_index = kNoProfile;
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
            line_info.profiles.push_back(ProfileLineInfo{.section_line = line_number, .key_lines = {}});
            current_profile_index = config.profiles.size() - 1;
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
        domain::Result<void> applied = domain::ok();
        if (current_profile_index == kNoProfile) {
            line_info.root_key_lines[key] = line_number;
            applied = apply_root_key(config, key, value, line_number);
        } else {
            line_info.profiles[current_profile_index].key_lines[key] = line_number;
            applied = apply_profile_key(
                config.profiles[current_profile_index], key, value, line_number
            );
        }
        if (!applied) {
            return domain::fail<domain::AppConfig>(applied.error());
        }
    }

    if (input.bad()) {
        return domain::fail<domain::AppConfig>(config_error(
            "read_failed", "failed to read config file: " + path.string()
        ));
    }

    if (config.profiles.empty()) {
        config = default_config();
        line_info = ConfigLineInfo{};
    }

    const auto validation = validate_config(config, &line_info);
    if (!validation) {
        return domain::fail<domain::AppConfig>(validation.error());
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
    const auto validation = validate_config(config);
    if (!validation) {
        return validation;
    }

    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return domain::fail(config_error(
                "mkdir_failed", "failed to create config directory: " + ec.message()
            ));
        }
    }

    auto temp_path = write_temp_config(path, render_config(config));
    if (!temp_path) {
        return domain::fail(temp_path.error());
    }

    std::filesystem::rename(temp_path.value(), path, ec);
    if (ec) {
        std::error_code remove_ec;
        std::filesystem::remove(temp_path.value(), remove_ec);
        return domain::fail(config_error(
            "rename_failed",
            "failed to replace config file " + path.string() + " with " +
                temp_path.value().string() + ": " + ec.message()
        ));
    }

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

auto ConfigStore::create_profile(domain::AppConfig& config, domain::Profile profile) const
    -> domain::Result<domain::Profile*> {
    if (profile.name.empty()) {
        return domain::fail<domain::Profile*>(config_error(
            "invalid_profile_name", "profile name cannot be empty"
        ));
    }

    auto existing = find_profile(config, profile.name);
    if (existing) {
        return domain::fail<domain::Profile*>(config_error(
            "profile_exists", "profile already exists: " + profile.name
        ));
    }

    config.profiles.push_back(std::move(profile));
    return domain::ok(&config.profiles.back());
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
