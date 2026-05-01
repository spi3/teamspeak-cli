#include "teamspeak_cli/cli/command_router.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>

#include "teamspeak_cli/bridge/media_bridge.hpp"
#include "teamspeak_cli/bridge/media_client.hpp"
#include "teamspeak_cli/bridge/socket_paths.hpp"
#include "teamspeak_cli/build/version.hpp"
#include "teamspeak_cli/cli/completion.hpp"
#include "teamspeak_cli/daemon/runtime.hpp"
#include "teamspeak_cli/session/session_service.hpp"
#include "teamspeak_cli/util/scope_exit.hpp"
#include "teamspeak_cli/util/strings.hpp"

namespace teamspeak_cli::cli {
namespace {

constexpr auto kConnectCompletionTimeout = std::chrono::seconds(15);
constexpr auto kDisconnectCompletionTimeout = std::chrono::seconds(10);
constexpr std::string_view kOfficialReleaseRepo = "spi3/teamspeak-cli";
constexpr std::string_view kOfficialInstallReleaseUrl =
    "https://raw.githubusercontent.com/spi3/teamspeak-cli/main/scripts/install-release.sh";
constexpr std::array<std::string_view, 1> kClientRequiredSharedLibraries = {"libXi.so.6"};
const std::array<std::filesystem::path, 2> kLdconfigFallbackPaths = {
    "/usr/sbin/ldconfig",
    "/sbin/ldconfig"
};
volatile std::sig_atomic_t g_daemon_stop_requested = 0;

extern "C" void daemon_signal_handler(int) {
    g_daemon_stop_requested = 1;
}

struct CommandDoc {
    std::vector<std::string> path;
    std::string usage;
    std::string summary;
    std::vector<std::string> options;
};

const std::vector<CommandDoc>& command_docs() {
    static const std::vector<CommandDoc> docs = {
        {{"version"}, "ts version", "Show CLI version information", {}},
        {{"plugin"}, "ts plugin <subcommand>", "Inspect TeamSpeak client plugin helpers", {}},
        {{"plugin", "info"}, "ts plugin info", "Show TeamSpeak client plugin backend information", {}},
        {{"sdk"}, "ts sdk <subcommand>", "Use the deprecated SDK compatibility helpers", {}},
        {{"sdk", "info"}, "ts sdk info", "Deprecated alias for plugin info", {}},
        {{"config"}, "ts config <subcommand>", "Manage CLI config files", {}},
        {{"config", "init"}, "ts config init [--force]", "Write a starter config file", {"--force"}},
        {{"config", "view"}, "ts config view", "Render the current config", {}},
        {{"profile"}, "ts profile <subcommand>", "Manage config profiles", {}},
        {{"profile", "create"}, "ts profile create <name> [--copy-from <name>] [--activate]", "Create a config profile", {"--copy-from", "--activate"}},
        {{"profile", "list"}, "ts profile list", "List config profiles", {}},
        {{"profile", "use"}, "ts profile use <name>", "Set the active default profile", {}},
        {{"update"}, "ts update [--release-tag TAG]", "Update this release install from the official GitHub release", {"--release-tag"}},
        {{"connect"}, "ts connect", "Open a TeamSpeak server connection and wait for completion", {}},
        {{"disconnect"}, "ts disconnect", "Ask the TeamSpeak client plugin to close the current connection", {}},
        {{"mute"}, "ts mute", "Mute your TeamSpeak microphone", {}},
        {{"unmute"}, "ts unmute", "Unmute your TeamSpeak microphone", {}},
        {{"away"}, "ts away [--message <text>]", "Set your TeamSpeak status to away", {"--message"}},
        {{"back"}, "ts back", "Clear your TeamSpeak away status", {}},
        {{"status"}, "ts status", "Show current TeamSpeak client connection status", {}},
        {{"server"}, "ts server <subcommand>", "Inspect the current server session", {}},
        {{"server", "info"}, "ts server info", "Show server details", {}},
        {{"channel"}, "ts channel <subcommand>", "Inspect and join channels", {}},
        {{"channel", "list"}, "ts channel list", "List channels", {}},
        {{"channel", "clients"}, "ts channel clients [id-or-name]", "List clients in one channel or across all channels", {}},
        {{"channel", "get"}, "ts channel get <id-or-name>", "Show one channel", {}},
        {{"channel", "join"}, "ts channel join <id-or-name>", "Join a channel if supported", {}},
        {{"client"}, "ts client <subcommand>", "Inspect connected clients and manage the local TeamSpeak client", {}},
        {{"client", "status"}, "ts client status", "Show the tracked local TeamSpeak client process status", {}},
        {{"client", "start"}, "ts client start", "Launch the local TeamSpeak client process", {}},
        {{"client", "stop"}, "ts client stop [--force]", "Stop the tracked local TeamSpeak client process", {"--force"}},
        {{"client", "logs"}, "ts client logs [--count N]", "Show recent TeamSpeak client logs", {"--count"}},
        {{"client", "list"}, "ts client list", "List connected clients", {}},
        {{"client", "get"}, "ts client get <id-or-name>", "Show one client", {}},
        {{"daemon"}, "ts daemon <subcommand>", "Manage the local TeamSpeak event daemon", {}},
        {{"daemon", "start"}, "ts daemon start [--foreground] [--poll-ms N]", "Start the local TeamSpeak event daemon", {"--foreground", "--poll-ms"}},
        {{"daemon", "stop"}, "ts daemon stop [--timeout-ms N]", "Stop the local TeamSpeak event daemon", {"--timeout-ms"}},
        {{"daemon", "status"}, "ts daemon status", "Show TeamSpeak event daemon status", {}},
        {{"message"}, "ts message <subcommand>", "Send TeamSpeak text messages", {}},
        {{"message", "send"}, "ts message send --target <client|channel> --id <id-or-name> --text <message>", "Send a text message if supported", {"--target", "--id", "--text"}},
        {{"message", "inbox"}, "ts message inbox [--count N]", "Show messages captured by the local TeamSpeak event daemon", {"--count"}},
        {{"playback"}, "ts playback <subcommand>", "Send outbound TeamSpeak playback audio", {}},
        {{"playback", "status"}, "ts playback status", "Show media playback and audio routing diagnostics", {}},
        {{"playback", "send"}, "ts playback send --file <wav> [--clear] [--timeout-ms N]", "Send a WAV file through the plugin media bridge", {"--file", "--clear", "--timeout-ms"}},
        {{"events"}, "ts events <subcommand>", "Watch backend domain events", {}},
        {{"events", "watch"}, "ts events watch [--count N] [--timeout-ms N]", "Watch backend domain events", {"--count", "--timeout-ms"}},
        {{"events", "hook"}, "ts events hook <subcommand>", "Manage daemon event hooks", {}},
        {{"events", "hook", "add"}, "ts events hook add --type <event-type> --exec <command> [--message-kind <client|channel|server>]", "Add a daemon hook for an event type", {"--type", "--exec", "--message-kind"}},
        {{"events", "hook", "list"}, "ts events hook list", "List daemon hook commands", {}},
        {{"events", "hook", "remove"}, "ts events hook remove <id>", "Remove one daemon hook command", {}},
        {{"completion"}, "ts completion bash|zsh|fish|powershell", "Emit a shell completion script", {}},
    };
    return docs;
}

auto path_is_prefix(const std::vector<std::string>& prefix, const std::vector<std::string>& path) -> bool {
    if (prefix.size() > path.size()) {
        return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (prefix[index] != path[index]) {
            return false;
        }
    }
    return true;
}

auto find_command_doc(const std::vector<std::string>& path) -> const CommandDoc* {
    for (const auto& doc : command_docs()) {
        if (doc.path == path) {
            return &doc;
        }
    }
    return nullptr;
}

auto has_command_prefix(const std::vector<std::string>& path) -> bool {
    for (const auto& doc : command_docs()) {
        if (path_is_prefix(path, doc.path)) {
            return true;
        }
    }
    return false;
}

auto has_subcommands(const std::vector<std::string>& path) -> bool {
    for (const auto& doc : command_docs()) {
        if (doc.path.size() == path.size() + 1 && path_is_prefix(path, doc.path)) {
            return true;
        }
    }
    return false;
}

auto immediate_subcommands(const std::vector<std::string>& path) -> std::vector<const CommandDoc*> {
    std::vector<const CommandDoc*> children;
    for (const auto& doc : command_docs()) {
        if (doc.path.size() == path.size() + 1 && path_is_prefix(path, doc.path)) {
            children.push_back(&doc);
        }
    }
    return children;
}

auto format_command_path(const std::vector<std::string>& path) -> std::string {
    return util::join(path, " ");
}

auto cli_error(std::string code, std::string message) -> domain::Error {
    return domain::make_error("cli", std::move(code), std::move(message), domain::ExitCode::usage);
}

auto app_error(std::string category, std::string code, std::string message, domain::ExitCode exit_code)
    -> domain::Error {
    return domain::make_error(std::move(category), std::move(code), std::move(message), exit_code);
}

auto client_error(std::string code, std::string message, domain::ExitCode exit_code) -> domain::Error {
    return domain::make_error("client", std::move(code), std::move(message), exit_code);
}

auto update_error(std::string code, std::string message, domain::ExitCode exit_code) -> domain::Error {
    return domain::make_error("update", std::move(code), std::move(message), exit_code);
}

auto add_error_hint(domain::Error& error, std::string hint) -> void {
    if (hint.empty()) {
        return;
    }

    constexpr std::string_view kHintPrefix = "hint_";
    for (const auto& [key, value] : error.details) {
        if (key.rfind(kHintPrefix, 0) == 0 && value == hint) {
            return;
        }
    }

    std::size_t next_index = 1;
    while (true) {
        std::ostringstream key;
        key << kHintPrefix << std::setw(3) << std::setfill('0') << next_index;
        if (!error.details.contains(key.str())) {
            error.details.emplace(key.str(), std::move(hint));
            return;
        }
        ++next_index;
    }
}

auto help_command_for_path(std::string_view path) -> std::string {
    if (path.empty()) {
        return "ts --help";
    }
    return "ts " + std::string(path) + " --help";
}

auto session_action_for_path(std::string_view path) -> std::string_view {
    if (path == "connect") {
        return "connect to TeamSpeak";
    }
    if (path == "disconnect") {
        return "disconnect from TeamSpeak";
    }
    if (path == "mute") {
        return "mute your TeamSpeak microphone";
    }
    if (path == "unmute") {
        return "unmute your TeamSpeak microphone";
    }
    if (path == "away") {
        return "set your TeamSpeak away status";
    }
    if (path == "back") {
        return "clear your TeamSpeak away status";
    }
    if (path == "status") {
        return "read TeamSpeak status";
    }
    if (path == "server info") {
        return "read TeamSpeak server info";
    }
    if (path == "channel list") {
        return "list TeamSpeak channels";
    }
    if (path == "channel get") {
        return "read TeamSpeak channel details";
    }
    if (path == "channel join") {
        return "join the TeamSpeak channel";
    }
    if (path == "channel clients") {
        return "list TeamSpeak clients by channel";
    }
    if (path == "client list") {
        return "list TeamSpeak clients";
    }
    if (path == "client get") {
        return "read TeamSpeak client details";
    }
    if (path == "message send") {
        return "send the TeamSpeak message";
    }
    if (path == "playback send") {
        return "send TeamSpeak playback audio";
    }
    if (path == "playback status") {
        return "read TeamSpeak playback diagnostics";
    }
    if (path == "events watch") {
        return "watch TeamSpeak events";
    }
    return "";
}

auto contextualize_error(const ParsedCommand& command, domain::Error error) -> domain::Error {
    const std::string path = util::join(command.path, " ");
    if (!path.empty()) {
        error.details.emplace("command", path);
    }

    const auto session_action = session_action_for_path(path);
    if ((error.code == "functions_unavailable" || error.code == "not_initialized") &&
        !session_action.empty()) {
        error.message = "Unable to " + std::string(session_action) +
                        " because the TeamSpeak client is not running or the ts3cli plugin is unavailable.";
    }
    if (error.code == "not_connected" && !session_action.empty()) {
        error.message =
            "Unable to " + std::string(session_action) + " because there is no active TeamSpeak server connection.";
        add_error_hint(error, "Run `ts connect` to join the configured TeamSpeak server.");
        add_error_hint(error, "Run `ts status` to confirm the connection state before retrying.");
    }

    if (error.code == "socket_connect_failed" || error.code == "functions_unavailable" ||
        error.code == "not_initialized") {
        add_error_hint(error, "Run `ts client start` to launch the local TeamSpeak client.");
        add_error_hint(error, "Run `ts plugin info` to verify the ts3cli plugin bridge is available.");
    }

    if (error.code == "socket_timeout") {
        add_error_hint(error, "Run `ts plugin info` to check whether the ts3cli plugin bridge is responsive.");
        add_error_hint(error, "Restart the TeamSpeak client if the plugin bridge appears stuck.");
    }

    if (path == "playback send" && error.category == "media") {
        add_error_hint(error, "Run `ts plugin info` to verify the media socket path and accepted playback format.");
    }

    if (path == "playback send" && error.code == "client_busy") {
        add_error_hint(error, "Stop the other media socket consumer before retrying playback.");
    }

    if (path == "playback send" && error.code == "unsupported_audio_format") {
        add_error_hint(error, "Use a WAV file encoded as pcm_s16le at 48000 Hz mono.");
    }

    if (error.code == "channel_not_found") {
        add_error_hint(error, "Run `ts channel list` to see available channel IDs and names.");
    }

    if (error.code == "client_not_found") {
        add_error_hint(error, "Run `ts client list` to see available client IDs and nicknames.");
    }

    if (error.code == "profile_not_found") {
        add_error_hint(error, "Run `ts profile list` to see the profiles available in the selected config.");
    }

    if (error.code == "profile_exists") {
        add_error_hint(error, "Run `ts profile list` to inspect the existing profiles in the selected config.");
        add_error_hint(error, "Run `ts profile use <name>` if you meant to switch to the existing profile.");
    }

    if (error.code == "missing_config") {
        add_error_hint(error, "Run `ts config init` to create a starter config file.");
    }

    if (error.code == "unknown_backend") {
        add_error_hint(error, "Run `ts config view` to inspect the backend configured for the active profile.");
    }

    if (path == "connect" && error.code == "missing_identity") {
        add_error_hint(error, "Run `ts config view` to inspect the active profile before retrying `ts connect`.");
    }

    if (path == "client stop" && error.code == "stop_timeout") {
        add_error_hint(error, "Run `ts client stop --force` to send SIGKILL to the tracked TeamSpeak process.");
        add_error_hint(error, "Run `ts client status` afterward to confirm the process is gone.");
    }

    if (path == "client start" && error.code == "launcher_not_found") {
        add_error_hint(error, "Install the TeamSpeak client or set `TS_CLIENT_LAUNCHER`, then rerun `ts client start`.");
    }

    if (path == "client start" && error.code == "missing_runtime_library") {
        std::string missing_library_hint =
            "Install a package that provides the missing shared library, or add it under the TeamSpeak client `runtime-libs` directory, then rerun `ts client start`.";
        if (const auto client_dir = error.details.find("client_dir"); client_dir != error.details.end()) {
            missing_library_hint =
                "Install a package that provides the missing shared library, or add it under `" +
                client_dir->second +
                "/runtime-libs`, then rerun `ts client start`.";
        }
        add_error_hint(error, std::move(missing_library_hint));
        add_error_hint(
            error,
            "If you installed the bundled TeamSpeak client through this project, rerun the installer to refresh the managed runtime libraries."
        );
    }

    if (path == "client start" &&
        (error.code == "xvfb_not_found" || error.code == "invalid_xvfb" || error.code == "display_not_found" ||
         error.code == "invalid_display")) {
        add_error_hint(error, "Install Xvfb or set `TS_CLIENT_HEADLESS=0`, then rerun `ts client start`.");
    }

    if (path == "client start" &&
        (error.code == "systemd_unavailable" || error.code == "systemd_user_unavailable" ||
         error.code == "systemd_launch_failed")) {
        add_error_hint(
            error,
            "Set `TS_CLIENT_SYSTEMD_RUN=0` to force the legacy detached launcher path, then rerun `ts client start`."
        );
        add_error_hint(
            error,
            "If this launch needs to survive a non-interactive session teardown, verify that `systemd-run --user` works for the current user."
        );
    }

    if (path == "update" && error.code == "missing_install_receipt") {
        add_error_hint(
            error,
            "Run the official release installer first so `ts update` can preserve the installed paths."
        );
    }

    if (path == "update" && error.code == "download_failed") {
        add_error_hint(error, "Check network access to GitHub, then rerun `ts update`.");
    }

    if (path == "update" && error.code == "installer_failed") {
        add_error_hint(error, "Rerun `ts update --debug` or the official install-release.sh script to inspect the installer failure.");
    }

    if (path == "daemon start" && error.code == "already_running") {
        add_error_hint(error, "Run `ts daemon status` to inspect the existing TeamSpeak event daemon.");
    }

    if (path == "events hook remove" && error.code == "hook_not_found") {
        add_error_hint(error, "Run `ts events hook list` to inspect the currently installed daemon hooks.");
    }

    if (error.exit_code == domain::ExitCode::usage) {
        add_error_hint(error, "Run `" + help_command_for_path(path) + "` to review usage.");
    }

    return error;
}

struct ResolvedProfile {
    std::filesystem::path config_path;
    domain::AppConfig config;
    domain::Profile profile;
};

struct ClientProcessPaths {
    std::filesystem::path launcher_path;
    std::filesystem::path state_dir;
    std::filesystem::path pid_file;
    std::filesystem::path log_file;
    std::filesystem::path unit_file;
    std::filesystem::path headless_script_file;
};

struct ClientStatePaths {
    std::filesystem::path state_dir;
    std::filesystem::path pid_file;
    std::filesystem::path log_file;
    std::filesystem::path unit_file;
    std::filesystem::path headless_script_file;
};

struct ClientHeadlessLaunch {
    std::filesystem::path xvfb_path;
    std::string xvfb_library_path;
    std::filesystem::path xvfb_xkb_dir;
    std::filesystem::path xvfb_binary_dir;
    std::string display;
};

struct XdotoolPaths {
    std::filesystem::path binary_path;
    std::string library_path;
};

struct DiscoveredClientProcess {
    pid_t pid = 0;
    std::string process_name;
    std::size_t match_count = 0;
};

struct ChannelClientGroup {
    domain::Channel channel;
    std::vector<domain::Client> clients;
};

struct ClientLogSource {
    std::string kind;
    std::filesystem::path path;
    std::vector<std::string> lines;
    std::string error;
};

enum class ClientSystemdRunMode {
    disabled,
    auto_detect,
    enabled,
};

struct ClientSystemdPaths {
    std::filesystem::path systemd_run_path;
    std::filesystem::path systemctl_path;
};

struct ClientSystemdUnitStatus {
    std::string active_state;
    std::string sub_state;
    pid_t main_pid = 0;
};

struct ClientLaunchResult {
    pid_t pid = 0;
    std::string note;
};

struct UpdateInstallPlan {
    std::filesystem::path receipt_path;
    std::filesystem::path prefix;
    std::filesystem::path client_install_dir;
    std::filesystem::path managed_dir;
    std::filesystem::path config_path;
    std::optional<std::string> release_tag;
    std::string release_repo = std::string(kOfficialReleaseRepo);
    bool skip_config = false;
};

struct UpdateInstaller {
    std::filesystem::path path;
    std::string source;
    bool temporary = false;
};

enum class ProcessStdoutMode {
    inherit,
    to_stderr,
};

auto resolve_client_launch_socket_path(
    const ParsedCommand& command,
    const config::ConfigStore& store
) -> domain::Result<std::string>;
auto process_is_running(pid_t pid) -> bool;

auto is_executable_file(const std::filesystem::path& path) -> bool {
    return !path.empty() && ::access(path.c_str(), X_OK) == 0;
}

auto current_executable_path() -> std::optional<std::filesystem::path> {
    std::array<char, 4096> buffer{};
    const auto bytes = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (bytes <= 0) {
        return std::nullopt;
    }
    buffer[static_cast<std::size_t>(bytes)] = '\0';
    return std::filesystem::path(buffer.data());
}

auto find_executable_on_path(std::string_view executable_name) -> std::optional<std::filesystem::path> {
    const char* path_env = std::getenv("PATH");
    if (path_env == nullptr || *path_env == '\0') {
        return std::nullopt;
    }

    for (const auto& entry : util::split(path_env, ':')) {
        const auto candidate =
            (entry.empty() ? std::filesystem::current_path() : std::filesystem::path(entry)) /
            std::string(executable_name);
        if (is_executable_file(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

auto find_executable(
    std::string_view executable_name,
    std::span<const std::filesystem::path> fallback_paths = {}
) -> std::optional<std::filesystem::path> {
    if (const auto discovered = find_executable_on_path(executable_name); discovered.has_value()) {
        return discovered;
    }

    for (const auto& fallback_path : fallback_paths) {
        if (is_executable_file(fallback_path)) {
            return fallback_path;
        }
    }

    return std::nullopt;
}

auto resolve_ldconfig_path() -> std::optional<std::filesystem::path> {
    if (const char* explicit_ldconfig = std::getenv("TS3_CLIENT_LDCONFIG");
        explicit_ldconfig != nullptr && *explicit_ldconfig != '\0') {
        const std::filesystem::path explicit_path = explicit_ldconfig;
        if (is_executable_file(explicit_path)) {
            return explicit_path;
        }
    }

    return find_executable("ldconfig", kLdconfigFallbackPaths);
}

auto default_teamspeak_cache_dir() -> std::filesystem::path {
    if (const char* xdg_cache = std::getenv("XDG_CACHE_HOME"); xdg_cache != nullptr && *xdg_cache != '\0') {
        return std::filesystem::path(xdg_cache) / "teamspeak-cli" / "install";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".cache" / "teamspeak-cli" / "install";
    }
    return {};
}

auto installed_share_dir() -> std::optional<std::filesystem::path> {
    const auto current = current_executable_path();
    if (!current.has_value()) {
        return std::nullopt;
    }

    const auto candidate = current->parent_path().parent_path() / "share" / "teamspeak-cli";
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec) && !ec) {
        return candidate;
    }
    return std::nullopt;
}

auto is_shell_escape_character(char ch) -> bool {
    const auto unsigned_ch = static_cast<unsigned char>(ch);
    return std::isspace(unsigned_ch) != 0 || std::ispunct(unsigned_ch) != 0;
}

auto looks_shell_escaped(std::string_view value) -> bool {
    if (value.rfind("$'", 0) == 0) {
        return true;
    }

    for (std::size_t index = 0; index + 1 < value.size(); ++index) {
        if (value[index] == '\\' && is_shell_escape_character(value[index + 1])) {
            return true;
        }
    }

    return false;
}

auto decode_ansi_c_escape(char escape) -> std::optional<char> {
    switch (escape) {
        case '\\':
        case '\'':
        case '"':
        case '?':
            return escape;
        case 'a':
            return '\a';
        case 'b':
            return '\b';
        case 'e':
        case 'E':
            return '\x1b';
        case 'f':
            return '\f';
        case 'n':
            return '\n';
        case 'r':
            return '\r';
        case 't':
            return '\t';
        case 'v':
            return '\v';
        default:
            return std::nullopt;
    }
}

auto decode_install_receipt_value(std::string_view value) -> std::optional<std::string> {
    if (!looks_shell_escaped(value)) {
        return std::nullopt;
    }

    std::string decoded;
    decoded.reserve(value.size());

    enum class Mode {
        plain,
        single_quoted,
        double_quoted,
        ansi_c_quoted,
    };

    auto decode_hex_escape = [&](std::size_t& index) -> std::optional<char> {
        if (index >= value.size() || value[index] != 'x') {
            return std::nullopt;
        }

        std::size_t cursor = index + 1;
        unsigned int parsed = 0;
        std::size_t digits = 0;
        while (cursor < value.size() && digits < 2) {
            const unsigned char ch = static_cast<unsigned char>(value[cursor]);
            if (!std::isxdigit(ch)) {
                break;
            }
            parsed <<= 4;
            if (std::isdigit(ch) != 0) {
                parsed |= static_cast<unsigned int>(ch - '0');
            } else {
                parsed |= static_cast<unsigned int>(std::tolower(ch) - 'a' + 10);
            }
            ++cursor;
            ++digits;
        }

        if (digits == 0) {
            return std::nullopt;
        }

        index = cursor - 1;
        return static_cast<char>(parsed);
    };

    auto decode_octal_escape = [&](std::size_t& index, char first_digit) -> std::optional<char> {
        if (first_digit < '0' || first_digit > '7') {
            return std::nullopt;
        }

        unsigned int parsed = static_cast<unsigned int>(first_digit - '0');
        std::size_t cursor = index + 1;
        std::size_t digits = 1;
        while (cursor < value.size() && digits < 3) {
            const char ch = value[cursor];
            if (ch < '0' || ch > '7') {
                break;
            }
            parsed = (parsed << 3) | static_cast<unsigned int>(ch - '0');
            ++cursor;
            ++digits;
        }

        index = cursor - 1;
        return static_cast<char>(parsed & 0xffU);
    };

    Mode mode = Mode::plain;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char ch = value[index];

        switch (mode) {
            case Mode::plain:
                if (ch == '\\') {
                    if (index + 1 >= value.size()) {
                        return std::nullopt;
                    }
                    const char next = value[++index];
                    if (const auto decoded_escape = decode_ansi_c_escape(next); decoded_escape.has_value()) {
                        decoded.push_back(*decoded_escape);
                    } else if (const auto decoded_octal = decode_octal_escape(index, next);
                               decoded_octal.has_value()) {
                        decoded.push_back(*decoded_octal);
                    } else {
                        decoded.push_back(next);
                    }
                } else if (ch == '\'') {
                    mode = Mode::single_quoted;
                } else if (ch == '"') {
                    mode = Mode::double_quoted;
                } else if (ch == '$' && index + 1 < value.size() && value[index + 1] == '\'') {
                    mode = Mode::ansi_c_quoted;
                    ++index;
                } else {
                    decoded.push_back(ch);
                }
                break;
            case Mode::single_quoted:
                if (ch == '\'') {
                    mode = Mode::plain;
                } else {
                    decoded.push_back(ch);
                }
                break;
            case Mode::double_quoted:
                if (ch == '"') {
                    mode = Mode::plain;
                } else if (ch == '\\') {
                    if (index + 1 >= value.size()) {
                        return std::nullopt;
                    }
                    const char next = value[++index];
                    if (next == '\n') {
                        continue;
                    }
                    if (next == '\\' || next == '"' || next == '$' || next == '`') {
                        decoded.push_back(next);
                    } else {
                        decoded.push_back(next);
                    }
                } else {
                    decoded.push_back(ch);
                }
                break;
            case Mode::ansi_c_quoted:
                if (ch == '\'') {
                    mode = Mode::plain;
                    break;
                }
                if (ch != '\\') {
                    decoded.push_back(ch);
                    break;
                }
                if (index + 1 >= value.size()) {
                    return std::nullopt;
                }
                {
                    const char next = value[++index];
                    if (const auto decoded_escape = decode_ansi_c_escape(next); decoded_escape.has_value()) {
                        decoded.push_back(*decoded_escape);
                    } else if (const auto decoded_hex = decode_hex_escape(index); decoded_hex.has_value()) {
                        decoded.push_back(*decoded_hex);
                    } else if (const auto decoded_octal = decode_octal_escape(index, next);
                               decoded_octal.has_value()) {
                        decoded.push_back(*decoded_octal);
                    } else {
                        decoded.push_back(next);
                    }
                }
                break;
        }
    }

    if (mode != Mode::plain) {
        return std::nullopt;
    }

    return decoded;
}

auto read_install_receipt_value_from_path(
    const std::filesystem::path& receipt_path,
    std::string_view key
) -> std::optional<std::string> {
    std::ifstream input(receipt_path);
    if (!input) {
        return std::nullopt;
    }

    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        const auto line_key = util::trim(line.substr(0, separator));
        if (line_key != key) {
            continue;
        }
        const auto value = util::trim(line.substr(separator + 1));
        if (const auto decoded = decode_install_receipt_value(value); decoded.has_value()) {
            return decoded;
        }
        return value;
    }
    return std::nullopt;
}

auto installed_receipt_path() -> std::optional<std::filesystem::path> {
    const auto share_dir = installed_share_dir();
    if (!share_dir.has_value()) {
        return std::nullopt;
    }
    return *share_dir / "install-receipt.env";
}

auto read_install_receipt_value(std::string_view key) -> std::optional<std::string> {
    const auto receipt_path = installed_receipt_path();
    if (!receipt_path.has_value()) {
        return std::nullopt;
    }
    return read_install_receipt_value_from_path(*receipt_path, key);
}

auto run_process_wait(
    const std::vector<std::string>& argv,
    ProcessStdoutMode stdout_mode = ProcessStdoutMode::inherit
) -> domain::Result<int> {
    if (argv.empty()) {
        return domain::fail<int>(update_error(
            "invalid_process",
            "cannot run an empty command",
            domain::ExitCode::internal
        ));
    }

    const pid_t child_pid = ::fork();
    if (child_pid < 0) {
        return domain::fail<int>(update_error(
            "fork_failed",
            "failed to fork subprocess: " + std::string(std::strerror(errno)),
            domain::ExitCode::internal
        ));
    }

    if (child_pid == 0) {
        if (stdout_mode == ProcessStdoutMode::to_stderr &&
            ::dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            _exit(127);
        }

        std::vector<char*> raw_argv;
        raw_argv.reserve(argv.size() + 1);
        for (const auto& argument : argv) {
            raw_argv.push_back(const_cast<char*>(argument.c_str()));
        }
        raw_argv.push_back(nullptr);

        ::execv(argv.front().c_str(), raw_argv.data());
        _exit(127);
    }

    int status = 0;
    while (::waitpid(child_pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return domain::fail<int>(update_error(
                "wait_failed",
                "failed to wait for subprocess: " + std::string(std::strerror(errno)),
                domain::ExitCode::internal
            ));
        }
    }

    if (WIFEXITED(status)) {
        return domain::ok(WEXITSTATUS(status));
    }
    if (WIFSIGNALED(status)) {
        return domain::ok(128 + WTERMSIG(status));
    }
    return domain::ok(127);
}

auto resolve_update_receipt_path() -> domain::Result<std::filesystem::path> {
    if (const char* explicit_receipt = std::getenv("TS_UPDATE_RECEIPT_PATH");
        explicit_receipt != nullptr && *explicit_receipt != '\0') {
        const std::filesystem::path receipt_path = explicit_receipt;
        std::error_code exists_ec;
        if (std::filesystem::exists(receipt_path, exists_ec) && !exists_ec) {
            return domain::ok(receipt_path);
        }
        return domain::fail<std::filesystem::path>(update_error(
            "missing_install_receipt",
            "TS_UPDATE_RECEIPT_PATH does not exist: " + receipt_path.string(),
            domain::ExitCode::not_found
        ));
    }

    const auto receipt_path = installed_receipt_path();
    if (!receipt_path.has_value()) {
        return domain::fail<std::filesystem::path>(update_error(
            "missing_install_receipt",
            "could not locate an install receipt for this ts binary",
            domain::ExitCode::not_found
        ));
    }

    std::error_code exists_ec;
    if (!std::filesystem::exists(*receipt_path, exists_ec) || exists_ec) {
        return domain::fail<std::filesystem::path>(update_error(
            "missing_install_receipt",
            "install receipt was not found at " + receipt_path->string(),
            domain::ExitCode::not_found
        ));
    }
    return domain::ok(*receipt_path);
}

auto require_update_receipt_value(const std::filesystem::path& receipt_path, std::string_view key)
    -> domain::Result<std::string> {
    const auto value = read_install_receipt_value_from_path(receipt_path, key);
    if (!value.has_value() || value->empty()) {
        return domain::fail<std::string>(update_error(
            "invalid_install_receipt",
            "install receipt is missing required value: " + std::string(key),
            domain::ExitCode::config
        ));
    }
    return domain::ok(*value);
}

auto resolve_update_install_plan(const ParsedCommand& command) -> domain::Result<UpdateInstallPlan> {
    if (!command.positionals.empty()) {
        return domain::fail<UpdateInstallPlan>(cli_error(
            "unexpected_argument",
            "update does not accept positional arguments"
        ));
    }

    auto receipt_path = resolve_update_receipt_path();
    if (!receipt_path) {
        return domain::fail<UpdateInstallPlan>(receipt_path.error());
    }

    auto prefix = require_update_receipt_value(receipt_path.value(), "prefix");
    if (!prefix) {
        return domain::fail<UpdateInstallPlan>(prefix.error());
    }
    auto client_install_dir = require_update_receipt_value(receipt_path.value(), "client_install_dir");
    if (!client_install_dir) {
        return domain::fail<UpdateInstallPlan>(client_install_dir.error());
    }
    auto managed_dir = require_update_receipt_value(receipt_path.value(), "managed_dir");
    if (!managed_dir) {
        return domain::fail<UpdateInstallPlan>(managed_dir.error());
    }
    auto config_path = require_update_receipt_value(receipt_path.value(), "config_path");
    if (!config_path) {
        return domain::fail<UpdateInstallPlan>(config_path.error());
    }
    const bool skip_config =
        read_install_receipt_value_from_path(receipt_path.value(), "config_created_by_installer")
            .value_or("1") != "1";

    std::optional<std::string> release_tag;
    if (const auto tag = command.options.find("release-tag"); tag != command.options.end()) {
        if (tag->second.empty()) {
            return domain::fail<UpdateInstallPlan>(cli_error(
                "missing_option_value",
                "missing value for --release-tag"
            ));
        }
        release_tag = tag->second;
    }

    return domain::ok(UpdateInstallPlan{
        .receipt_path = receipt_path.value(),
        .prefix = prefix.value(),
        .client_install_dir = client_install_dir.value(),
        .managed_dir = managed_dir.value(),
        .config_path = config_path.value(),
        .release_tag = std::move(release_tag),
        .release_repo = std::string(kOfficialReleaseRepo),
        .skip_config = skip_config,
    });
}

auto make_update_temp_file() -> domain::Result<std::filesystem::path> {
    std::error_code temp_ec;
    const auto temp_dir = std::filesystem::temp_directory_path(temp_ec);
    if (temp_ec) {
        return domain::fail<std::filesystem::path>(update_error(
            "temp_dir_failed",
            "failed to resolve a temporary directory: " + temp_ec.message(),
            domain::ExitCode::internal
        ));
    }

    std::string path_template = (temp_dir / "teamspeak-cli-update-installer.XXXXXX").string();
    std::vector<char> path_buffer(path_template.begin(), path_template.end());
    path_buffer.push_back('\0');

    const int fd = ::mkstemp(path_buffer.data());
    if (fd < 0) {
        return domain::fail<std::filesystem::path>(update_error(
            "temp_file_failed",
            "failed to create a temporary installer file: " + std::string(std::strerror(errno)),
            domain::ExitCode::internal
        ));
    }
    ::close(fd);

    return domain::ok(std::filesystem::path(path_buffer.data()));
}

auto download_update_installer(const std::filesystem::path& destination) -> domain::Result<void> {
    std::vector<std::string> argv;
    const std::string url(kOfficialInstallReleaseUrl);
    const std::string token = [] {
        if (const char* gh_token = std::getenv("GH_TOKEN"); gh_token != nullptr && *gh_token != '\0') {
            return std::string(gh_token);
        }
        if (const char* github_token = std::getenv("GITHUB_TOKEN");
            github_token != nullptr && *github_token != '\0') {
            return std::string(github_token);
        }
        return std::string{};
    }();

    if (const auto curl = find_executable_on_path("curl"); curl.has_value()) {
        argv = {
            curl->string(),
            "--fail",
            "--location",
            "--silent",
            "--show-error",
        };
        if (!token.empty()) {
            argv.push_back("-H");
            argv.push_back("Authorization: Bearer " + token);
        }
        argv.push_back("--output");
        argv.push_back(destination.string());
        argv.push_back(url);
    } else if (const auto wget = find_executable_on_path("wget"); wget.has_value()) {
        argv = {wget->string(), "-qO", destination.string()};
        if (!token.empty()) {
            argv.push_back("--header=Authorization: Bearer " + token);
        }
        argv.push_back(url);
    } else {
        return domain::fail(update_error(
            "downloader_not_found",
            "curl or wget is required to download the release installer",
            domain::ExitCode::not_found
        ));
    }

    const auto status = run_process_wait(argv, ProcessStdoutMode::to_stderr);
    if (!status) {
        return domain::fail(status.error());
    }
    if (status.value() != 0) {
        auto error = update_error(
            "download_failed",
            "failed to download the release installer from " + url,
            domain::ExitCode::connection
        );
        error.details.emplace("exit_status", std::to_string(status.value()));
        error.details.emplace("url", url);
        return domain::fail(std::move(error));
    }

    std::error_code size_ec;
    const auto bytes = std::filesystem::file_size(destination, size_ec);
    if (size_ec || bytes == 0) {
        return domain::fail(update_error(
            "download_failed",
            "downloaded release installer is empty or unreadable: " + destination.string(),
            domain::ExitCode::connection
        ));
    }
    return domain::ok();
}

auto resolve_update_installer(const CommandRouter::ProgressSink& progress) -> domain::Result<UpdateInstaller> {
    if (const char* explicit_installer = std::getenv("TS_UPDATE_INSTALLER_PATH");
        explicit_installer != nullptr && *explicit_installer != '\0') {
        const std::filesystem::path installer_path = explicit_installer;
        std::error_code exists_ec;
        if (!std::filesystem::exists(installer_path, exists_ec) || exists_ec) {
            return domain::fail<UpdateInstaller>(update_error(
                "installer_not_found",
                "TS_UPDATE_INSTALLER_PATH does not exist: " + installer_path.string(),
                domain::ExitCode::not_found
            ));
        }
        if (progress) {
            progress("Using release installer " + installer_path.string() + ".");
        }
        return domain::ok(UpdateInstaller{
            .path = installer_path,
            .source = installer_path.string(),
            .temporary = false,
        });
    }

    auto installer_path = make_update_temp_file();
    if (!installer_path) {
        return domain::fail<UpdateInstaller>(installer_path.error());
    }

    if (progress) {
        progress("Downloading release installer from " + std::string(kOfficialInstallReleaseUrl) + ".");
    }
    auto downloaded = download_update_installer(installer_path.value());
    if (!downloaded) {
        std::error_code remove_ec;
        std::filesystem::remove(installer_path.value(), remove_ec);
        return domain::fail<UpdateInstaller>(downloaded.error());
    }

    return domain::ok(UpdateInstaller{
        .path = installer_path.value(),
        .source = std::string(kOfficialInstallReleaseUrl),
        .temporary = true,
    });
}

auto run_update_installer(
    const UpdateInstallPlan& plan,
    const UpdateInstaller& installer,
    ProcessStdoutMode stdout_mode
) -> domain::Result<void> {
    const std::array<std::filesystem::path, 2> bash_fallback_paths = {"/usr/bin/bash", "/bin/bash"};
    const auto bash_path = find_executable("bash", bash_fallback_paths);
    if (!bash_path.has_value()) {
        return domain::fail(update_error(
            "bash_not_found",
            "bash is required to run the release installer",
            domain::ExitCode::not_found
        ));
    }

    std::vector<std::string> argv{
        bash_path->string(),
        installer.path.string(),
        "--repo",
        plan.release_repo,
        "--prefix",
        plan.prefix.string(),
        "--client-dir",
        plan.client_install_dir.string(),
        "--managed-dir",
        plan.managed_dir.string(),
        "--config-path",
        plan.config_path.string(),
    };
    if (plan.release_tag.has_value()) {
        argv.push_back("--release-tag");
        argv.push_back(*plan.release_tag);
    }
    if (plan.skip_config) {
        argv.push_back("--skip-config");
    }

    const auto status = run_process_wait(argv, stdout_mode);
    if (!status) {
        return domain::fail(status.error());
    }
    if (status.value() != 0) {
        auto error = update_error(
            "installer_failed",
            "release installer exited with status " + std::to_string(status.value()),
            domain::ExitCode::internal
        );
        error.details.emplace("exit_status", std::to_string(status.value()));
        error.details.emplace("installer", installer.path.string());
        error.details.emplace("release_repo", plan.release_repo);
        return domain::fail(std::move(error));
    }

    return domain::ok();
}

auto update_current_release_install(
    const ParsedCommand& command,
    const CommandRouter::ProgressSink& progress
) -> domain::Result<output::CommandOutput> {
    auto plan = resolve_update_install_plan(command);
    if (!plan) {
        return domain::fail<output::CommandOutput>(plan.error());
    }

    if (progress) {
        progress("Updating teamspeak-cli from " + plan.value().release_repo + ".");
        progress("Using install receipt " + plan.value().receipt_path.string() + ".");
    }

    auto installer = resolve_update_installer(progress);
    if (!installer) {
        return domain::fail<output::CommandOutput>(installer.error());
    }
    auto installer_cleanup = util::make_scope_exit([&] {
        if (installer.value().temporary) {
            std::error_code remove_ec;
            std::filesystem::remove(installer.value().path, remove_ec);
        }
    });

    if (progress) {
        progress("Running the release installer. This may refresh the TeamSpeak client bundle and plugin.");
    }
    auto ran = run_update_installer(
        plan.value(),
        installer.value(),
        command.global.format == output::Format::table ? ProcessStdoutMode::inherit
                                                       : ProcessStdoutMode::to_stderr
    );
    if (!ran) {
        return domain::fail<output::CommandOutput>(ran.error());
    }

    const std::string release_tag =
        read_install_receipt_value_from_path(plan.value().receipt_path, "release_tag")
            .value_or(plan.value().release_tag.value_or("latest"));

    return domain::ok(output::CommandOutput{
        .data = output::make_object({
            {"result", output::make_string("updated")},
            {"release_repo", output::make_string(plan.value().release_repo)},
            {"release_tag", output::make_string(release_tag)},
            {"prefix", output::make_string(plan.value().prefix.string())},
            {"client_dir", output::make_string(plan.value().client_install_dir.string())},
            {"managed_dir", output::make_string(plan.value().managed_dir.string())},
            {"config_path", output::make_string(plan.value().config_path.string())},
            {"receipt_path", output::make_string(plan.value().receipt_path.string())},
            {"installer_source", output::make_string(installer.value().source)},
            {"skip_config", output::make_bool(plan.value().skip_config)},
        }),
        .human = std::string(
            "Updated teamspeak-cli to " + release_tag + " from " + plan.value().release_repo +
            ".\nRestart TeamSpeak to load the refreshed plugin."
        ),
    });
}

auto resolve_xdotool_paths() -> std::optional<XdotoolPaths> {
    std::filesystem::path binary_path;
    if (const char* explicit_xdotool = std::getenv("TS_CLIENT_XDOTOOL");
        explicit_xdotool != nullptr && *explicit_xdotool != '\0') {
        binary_path = explicit_xdotool;
    } else if (const auto discovered = find_executable_on_path("xdotool"); discovered.has_value()) {
        binary_path = *discovered;
    } else {
        auto managed_dir = default_teamspeak_cache_dir();
        if (const auto receipt_managed_dir = read_install_receipt_value("managed_dir");
            receipt_managed_dir.has_value()) {
            managed_dir = *receipt_managed_dir;
        }
        if (!managed_dir.empty()) {
            const auto managed_xdotool = managed_dir / "xdotool" / "root" / "usr" / "bin" / "xdotool";
            if (is_executable_file(managed_xdotool)) {
                binary_path = managed_xdotool;
            }
        }
    }

    if (!is_executable_file(binary_path)) {
        return std::nullopt;
    }

    std::string library_path;
    if (const char* explicit_library = std::getenv("TS_CLIENT_XDOTOOL_LIBRARY_PATH");
        explicit_library != nullptr && *explicit_library != '\0') {
        library_path = explicit_library;
    } else {
        const auto search_root = binary_path.parent_path().parent_path();
        std::error_code iterator_ec;
        for (std::filesystem::recursive_directory_iterator iter(
                 search_root,
                 std::filesystem::directory_options::skip_permission_denied,
                 iterator_ec
             ),
             end;
             iter != end;
             iter.increment(iterator_ec)) {
            if (iterator_ec) {
                iterator_ec.clear();
                continue;
            }
            if (!iter->is_regular_file(iterator_ec)) {
                if (iterator_ec) {
                    iterator_ec.clear();
                }
                continue;
            }
            const auto name = iter->path().filename().string();
            if (name.rfind("libxdo.so", 0) == 0) {
                library_path = iter->path().parent_path().string();
                break;
            }
        }
    }

    return XdotoolPaths{
        .binary_path = std::move(binary_path),
        .library_path = std::move(library_path),
    };
}

auto managed_xvfb_root_for_path(const std::filesystem::path& xvfb_path) -> std::optional<std::filesystem::path> {
    const auto bin_dir = xvfb_path.parent_path();
    if (bin_dir.filename() != "bin") {
        return std::nullopt;
    }

    const auto usr_dir = bin_dir.parent_path();
    if (usr_dir.filename() != "usr") {
        return std::nullopt;
    }

    const auto root_dir = usr_dir.parent_path();
    if (root_dir.empty() || root_dir.filename() != "root") {
        return std::nullopt;
    }

    const auto package_dir = root_dir.parent_path();
    if (package_dir.filename() != "xvfb") {
        return std::nullopt;
    }

    return root_dir;
}

auto library_path_for_root(const std::filesystem::path& root_dir) -> std::string {
    std::vector<std::filesystem::path> library_dirs;
    std::error_code iterator_ec;
    for (std::filesystem::recursive_directory_iterator iter(
             root_dir,
             std::filesystem::directory_options::skip_permission_denied,
             iterator_ec
         ),
         end;
         iter != end;
         iter.increment(iterator_ec)) {
        if (iterator_ec) {
            iterator_ec.clear();
            continue;
        }
        std::error_code type_ec;
        if (!iter->is_regular_file(type_ec)) {
            if (type_ec) {
                type_ec.clear();
            }
            continue;
        }
        const auto name = iter->path().filename().string();
        if (name.find(".so") == std::string::npos) {
            continue;
        }
        const auto library_dir = iter->path().parent_path();
        if (std::find(library_dirs.begin(), library_dirs.end(), library_dir) == library_dirs.end()) {
            library_dirs.push_back(library_dir);
        }
    }

    std::vector<std::string> entries;
    entries.reserve(library_dirs.size());
    for (const auto& dir : library_dirs) {
        entries.push_back(dir.string());
    }
    return util::join(entries, ":");
}

auto path_from_env_or_receipt(std::string_view env_name, std::string_view receipt_key) -> std::filesystem::path {
    const std::string env_key(env_name);
    if (const char* explicit_path = std::getenv(env_key.c_str());
        explicit_path != nullptr && *explicit_path != '\0') {
        return explicit_path;
    }
    if (const auto receipt_value = read_install_receipt_value(receipt_key); receipt_value.has_value()) {
        return *receipt_value;
    }
    return {};
}

auto string_from_env_or_receipt(std::string_view env_name, std::string_view receipt_key) -> std::string {
    const std::string env_key(env_name);
    if (const char* explicit_value = std::getenv(env_key.c_str());
        explicit_value != nullptr && *explicit_value != '\0') {
        return explicit_value;
    }
    return read_install_receipt_value(receipt_key).value_or("");
}

auto resolve_xvfb_paths() -> domain::Result<ClientHeadlessLaunch> {
    std::filesystem::path xvfb_path;

    if (const char* explicit_xvfb = std::getenv("TS_CLIENT_XVFB");
        explicit_xvfb != nullptr && *explicit_xvfb != '\0') {
        xvfb_path = explicit_xvfb;
        if (!is_executable_file(xvfb_path)) {
            return domain::fail<ClientHeadlessLaunch>(client_error(
                "invalid_xvfb",
                "TS_CLIENT_XVFB is not executable: " + xvfb_path.string(),
                domain::ExitCode::not_found
            ));
        }
    } else if (const auto found = find_executable_on_path("Xvfb"); found.has_value()) {
        xvfb_path = *found;
    } else if (auto receipt_xvfb = path_from_env_or_receipt("TS_CLIENT_XVFB", "xvfb_bin_path");
               is_executable_file(receipt_xvfb)) {
        xvfb_path = std::move(receipt_xvfb);
    } else {
        auto managed_dir = default_teamspeak_cache_dir();
        if (const auto receipt_managed_dir = read_install_receipt_value("managed_dir"); receipt_managed_dir.has_value()) {
            managed_dir = *receipt_managed_dir;
        }
        const auto managed_xvfb = managed_dir / "xvfb" / "root" / "usr" / "bin" / "Xvfb";
        if (is_executable_file(managed_xvfb)) {
            xvfb_path = managed_xvfb;
        }
    }

    if (xvfb_path.empty()) {
        return domain::fail<ClientHeadlessLaunch>(client_error(
            "xvfb_not_found",
            "Xvfb is required to launch the TeamSpeak client headlessly; install Xvfb or set TS_CLIENT_HEADLESS=0",
            domain::ExitCode::not_found
        ));
    }

    auto library_path = string_from_env_or_receipt("TS_CLIENT_XVFB_LIBRARY_PATH", "xvfb_library_path");
    auto xkb_dir = path_from_env_or_receipt("TS_CLIENT_XVFB_XKB_DIR", "xvfb_xkb_dir");
    auto binary_dir = path_from_env_or_receipt("TS_CLIENT_XVFB_BINARY_DIR", "xvfb_binary_dir");

    if (const auto managed_root = managed_xvfb_root_for_path(xvfb_path); managed_root.has_value()) {
        if (library_path.empty()) {
            library_path = library_path_for_root(*managed_root);
        }
        if (xkb_dir.empty()) {
            const auto candidate = *managed_root / "usr" / "share" / "X11" / "xkb";
            std::error_code exists_ec;
            if (std::filesystem::is_directory(candidate, exists_ec) && !exists_ec) {
                xkb_dir = candidate;
            }
        }
        if (binary_dir.empty()) {
            const auto candidate = *managed_root / "usr" / "bin";
            std::error_code exists_ec;
            if (std::filesystem::is_directory(candidate, exists_ec) && !exists_ec) {
                binary_dir = candidate;
            }
        }
    }

    return domain::ok(ClientHeadlessLaunch{
        .xvfb_path = std::move(xvfb_path),
        .xvfb_library_path = std::move(library_path),
        .xvfb_xkb_dir = std::move(xkb_dir),
        .xvfb_binary_dir = std::move(binary_dir),
        .display = {},
    });
}

auto run_command_capture_stdout(
    const std::vector<std::string>& argv,
    const std::vector<std::pair<std::string, std::string>>& env_overrides = {}
) -> std::optional<std::string> {
    if (argv.empty()) {
        return std::nullopt;
    }

    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) {
        return std::nullopt;
    }
    auto close_pipe = util::make_scope_exit([&] {
        if (pipe_fds[0] >= 0) {
            ::close(pipe_fds[0]);
        }
        if (pipe_fds[1] >= 0) {
            ::close(pipe_fds[1]);
        }
    });

    const pid_t child_pid = ::fork();
    if (child_pid < 0) {
        return std::nullopt;
    }

    if (child_pid == 0) {
        ::close(pipe_fds[0]);
        const int devnull_fd = ::open("/dev/null", O_WRONLY);
        if (devnull_fd < 0 || ::dup2(pipe_fds[1], STDOUT_FILENO) == -1 ||
            ::dup2(devnull_fd, STDERR_FILENO) == -1) {
            if (devnull_fd >= 0) {
                ::close(devnull_fd);
            }
            _exit(127);
        }
        ::close(pipe_fds[1]);
        ::close(devnull_fd);

        for (const auto& [key, value] : env_overrides) {
            if (::setenv(key.c_str(), value.c_str(), 1) != 0) {
                _exit(127);
            }
        }

        std::vector<char*> raw_argv;
        raw_argv.reserve(argv.size() + 1);
        for (const auto& argument : argv) {
            raw_argv.push_back(const_cast<char*>(argument.c_str()));
        }
        raw_argv.push_back(nullptr);
        ::execv(argv.front().c_str(), raw_argv.data());
        _exit(127);
    }

    ::close(pipe_fds[1]);
    pipe_fds[1] = -1;

    std::string output;
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t bytes_read = ::read(pipe_fds[0], buffer.data(), buffer.size());
        if (bytes_read > 0) {
            output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
            continue;
        }
        if (bytes_read == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        output.clear();
        break;
    }
    ::close(pipe_fds[0]);
    pipe_fds[0] = -1;

    int status = 0;
    while (::waitpid(child_pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return std::nullopt;
        }
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::nullopt;
    }
    return output;
}

auto split_output_lines(const std::optional<std::string>& output) -> std::vector<std::string> {
    std::vector<std::string> lines;
    if (!output.has_value()) {
        return lines;
    }

    std::istringstream input(*output);
    std::string line;
    while (std::getline(input, line)) {
        const auto trimmed = util::trim(line);
        if (!trimmed.empty()) {
            lines.push_back(trimmed);
        }
    }
    return lines;
}

auto append_unique_path(
    std::vector<std::filesystem::path>& paths,
    const std::filesystem::path& candidate
) -> void {
    if (candidate.empty()) {
        return;
    }

    if (std::find(paths.begin(), paths.end(), candidate) == paths.end()) {
        paths.push_back(candidate);
    }
}

auto append_search_path_entries(
    std::vector<std::filesystem::path>& paths,
    std::string_view raw_path_list
) -> void {
    for (const auto& entry : util::split(raw_path_list, ':')) {
        if (!entry.empty()) {
            append_unique_path(paths, std::filesystem::path(entry));
        }
    }
}

auto append_runtime_library_dirs(
    std::vector<std::filesystem::path>& paths,
    const std::filesystem::path& runtime_root
) -> void {
    std::error_code exists_ec;
    if (runtime_root.empty() || !std::filesystem::exists(runtime_root, exists_ec) || exists_ec) {
        return;
    }

    std::error_code iterator_ec;
    for (std::filesystem::recursive_directory_iterator iter(
             runtime_root,
             std::filesystem::directory_options::skip_permission_denied,
             iterator_ec
         ),
         end;
         iter != end;
         iter.increment(iterator_ec)) {
        if (iterator_ec) {
            iterator_ec.clear();
            continue;
        }

        const auto filename = iter->path().filename().string();
        if (filename.find(".so") != std::string::npos) {
            append_unique_path(paths, iter->path().parent_path());
        }
    }
}

auto has_ldconfig_cache_entry(const std::filesystem::path& ldconfig_path, std::string_view soname) -> bool {
    const auto cache_lines = split_output_lines(run_command_capture_stdout({ldconfig_path.string(), "-p"}, {}));
    for (const auto& line : cache_lines) {
        if (line.size() < soname.size()) {
            continue;
        }
        if (line.compare(0, soname.size(), soname) != 0) {
            continue;
        }
        if (line.size() == soname.size() ||
            std::isspace(static_cast<unsigned char>(line[soname.size()])) != 0) {
            return true;
        }
    }
    return false;
}

auto required_client_launch_libraries_missing(const ClientProcessPaths& paths) -> std::vector<std::string> {
    const auto launcher_name = paths.launcher_path.filename().string();
    if (launcher_name != "ts3client_runscript.sh" && launcher_name != "ts3client_linux_amd64") {
        return {};
    }

    const auto client_dir = paths.launcher_path.parent_path();
    std::vector<std::filesystem::path> search_dirs;
    append_unique_path(search_dirs, client_dir);

    if (const char* explicit_runtime_path = std::getenv("TS3_CLIENT_LIBRARY_PATH");
        explicit_runtime_path != nullptr && *explicit_runtime_path != '\0') {
        append_search_path_entries(search_dirs, explicit_runtime_path);
    } else {
        std::filesystem::path runtime_root = client_dir / "runtime-libs";
        if (const char* explicit_runtime_root = std::getenv("TS3_CLIENT_RUNTIME_ROOT");
            explicit_runtime_root != nullptr && *explicit_runtime_root != '\0') {
            runtime_root = explicit_runtime_root;
        }
        append_runtime_library_dirs(search_dirs, runtime_root);
    }

    if (const char* current_ld_library_path = std::getenv("LD_LIBRARY_PATH");
        current_ld_library_path != nullptr && *current_ld_library_path != '\0') {
        append_search_path_entries(search_dirs, current_ld_library_path);
    }

    const auto ldconfig_path = resolve_ldconfig_path();
    std::vector<std::string> missing_libraries;
    for (const auto soname : kClientRequiredSharedLibraries) {
        bool found_in_search_dirs = false;
        for (const auto& dir : search_dirs) {
            std::error_code ec;
            if (std::filesystem::exists(dir / std::string(soname), ec) && !ec) {
                found_in_search_dirs = true;
                break;
            }
        }

        if (found_in_search_dirs) {
            continue;
        }
        if (ldconfig_path.has_value() && has_ldconfig_cache_entry(*ldconfig_path, soname)) {
            continue;
        }
        if (!ldconfig_path.has_value()) {
            continue;
        }

        missing_libraries.emplace_back(soname);
    }

    return missing_libraries;
}

auto preflight_client_launch_runtime(const ClientProcessPaths& paths) -> domain::Result<void> {
    const auto missing_libraries = required_client_launch_libraries_missing(paths);
    if (missing_libraries.empty()) {
        return domain::ok();
    }

    auto error = client_error(
        "missing_runtime_library",
        "refusing to start the TeamSpeak client because required shared libraries are unavailable: " +
            util::join(missing_libraries, ", "),
        domain::ExitCode::not_found
    );
    error.details.emplace("libraries", util::join(missing_libraries, ", "));
    error.details.emplace("client_dir", paths.launcher_path.parent_path().string());
    error.details.emplace("launcher", paths.launcher_path.string());
    return domain::fail(std::move(error));
}

auto xdotool_env(const XdotoolPaths& xdotool, std::string_view display)
    -> std::vector<std::pair<std::string, std::string>> {
    std::vector<std::pair<std::string, std::string>> env{
        {"DISPLAY", std::string(display)},
    };
    if (!xdotool.library_path.empty()) {
        std::string library_path = xdotool.library_path;
        if (const char* existing = std::getenv("LD_LIBRARY_PATH"); existing != nullptr && *existing != '\0') {
            library_path += ":" + std::string(existing);
        }
        env.emplace_back("LD_LIBRARY_PATH", std::move(library_path));
    }
    return env;
}

auto xdotool_window_ids(const XdotoolPaths& xdotool, std::string_view display, std::string_view title)
    -> std::vector<std::string> {
    return split_output_lines(run_command_capture_stdout(
        {xdotool.binary_path.string(), "search", "--name", std::string(title)},
        xdotool_env(xdotool, display)
    ));
}

auto xdotool_run(const XdotoolPaths& xdotool, std::string_view display, const std::vector<std::string>& args)
    -> void {
    std::vector<std::string> argv;
    argv.reserve(args.size() + 1);
    argv.push_back(xdotool.binary_path.string());
    argv.insert(argv.end(), args.begin(), args.end());
    (void)run_command_capture_stdout(argv, xdotool_env(xdotool, display));
}

auto xdotool_window_size(
    const XdotoolPaths& xdotool,
    std::string_view display,
    std::string_view window_id
) -> std::optional<std::pair<int, int>> {
    const auto geometry_lines = split_output_lines(run_command_capture_stdout(
        {xdotool.binary_path.string(), "getwindowgeometry", "--shell", std::string(window_id)},
        xdotool_env(xdotool, display)
    ));

    int width = 0;
    int height = 0;
    for (const auto& line : geometry_lines) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        const auto key = util::trim(line.substr(0, separator));
        const auto value = util::trim(line.substr(separator + 1));
        const auto parsed = util::parse_u64(value);
        if (!parsed.has_value()) {
            continue;
        }
        if (key == "WIDTH") {
            width = static_cast<int>(*parsed);
        } else if (key == "HEIGHT") {
            height = static_cast<int>(*parsed);
        }
    }

    if (width <= 0 || height <= 0) {
        return std::nullopt;
    }
    return std::make_pair(width, height);
}

void dismiss_headless_client_dialogs(const ClientHeadlessLaunch& headless_launch, pid_t client_pid) {
    const auto xdotool = resolve_xdotool_paths();
    if (!xdotool.has_value()) {
        return;
    }

    const std::array<std::string_view, 5> escape_titles = {
        "Introducing the next generation of TeamSpeak",
        "myTeamSpeak Account",
        "Warning",
        "Identities",
        "Choose Your Nickname",
    };

    for (int attempt = 0; attempt < 25; ++attempt) {
        if (!process_is_running(client_pid)) {
            break;
        }

        bool handled_window = false;
        for (const auto& window_id : xdotool_window_ids(*xdotool, headless_launch.display, "License agreement")) {
            xdotool_run(*xdotool, headless_launch.display, {"key", "--window", window_id, "End"});
            std::this_thread::sleep_for(std::chrono::milliseconds(300));

            int click_x = 573;
            int click_y = 676;
            if (const auto geometry = xdotool_window_size(*xdotool, headless_launch.display, window_id);
                geometry.has_value()) {
                click_x = geometry->first == 740 ? click_x : geometry->first * 77 / 100;
                click_y = geometry->second == 700 ? click_y : geometry->second * 97 / 100;
            }
            xdotool_run(
                *xdotool,
                headless_launch.display,
                {"mousemove", "--window", window_id, std::to_string(click_x), std::to_string(click_y), "click", "1"}
            );
            handled_window = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
        }

        for (const auto title : escape_titles) {
            for (const auto& window_id : xdotool_window_ids(*xdotool, headless_launch.display, title)) {
                xdotool_run(*xdotool, headless_launch.display, {"key", "--window", window_id, "Escape"});
                handled_window = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
            }
        }

        if (!handled_window) {
            break;
        }
    }
}

auto resolve_client_state_dir() -> domain::Result<std::filesystem::path> {
    if (const char* state_dir = std::getenv("TS_CLIENT_STATE_DIR"); state_dir != nullptr && *state_dir != '\0') {
        return domain::ok(std::filesystem::path(state_dir));
    }
    if (const char* xdg_state = std::getenv("XDG_STATE_HOME"); xdg_state != nullptr && *xdg_state != '\0') {
        return domain::ok(std::filesystem::path(xdg_state) / "teamspeak-cli");
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return domain::ok(std::filesystem::path(home) / ".local" / "state" / "teamspeak-cli");
    }
    return domain::fail<std::filesystem::path>(client_error(
        "missing_home",
        "HOME or XDG_STATE_HOME must be set to manage the TeamSpeak client process",
        domain::ExitCode::config
    ));
}

auto resolve_client_state_paths() -> domain::Result<ClientStatePaths> {
    auto state_dir = resolve_client_state_dir();
    if (!state_dir) {
        return domain::fail<ClientStatePaths>(state_dir.error());
    }

    return domain::ok(ClientStatePaths{
        .state_dir = state_dir.value(),
        .pid_file = state_dir.value() / "client.pid",
        .log_file = state_dir.value() / "client.log",
        .unit_file = state_dir.value() / "client.unit",
        .headless_script_file = state_dir.value() / "client-systemd-headless.sh",
    });
}

auto resolve_client_process_paths() -> domain::Result<ClientProcessPaths> {
    std::filesystem::path launcher_path;

    if (const char* explicit_launcher = std::getenv("TS_CLIENT_LAUNCHER");
        explicit_launcher != nullptr && *explicit_launcher != '\0') {
        launcher_path = explicit_launcher;
        if (!is_executable_file(launcher_path)) {
            return domain::fail<ClientProcessPaths>(client_error(
                "invalid_launcher",
                "TS_CLIENT_LAUNCHER is not executable: " + launcher_path.string(),
                domain::ExitCode::not_found
            ));
        }
    }

    if (launcher_path.empty()) {
        if (const auto current = current_executable_path(); current.has_value()) {
            const auto sibling = current->parent_path() / "ts3client";
            if (is_executable_file(sibling)) {
                launcher_path = sibling;
            } else {
                const auto alias = current->parent_path() / "teamspeak3-client";
                if (is_executable_file(alias)) {
                    launcher_path = alias;
                }
            }
        }
    }

    if (launcher_path.empty()) {
        if (const auto found = find_executable_on_path("ts3client"); found.has_value()) {
            launcher_path = *found;
        } else if (const auto alias = find_executable_on_path("teamspeak3-client"); alias.has_value()) {
            launcher_path = *alias;
        }
    }

    if (launcher_path.empty()) {
        if (const char* client_dir = std::getenv("TS3_CLIENT_DIR"); client_dir != nullptr && *client_dir != '\0') {
            const auto runscript = std::filesystem::path(client_dir) / "ts3client_runscript.sh";
            if (is_executable_file(runscript)) {
                launcher_path = runscript;
            }
        }
    }

    if (launcher_path.empty()) {
        return domain::fail<ClientProcessPaths>(client_error(
            "launcher_not_found",
            "could not find a TeamSpeak client launcher; install ts3client or set TS_CLIENT_LAUNCHER",
            domain::ExitCode::not_found
        ));
    }

    auto state_paths = resolve_client_state_paths();
    if (!state_paths) {
        return domain::fail<ClientProcessPaths>(state_paths.error());
    }

    return domain::ok(ClientProcessPaths{
        .launcher_path = std::move(launcher_path),
        .state_dir = state_paths.value().state_dir,
        .pid_file = state_paths.value().pid_file,
        .log_file = state_paths.value().log_file,
        .unit_file = state_paths.value().unit_file,
        .headless_script_file = state_paths.value().headless_script_file,
    });
}

auto normalize_boolean_env(std::string_view value) -> std::string {
    std::string normalized;
    normalized.reserve(value.size());
    for (const char raw_ch : value) {
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(raw_ch))));
    }
    return normalized;
}

auto resolve_client_headless_mode() -> bool {
    if (const char* raw = std::getenv("TS_CLIENT_HEADLESS"); raw != nullptr && *raw != '\0') {
        const auto normalized = normalize_boolean_env(raw);
        if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off" ||
            normalized == "disabled") {
            return false;
        }
        if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on" ||
            normalized == "enabled") {
            return true;
        }
    }

    const char* display = std::getenv("DISPLAY");
    const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
    return (display == nullptr || *display == '\0') &&
           (wayland_display == nullptr || *wayland_display == '\0');
}

auto x11_socket_path_for_display(std::string_view display) -> std::optional<std::filesystem::path> {
    if (display.size() < 2 || display.front() != ':') {
        return std::nullopt;
    }

    std::size_t digit_end = 1;
    while (digit_end < display.size() && std::isdigit(static_cast<unsigned char>(display[digit_end])) != 0) {
        ++digit_end;
    }
    if (digit_end == 1) {
        return std::nullopt;
    }

    return std::filesystem::path("/tmp/.X11-unix") /
           ("X" + std::string(display.substr(1, digit_end - 1)));
}

auto resolve_client_headless_launch() -> domain::Result<std::optional<ClientHeadlessLaunch>> {
    if (!resolve_client_headless_mode()) {
        return domain::ok(std::optional<ClientHeadlessLaunch>{});
    }

    auto xvfb = resolve_xvfb_paths();
    if (!xvfb) {
        return domain::fail<std::optional<ClientHeadlessLaunch>>(xvfb.error());
    }

    std::string display;
    if (const char* explicit_display = std::getenv("TS_CLIENT_HEADLESS_DISPLAY");
        explicit_display != nullptr && *explicit_display != '\0') {
        display = explicit_display;
    } else {
        for (int candidate = 130; candidate <= 170; ++candidate) {
            const auto socket_path = std::filesystem::path("/tmp/.X11-unix") / ("X" + std::to_string(candidate));
            std::error_code ec;
            if (!std::filesystem::exists(socket_path, ec)) {
                display = ":" + std::to_string(candidate);
                break;
            }
        }
    }

    if (display.empty()) {
        return domain::fail<std::optional<ClientHeadlessLaunch>>(client_error(
            "display_not_found",
            "failed to find a free Xvfb display between :130 and :170",
            domain::ExitCode::internal
        ));
    }

    if (!x11_socket_path_for_display(display).has_value()) {
        return domain::fail<std::optional<ClientHeadlessLaunch>>(client_error(
            "invalid_display",
            "invalid TS_CLIENT_HEADLESS_DISPLAY value: " + display,
            domain::ExitCode::config
        ));
    }

    xvfb.value().display = std::move(display);
    return domain::ok(std::optional<ClientHeadlessLaunch>{std::move(xvfb.value())});
}

auto read_pid_file(const std::filesystem::path& pid_file) -> std::optional<pid_t> {
    std::ifstream input(pid_file);
    long long raw_pid = 0;
    if (!(input >> raw_pid) || raw_pid <= 0 ||
        raw_pid > static_cast<long long>(std::numeric_limits<pid_t>::max())) {
        return std::nullopt;
    }
    return static_cast<pid_t>(raw_pid);
}

void remove_pid_file(const std::filesystem::path& pid_file) {
    std::error_code ec;
    std::filesystem::remove(pid_file, ec);
}

auto read_client_unit_file(const std::filesystem::path& unit_file) -> std::optional<std::string> {
    std::ifstream input(unit_file);
    std::string unit_name;
    if (!std::getline(input, unit_name)) {
        return std::nullopt;
    }

    unit_name = util::trim(unit_name);
    if (unit_name.empty()) {
        return std::nullopt;
    }
    return unit_name;
}

auto write_client_unit_file(const std::filesystem::path& unit_file, std::string_view unit_name) -> domain::Result<void> {
    std::ofstream output(unit_file, std::ios::trunc);
    if (!output) {
        return domain::fail(client_error(
            "unit_file_write_failed",
            "failed to write client unit file: " + unit_file.string(),
            domain::ExitCode::internal
        ));
    }
    output << unit_name << '\n';
    return domain::ok();
}

auto write_client_headless_script_file(const std::filesystem::path& script_file, std::string_view script)
    -> domain::Result<void> {
    std::ofstream output(script_file, std::ios::trunc);
    if (!output) {
        return domain::fail(client_error(
            "headless_script_write_failed",
            "failed to write client headless script: " + script_file.string(),
            domain::ExitCode::internal
        ));
    }

    output << script;
    output.close();
    if (!output) {
        return domain::fail(client_error(
            "headless_script_write_failed",
            "failed to finish writing client headless script: " + script_file.string(),
            domain::ExitCode::internal
        ));
    }

    std::error_code ec;
    std::filesystem::permissions(
        script_file,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace,
        ec
    );
    if (ec) {
        return domain::fail(client_error(
            "headless_script_write_failed",
            "failed to mark the client headless script executable: " + ec.message(),
            domain::ExitCode::internal
        ));
    }

    return domain::ok();
}

void remove_client_unit_file(const std::filesystem::path& unit_file) {
    std::error_code ec;
    std::filesystem::remove(unit_file, ec);
}

void remove_client_headless_script_file(const std::filesystem::path& script_file) {
    std::error_code ec;
    std::filesystem::remove(script_file, ec);
}

auto resolve_client_systemd_run_mode() -> ClientSystemdRunMode {
    if (const char* raw = std::getenv("TS_CLIENT_SYSTEMD_RUN"); raw != nullptr && *raw != '\0') {
        if (const auto parsed = util::parse_bool(raw); parsed.has_value()) {
            return *parsed ? ClientSystemdRunMode::enabled : ClientSystemdRunMode::disabled;
        }

        const auto normalized = normalize_boolean_env(raw);
        if (normalized == "auto") {
            return ClientSystemdRunMode::auto_detect;
        }
        if (normalized == "force" || normalized == "always" || normalized == "enabled") {
            return ClientSystemdRunMode::enabled;
        }
        if (normalized == "disable" || normalized == "disabled" || normalized == "never") {
            return ClientSystemdRunMode::disabled;
        }
    }

    return ClientSystemdRunMode::auto_detect;
}

auto resolve_client_systemd_paths() -> std::optional<ClientSystemdPaths> {
    const auto systemd_run_path = find_executable_on_path("systemd-run");
    const auto systemctl_path = find_executable_on_path("systemctl");
    if (!systemd_run_path.has_value() || !systemctl_path.has_value()) {
        return std::nullopt;
    }
    return ClientSystemdPaths{
        .systemd_run_path = *systemd_run_path,
        .systemctl_path = *systemctl_path,
    };
}

auto client_systemd_user_manager_available(const ClientSystemdPaths& paths) -> bool {
    return run_command_capture_stdout({paths.systemctl_path.string(), "--user", "show-environment"}).has_value();
}

auto read_client_systemd_unit_status(
    const ClientSystemdPaths& paths,
    const std::string& unit_name
) -> std::optional<ClientSystemdUnitStatus> {
    const auto output = run_command_capture_stdout(
        {
            paths.systemctl_path.string(),
            "--user",
            "show",
            "--property=ActiveState,SubState,MainPID",
            unit_name,
        }
    );
    if (!output.has_value()) {
        return std::nullopt;
    }

    ClientSystemdUnitStatus status;
    std::istringstream input(*output);
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const auto key = line.substr(0, separator);
        const auto value = line.substr(separator + 1);
        if (key == "ActiveState") {
            status.active_state = value;
        } else if (key == "SubState") {
            status.sub_state = value;
        } else if (key == "MainPID") {
            const auto parsed_pid = util::parse_u64(value);
            if (parsed_pid.has_value() && *parsed_pid <= static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
                status.main_pid = static_cast<pid_t>(*parsed_pid);
            }
        }
    }

    return status;
}

auto stop_client_systemd_unit(const ClientSystemdPaths& paths, const std::string& unit_name) -> bool {
    return run_command_capture_stdout({paths.systemctl_path.string(), "--user", "stop", unit_name}).has_value();
}

auto shell_quote(std::string_view value) -> std::string {
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('\'');
    for (const char ch : value) {
        if (ch == '\'') {
            quoted.append("'\\''");
            continue;
        }
        quoted.push_back(ch);
    }
    quoted.push_back('\'');
    return quoted;
}

auto make_client_systemd_unit_name() -> std::string {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch()
    )
                         .count();
    return "teamspeak-cli-client-" + std::to_string(::getpid()) + "-" + std::to_string(now) + ".service";
}

auto build_client_systemd_headless_script(
    const ClientProcessPaths& paths,
    const ClientHeadlessLaunch& headless_launch
) -> domain::Result<std::string> {
    const auto socket_path = x11_socket_path_for_display(headless_launch.display);
    if (!socket_path.has_value()) {
        return domain::fail<std::string>(client_error(
            "invalid_display",
            "invalid TS_CLIENT_HEADLESS_DISPLAY value: " + headless_launch.display,
            domain::ExitCode::config
        ));
    }

    std::ostringstream script;
    script << "set -euo pipefail\n";
    script << "xvfb_pid=''\n";
    script << "cleanup() {\n";
    script << "  if [[ -n \"${xvfb_pid:-}\" ]]; then\n";
    script << "    kill \"${xvfb_pid}\" >/dev/null 2>&1 || true\n";
    script << "    wait \"${xvfb_pid}\" >/dev/null 2>&1 || true\n";
    script << "  fi\n";
    script << "}\n";
    script << "trap cleanup EXIT\n";
    script << "export DISPLAY=" << shell_quote(headless_launch.display) << "\n";
    script << "export XDG_SESSION_TYPE='x11'\n";
    script << "unset WAYLAND_DISPLAY\n";
    script << "env";
    if (!headless_launch.xvfb_library_path.empty()) {
        script << " LD_LIBRARY_PATH=" << shell_quote(headless_launch.xvfb_library_path)
               << "${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}";
    }
    if (!headless_launch.xvfb_binary_dir.empty()) {
        script << " PATH=" << shell_quote(headless_launch.xvfb_binary_dir.string())
               << "${PATH:+:${PATH}}";
    }
    script << ' ' << shell_quote(headless_launch.xvfb_path.string()) << ' '
           << shell_quote(headless_launch.display) << " -screen 0 1280x1024x24 -ac";
    if (!headless_launch.xvfb_xkb_dir.empty()) {
        script << " -xkbdir " << shell_quote(headless_launch.xvfb_xkb_dir.string());
    }
    script << " &\n";
    script << "xvfb_pid=$!\n";
    script << "for _ in $(seq 1 100); do\n";
    script << "  if [[ -e " << shell_quote(socket_path->string()) << " ]]; then\n";
    script << "    break\n";
    script << "  fi\n";
    script << "  if ! kill -0 \"${xvfb_pid}\" >/dev/null 2>&1; then\n";
    script << "    wait \"${xvfb_pid}\" >/dev/null 2>&1 || true\n";
    script << "    exit 127\n";
    script << "  fi\n";
    script << "  sleep 0.05\n";
    script << "done\n";
    script << "[[ -e " << shell_quote(socket_path->string()) << " ]]\n";
    script << "exec " << shell_quote(paths.launcher_path.string()) << "\n";
    return domain::ok(script.str());
}

auto process_is_running(pid_t pid) -> bool {
    if (pid <= 0) {
        return false;
    }
    int status = 0;
    const pid_t wait_result = ::waitpid(pid, &status, WNOHANG);
    if (wait_result == pid) {
        return false;
    }
    if (::kill(pid, 0) == 0) {
        return true;
    }
    return errno == EPERM;
}

auto process_group_for_pid(pid_t pid) -> std::optional<pid_t> {
    if (pid <= 0) {
        return std::nullopt;
    }
    const pid_t pgid = ::getpgid(pid);
    if (pgid <= 0) {
        return std::nullopt;
    }
    return pgid;
}

auto signal_client_target(pid_t pid, int signal_number) -> bool {
    if (const auto pgid = process_group_for_pid(pid); pgid.has_value()) {
        if (::kill(-*pgid, signal_number) == 0) {
            return true;
        }
        if (errno != ESRCH) {
            return false;
        }
    }
    return ::kill(pid, signal_number) == 0;
}

auto reap_process_with_timeout(pid_t pid, std::chrono::milliseconds timeout) -> bool {
    if (pid <= 0) {
        return true;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t wait_result = ::waitpid(pid, &status, WNOHANG);
        if (wait_result == pid) {
            return true;
        }
        if (wait_result < 0 && errno == ECHILD) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    int status = 0;
    const pid_t final_wait_result = ::waitpid(pid, &status, WNOHANG);
    return final_wait_result == pid || (final_wait_result < 0 && errno == ECHILD);
}

void terminate_and_reap_process(pid_t pid) {
    if (pid <= 0) {
        return;
    }

    if (::kill(pid, SIGTERM) == 0 || errno == ESRCH) {
        if (reap_process_with_timeout(pid, std::chrono::seconds(1))) {
            return;
        }
    }

    if (::kill(pid, SIGKILL) == 0 || errno == ESRCH) {
        (void)reap_process_with_timeout(pid, std::chrono::seconds(1));
    }
}

auto basename_string(const std::string& raw) -> std::string {
    return std::filesystem::path(raw).filename().string();
}

auto client_process_match_score(const std::string& raw_name) -> int {
    const auto name = basename_string(raw_name);
    if (const char* override_name = std::getenv("TS_CLIENT_DISCOVERY_NAME");
        override_name != nullptr && *override_name != '\0') {
        return name == override_name ? 100 : 0;
    }
    if (name == "ts3client_linux_amd64") {
        return 4;
    }
    if (name == "ts3client_runscript.sh") {
        return 3;
    }
    if (name == "ts3client") {
        return 2;
    }
    if (name == "teamspeak3-client") {
        return 1;
    }
    return 0;
}

auto read_proc_cmdline_first_arg(pid_t pid) -> std::optional<std::string> {
    std::ifstream input("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    std::string value;
    if (!std::getline(input, value, '\0') || value.empty()) {
        return std::nullopt;
    }
    return value;
}

auto read_proc_exe_path(pid_t pid) -> std::optional<std::string> {
    std::error_code ec;
    const auto symlink_target =
        std::filesystem::read_symlink("/proc/" + std::to_string(pid) + "/exe", ec);
    if (ec || symlink_target.empty()) {
        return std::nullopt;
    }
    return symlink_target.string();
}

auto discover_running_client_process() -> std::optional<DiscoveredClientProcess> {
    std::error_code ec;
    DiscoveredClientProcess best_match;
    int best_score = 0;

    for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_directory(ec)) {
            continue;
        }

        const auto name = entry.path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char ch) {
                return std::isdigit(ch) != 0;
            })) {
            continue;
        }

        const auto raw_pid = util::parse_u64(name);
        if (!raw_pid.has_value() || *raw_pid == 0 ||
            *raw_pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
            continue;
        }
        const auto pid = static_cast<pid_t>(*raw_pid);
        if (!process_is_running(pid)) {
            continue;
        }

        int score = 0;
        std::string matched_name;
        if (const auto first_arg = read_proc_cmdline_first_arg(pid); first_arg.has_value()) {
            score = client_process_match_score(*first_arg);
            if (score > 0) {
                matched_name = basename_string(*first_arg);
            }
        }
        if (const auto exe_path = read_proc_exe_path(pid); exe_path.has_value()) {
            const int exe_score = client_process_match_score(*exe_path);
            if (exe_score > score) {
                score = exe_score;
                matched_name = basename_string(*exe_path);
            }
        }
        if (score <= 0) {
            continue;
        }

        best_match.match_count += 1;
        if (best_match.pid == 0 || score > best_score || (score == best_score && pid > best_match.pid)) {
            best_match.pid = pid;
            best_match.process_name = std::move(matched_name);
            best_score = score;
        }
    }

    if (best_match.pid == 0) {
        return std::nullopt;
    }
    return best_match;
}

auto write_pid_file(const std::filesystem::path& pid_file, pid_t pid) -> domain::Result<void> {
    std::ofstream output(pid_file, std::ios::trunc);
    if (!output) {
        return domain::fail(client_error(
            "pid_file_write_failed",
            "failed to write pid file: " + pid_file.string(),
            domain::ExitCode::internal
        ));
    }
    output << pid << '\n';
    return domain::ok();
}

auto client_process_output(
    std::string action,
    std::string status,
    pid_t pid,
    const ClientProcessPaths& paths,
    std::string note
) -> output::CommandOutput {
    auto client_process_human = [&]() -> std::string {
        std::ostringstream out;
        if (action == "status" && status == "running") {
            out << "The local TeamSpeak client is running as PID " << pid << '.';
        } else if (action == "status") {
            out << "The local TeamSpeak client is not running.";
        } else if (action == "start" && status == "started") {
            out << "Started the local TeamSpeak client as PID " << pid << '.';
        } else if (action == "start" && status == "already-running") {
            out << "The local TeamSpeak client is already running as PID " << pid << '.';
        } else if (action == "stop" && status == "stopped") {
            out << "Stopped the local TeamSpeak client process rooted at PID " << pid << '.';
        } else {
            out << "Updated the local TeamSpeak client process state.";
        }
        out << "\n\nClient Context\n";
        out << output::render_details_block(output::Details{
            .fields = {
                {"Status", status},
                {"PID", pid > 0 ? std::to_string(pid) : "-"},
                {"Launcher", paths.launcher_path.string()},
                {"PIDFile", paths.pid_file.string()},
                {"LogFile", paths.log_file.string()},
                {"Note", note},
            },
        });
        return out.str();
    };

    return output::CommandOutput{
        .data = output::make_object({
            {"action", output::make_string(action)},
            {"status", output::make_string(status)},
            {"pid", output::make_int(static_cast<std::int64_t>(pid))},
            {"launcher", output::make_string(paths.launcher_path.string())},
            {"pid_file", output::make_string(paths.pid_file.string())},
            {"log_file", output::make_string(paths.log_file.string())},
            {"note", output::make_string(note)},
        }),
        .human = client_process_human(),
    };
}

auto discovered_process_note(const DiscoveredClientProcess& process, std::string base_note) -> std::string {
    if (process.match_count <= 1) {
        return base_note + " (" + process.process_name + ")";
    }
    return base_note + " (" + process.process_name + ", " + std::to_string(process.match_count) +
           " matching processes found)";
}

auto client_process_missing_output(std::string action, const ClientProcessPaths& paths, std::string note)
    -> output::CommandOutput {
    auto client_process_human = [&]() -> std::string {
        std::ostringstream out;
        if (action == "stop") {
            out << "The local TeamSpeak client is not running, so there was nothing to stop.";
        } else {
            out << "The local TeamSpeak client is not running.";
        }
        out << "\n\nClient Context\n";
        out << output::render_details_block(output::Details{
            .fields = {
                {"Status", "not-running"},
                {"PID", "-"},
                {"Launcher", paths.launcher_path.string()},
                {"PIDFile", paths.pid_file.string()},
                {"LogFile", paths.log_file.string()},
                {"Note", note},
            },
        });
        return out.str();
    };

    return output::CommandOutput{
        .data = output::make_object({
            {"action", output::make_string(action)},
            {"status", output::make_string("not-running")},
            {"pid", output::make_int(0)},
            {"launcher", output::make_string(paths.launcher_path.string())},
            {"pid_file", output::make_string(paths.pid_file.string())},
            {"log_file", output::make_string(paths.log_file.string())},
            {"note", output::make_string(note)},
        }),
        .human = client_process_human(),
    };
}

auto resolve_teamspeak_client_logs_dir() -> std::optional<std::filesystem::path> {
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".ts3client" / "logs";
    }
    return std::nullopt;
}

auto read_log_tail(const std::filesystem::path& path, std::size_t count) -> domain::Result<std::vector<std::string>> {
    std::ifstream input(path);
    if (!input) {
        return domain::fail<std::vector<std::string>>(client_error(
            "log_read_failed",
            "failed to read client log: " + path.string(),
            domain::ExitCode::internal
        ));
    }

    std::deque<std::string> tail;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        tail.push_back(line);
        if (tail.size() > count) {
            tail.pop_front();
        }
    }

    if (!input.eof() && input.fail()) {
        return domain::fail<std::vector<std::string>>(client_error(
            "log_read_failed",
            "failed while reading client log: " + path.string(),
            domain::ExitCode::internal
        ));
    }

    return domain::ok(std::vector<std::string>(tail.begin(), tail.end()));
}

auto record_client_log_source(
    std::vector<ClientLogSource>& sources,
    const std::string& kind,
    const std::filesystem::path& path,
    std::size_t count
) -> void {
    auto lines = read_log_tail(path, count);
    if (!lines) {
        sources.push_back(ClientLogSource{
            .kind = kind,
            .path = path,
            .lines = {},
            .error = lines.error().message,
        });
        return;
    }

    sources.push_back(ClientLogSource{
        .kind = kind,
        .path = path,
        .lines = lines.value(),
        .error = {},
    });
}

auto collect_recent_teamspeak_log_files(const std::filesystem::path& log_dir) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(log_dir, ec)) {
        if (ec) {
            return {};
        }
        if (entry.is_regular_file(ec) && !ec) {
            files.push_back(entry.path());
        }
    }

    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        std::error_code left_ec;
        std::error_code right_ec;
        const auto left_time = std::filesystem::last_write_time(left, left_ec);
        const auto right_time = std::filesystem::last_write_time(right, right_ec);
        if (!left_ec && !right_ec && left_time != right_time) {
            return left_time > right_time;
        }
        return left.filename().string() > right.filename().string();
    });

    if (files.size() > 3) {
        files.resize(3);
    }
    return files;
}

auto client_logs_output(std::size_t count) -> domain::Result<output::CommandOutput> {
    auto state_paths = resolve_client_state_paths();
    if (!state_paths) {
        return domain::fail<output::CommandOutput>(state_paths.error());
    }

    std::vector<std::filesystem::path> searched;
    append_unique_path(searched, state_paths.value().log_file);
    if (const auto teamspeak_logs_dir = resolve_teamspeak_client_logs_dir(); teamspeak_logs_dir.has_value()) {
        append_unique_path(searched, *teamspeak_logs_dir);
    }

    std::vector<ClientLogSource> sources;
    std::error_code ec;
    if (std::filesystem::exists(state_paths.value().log_file, ec) && !ec) {
        record_client_log_source(sources, "launcher", state_paths.value().log_file, count);
    }

    if (const auto teamspeak_logs_dir = resolve_teamspeak_client_logs_dir(); teamspeak_logs_dir.has_value()) {
        ec.clear();
        if (std::filesystem::is_directory(*teamspeak_logs_dir, ec) && !ec) {
            for (const auto& log_file : collect_recent_teamspeak_log_files(*teamspeak_logs_dir)) {
                record_client_log_source(sources, "teamspeak", log_file, count);
            }
        }
    }

    std::vector<output::ValueHolder> source_values;
    source_values.reserve(sources.size());
    for (const auto& source : sources) {
        std::vector<output::ValueHolder> line_values;
        line_values.reserve(source.lines.size());
        for (const auto& line : source.lines) {
            line_values.push_back(output::make_string(line));
        }

        std::map<std::string, output::ValueHolder> payload{
            {"kind", output::make_string(source.kind)},
            {"lines", output::make_array(std::move(line_values))},
            {"path", output::make_string(source.path.string())},
        };
        if (!source.error.empty()) {
            payload.emplace("error", output::make_string(source.error));
        }
        source_values.push_back(output::make_object(std::move(payload)));
    }

    std::vector<output::ValueHolder> searched_values;
    searched_values.reserve(searched.size());
    for (const auto& path : searched) {
        searched_values.push_back(output::make_string(path.string()));
    }

    auto client_logs_human = [&]() -> std::string {
        std::ostringstream out;
        if (sources.empty()) {
            out << "No local TeamSpeak client logs were found.";
            if (!searched.empty()) {
                out << "\n\nSearched\n";
                for (std::size_t index = 0; index < searched.size(); ++index) {
                    out << searched[index].string();
                    if (index + 1 != searched.size()) {
                        out << '\n';
                    }
                }
            }
            return out.str();
        }

        out << "Recent TeamSpeak client logs (last " << count << " line";
        if (count != 1) {
            out << 's';
        }
        out << " per file)\n\n";
        for (std::size_t index = 0; index < sources.size(); ++index) {
            const auto& source = sources[index];
            out << (source.kind == "launcher" ? "Tracked launcher log" : "TeamSpeak client log")
                << ": " << source.path.string() << '\n';
            if (!source.error.empty()) {
                out << source.error;
            } else if (source.lines.empty()) {
                out << "(empty)";
            } else {
                for (std::size_t line_index = 0; line_index < source.lines.size(); ++line_index) {
                    out << source.lines[line_index];
                    if (line_index + 1 != source.lines.size()) {
                        out << '\n';
                    }
                }
            }
            if (index + 1 != sources.size()) {
                out << "\n\n";
            }
        }
        return out.str();
    };

    return domain::ok(output::CommandOutput{
        .data = output::make_object({
            {"count", output::make_int(static_cast<std::int64_t>(count))},
            {"searched", output::make_array(std::move(searched_values))},
            {"sources", output::make_array(std::move(source_values))},
            {"status", output::make_string(sources.empty() ? "not-found" : "found")},
        }),
        .human = client_logs_human(),
    });
}

auto client_should_use_systemd_run(const std::optional<ClientHeadlessLaunch>& headless_launch) -> bool {
    const auto mode = resolve_client_systemd_run_mode();
    if (mode == ClientSystemdRunMode::disabled) {
        return false;
    }
    if (mode == ClientSystemdRunMode::enabled) {
        return true;
    }
    return headless_launch.has_value();
}

auto launch_client_process_direct(
    const ClientProcessPaths& paths,
    const std::string& launch_socket_path,
    const std::optional<ClientHeadlessLaunch>& headless_launch
) -> domain::Result<ClientLaunchResult> {
    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) {
        return domain::fail<ClientLaunchResult>(client_error(
            "pipe_failed",
            "failed to create launcher status pipe: " + std::string(std::strerror(errno)),
            domain::ExitCode::internal
        ));
    }
    auto close_pipe_fds = util::make_scope_exit([&] {
        if (pipe_fds[0] >= 0) {
            ::close(pipe_fds[0]);
        }
        if (pipe_fds[1] >= 0) {
            ::close(pipe_fds[1]);
        }
    });

    if (::fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC) == -1) {
        return domain::fail<ClientLaunchResult>(client_error(
            "pipe_cloexec_failed",
            "failed to mark launcher status pipe close-on-exec: " + std::string(std::strerror(errno)),
            domain::ExitCode::internal
        ));
    }

    const pid_t child_pid = ::fork();
    if (child_pid < 0) {
        return domain::fail<ClientLaunchResult>(client_error(
            "fork_failed",
            "failed to fork TeamSpeak client process: " + std::string(std::strerror(errno)),
            domain::ExitCode::internal
        ));
    }

    if (child_pid == 0) {
        ::close(pipe_fds[0]);

        if (::setsid() == -1) {
            const int child_errno = errno;
            (void)::write(pipe_fds[1], &child_errno, sizeof(child_errno));
            _exit(127);
        }

        const int stdin_fd = ::open("/dev/null", O_RDONLY);
        const int log_fd = ::open(paths.log_file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (stdin_fd < 0 || log_fd < 0) {
            const int child_errno = errno;
            if (stdin_fd >= 0) {
                ::close(stdin_fd);
            }
            if (log_fd >= 0) {
                ::close(log_fd);
            }
            (void)::write(pipe_fds[1], &child_errno, sizeof(child_errno));
            _exit(127);
        }

        if (::dup2(stdin_fd, STDIN_FILENO) == -1 || ::dup2(log_fd, STDOUT_FILENO) == -1 ||
            ::dup2(log_fd, STDERR_FILENO) == -1) {
            const int child_errno = errno;
            ::close(stdin_fd);
            ::close(log_fd);
            (void)::write(pipe_fds[1], &child_errno, sizeof(child_errno));
            _exit(127);
        }

        ::close(stdin_fd);
        ::close(log_fd);

        if (::setenv("TS_CONTROL_SOCKET_PATH", launch_socket_path.c_str(), 1) != 0) {
            const int child_errno = errno;
            (void)::write(pipe_fds[1], &child_errno, sizeof(child_errno));
            _exit(127);
        }

        if (headless_launch.has_value()) {
            const auto& headless = *headless_launch;
            const pid_t xvfb_pid = ::fork();
            if (xvfb_pid < 0) {
                const int child_errno = errno;
                (void)::write(pipe_fds[1], &child_errno, sizeof(child_errno));
                _exit(127);
            }

            if (xvfb_pid == 0) {
                auto prepend_env_path = [&](const char* name, const std::string& prefix) {
                    if (prefix.empty()) {
                        return;
                    }
                    std::string value = prefix;
                    if (const char* existing = std::getenv(name); existing != nullptr && *existing != '\0') {
                        value += ":";
                        value += existing;
                    }
                    if (::setenv(name, value.c_str(), 1) != 0) {
                        const int child_errno = errno;
                        (void)::write(pipe_fds[1], &child_errno, sizeof(child_errno));
                        _exit(127);
                    }
                };

                prepend_env_path("LD_LIBRARY_PATH", headless.xvfb_library_path);
                prepend_env_path("PATH", headless.xvfb_binary_dir.string());

                std::vector<std::string> xvfb_args = {
                    headless.xvfb_path.string(),
                    headless.display,
                    "-screen",
                    "0",
                    "1280x1024x24",
                    "-ac",
                };
                if (!headless.xvfb_xkb_dir.empty()) {
                    xvfb_args.push_back("-xkbdir");
                    xvfb_args.push_back(headless.xvfb_xkb_dir.string());
                }

                std::vector<char*> xvfb_argv;
                xvfb_argv.reserve(xvfb_args.size() + 1);
                for (auto& argument : xvfb_args) {
                    xvfb_argv.push_back(argument.data());
                }
                xvfb_argv.push_back(nullptr);

                ::execv(headless.xvfb_path.c_str(), xvfb_argv.data());

                const int child_errno = errno;
                (void)::write(pipe_fds[1], &child_errno, sizeof(child_errno));
                _exit(127);
            }

            const auto socket_path = x11_socket_path_for_display(headless.display);
            if (!socket_path.has_value()) {
                const int child_errno = EINVAL;
                terminate_and_reap_process(xvfb_pid);
                (void)::write(pipe_fds[1], &child_errno, sizeof(child_errno));
                _exit(127);
            }

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            bool display_ready = false;
            while (std::chrono::steady_clock::now() < deadline) {
                std::error_code socket_ec;
                if (std::filesystem::exists(*socket_path, socket_ec) && !socket_ec) {
                    display_ready = true;
                    break;
                }

                int xvfb_status = 0;
                const pid_t wait_result = ::waitpid(xvfb_pid, &xvfb_status, WNOHANG);
                if (wait_result == xvfb_pid) {
                    _exit(127);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            if (!display_ready) {
                const int child_errno = ETIMEDOUT;
                terminate_and_reap_process(xvfb_pid);
                (void)::write(pipe_fds[1], &child_errno, sizeof(child_errno));
                _exit(127);
            }

            if (::setenv("DISPLAY", headless.display.c_str(), 1) != 0 ||
                ::setenv("XDG_SESSION_TYPE", "x11", 1) != 0) {
                const int child_errno = errno;
                terminate_and_reap_process(xvfb_pid);
                (void)::write(pipe_fds[1], &child_errno, sizeof(child_errno));
                _exit(127);
            }
            (void)::unsetenv("WAYLAND_DISPLAY");
        }

        char* const argv[] = {const_cast<char*>(paths.launcher_path.c_str()), nullptr};
        ::execv(paths.launcher_path.c_str(), argv);

        const int child_errno = errno;
        (void)::write(pipe_fds[1], &child_errno, sizeof(child_errno));
        _exit(127);
    }

    ::close(pipe_fds[1]);
    pipe_fds[1] = -1;

    int child_errno = 0;
    const auto bytes_read = ::read(pipe_fds[0], &child_errno, sizeof(child_errno));
    ::close(pipe_fds[0]);
    pipe_fds[0] = -1;

    if (bytes_read > 0) {
        int ignored_status = 0;
        (void)::waitpid(child_pid, &ignored_status, 0);
        return domain::fail<ClientLaunchResult>(client_error(
            "launch_failed",
            "failed to launch TeamSpeak client: " + std::string(std::strerror(child_errno)),
            domain::ExitCode::internal
        ));
    }

    return domain::ok(ClientLaunchResult{
        .pid = child_pid,
        .note = headless_launch.has_value()
                    ? "client process launched headlessly on DISPLAY " + headless_launch->display +
                          " using socket " + launch_socket_path
                    : "client process launched using socket " + launch_socket_path,
    });
}

auto launch_client_process_via_systemd(
    const ClientProcessPaths& paths,
    const std::string& launch_socket_path,
    const std::optional<ClientHeadlessLaunch>& headless_launch
) -> domain::Result<ClientLaunchResult> {
    const auto systemd_paths = resolve_client_systemd_paths();
    if (!systemd_paths.has_value()) {
        return domain::fail<ClientLaunchResult>(client_error(
            "systemd_unavailable",
            "systemd-run is not available on PATH for this launch",
            domain::ExitCode::not_found
        ));
    }
    if (!client_systemd_user_manager_available(*systemd_paths)) {
        return domain::fail<ClientLaunchResult>(client_error(
            "systemd_user_unavailable",
            "the user systemd manager is not available for this launch",
            domain::ExitCode::unsupported
        ));
    }

    const std::string unit_name = make_client_systemd_unit_name();
    std::vector<std::string> argv = {
        systemd_paths->systemd_run_path.string(),
        "--user",
        "--collect",
        "--quiet",
        "--same-dir",
        "--unit=" + unit_name,
        "--service-type=exec",
        "--property=StandardInput=null",
        "--property=StandardOutput=append:" + paths.log_file.string(),
        "--property=StandardError=append:" + paths.log_file.string(),
        "--setenv=TS_CONTROL_SOCKET_PATH=" + launch_socket_path,
    };

    if (headless_launch.has_value()) {
        auto headless_script = build_client_systemd_headless_script(paths, *headless_launch);
        if (!headless_script) {
            return domain::fail<ClientLaunchResult>(headless_script.error());
        }

        auto wrote_headless_script =
            write_client_headless_script_file(paths.headless_script_file, headless_script.value());
        if (!wrote_headless_script) {
            return domain::fail<ClientLaunchResult>(wrote_headless_script.error());
        }

        argv.push_back("/bin/bash");
        argv.push_back(paths.headless_script_file.string());
    } else {
        remove_client_headless_script_file(paths.headless_script_file);
        argv.push_back(paths.launcher_path.string());
    }

    if (!run_command_capture_stdout(argv).has_value()) {
        if (headless_launch.has_value()) {
            remove_client_headless_script_file(paths.headless_script_file);
        }
        return domain::fail<ClientLaunchResult>(client_error(
            "systemd_launch_failed",
            "failed to launch TeamSpeak client through systemd-run --user",
            domain::ExitCode::internal
        ));
    }

    std::optional<ClientSystemdUnitStatus> unit_status;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        unit_status = read_client_systemd_unit_status(*systemd_paths, unit_name);
        if (unit_status.has_value() && unit_status->main_pid > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!unit_status.has_value() || unit_status->main_pid <= 0) {
        (void)stop_client_systemd_unit(*systemd_paths, unit_name);
        if (headless_launch.has_value()) {
            remove_client_headless_script_file(paths.headless_script_file);
        }
        return domain::fail<ClientLaunchResult>(client_error(
            "systemd_launch_failed",
            "systemd-run started a transient unit but no main client pid was reported",
            domain::ExitCode::internal
        ));
    }

    auto wrote_unit = write_client_unit_file(paths.unit_file, unit_name);
    if (!wrote_unit) {
        (void)stop_client_systemd_unit(*systemd_paths, unit_name);
        if (headless_launch.has_value()) {
            remove_client_headless_script_file(paths.headless_script_file);
        }
        return domain::fail<ClientLaunchResult>(wrote_unit.error());
    }

    return domain::ok(ClientLaunchResult{
        .pid = unit_status->main_pid,
        .note = headless_launch.has_value()
                    ? "client process launched headlessly on DISPLAY " + headless_launch->display +
                          " using socket " + launch_socket_path + " via transient user systemd unit " + unit_name
                    : "client process launched using socket " + launch_socket_path +
                          " via transient user systemd unit " + unit_name,
    });
}

auto client_status_process() -> domain::Result<output::CommandOutput> {
    auto paths = resolve_client_process_paths();
    if (!paths) {
        return domain::fail<output::CommandOutput>(paths.error());
    }

    if (const auto unit_name = read_client_unit_file(paths.value().unit_file); unit_name.has_value()) {
        if (const auto systemd_paths = resolve_client_systemd_paths(); systemd_paths.has_value()) {
            if (const auto unit_status = read_client_systemd_unit_status(*systemd_paths, *unit_name);
                unit_status.has_value()) {
                const bool unit_active =
                    unit_status->active_state == "active" || unit_status->active_state == "activating";
                if (unit_active && unit_status->main_pid > 0 && process_is_running(unit_status->main_pid)) {
                    auto wrote_pid = write_pid_file(paths.value().pid_file, unit_status->main_pid);
                    (void)wrote_pid;
                    return domain::ok(client_process_output(
                        "status",
                        "running",
                        unit_status->main_pid,
                        paths.value(),
                        "tracked transient user systemd unit " + *unit_name + " is " + unit_status->active_state
                    ));
                }
                if (!unit_active) {
                    remove_pid_file(paths.value().pid_file);
                    remove_client_unit_file(paths.value().unit_file);
                    remove_client_headless_script_file(paths.value().headless_script_file);
                }
            }
        }
    }

    const auto pid = read_pid_file(paths.value().pid_file);
    if (!pid.has_value()) {
        remove_pid_file(paths.value().pid_file);
        if (const auto discovered = discover_running_client_process(); discovered.has_value()) {
            return domain::ok(client_process_output(
                "status",
                "running",
                discovered->pid,
                paths.value(),
                discovered_process_note(*discovered, "detected running TeamSpeak client without a ts pid file")
            ));
        }
        return domain::ok(client_process_missing_output(
            "status",
            paths.value(),
            "no tracked client pid file was found"
        ));
    }

    if (!process_is_running(*pid)) {
        remove_pid_file(paths.value().pid_file);
        if (const auto discovered = discover_running_client_process(); discovered.has_value()) {
            return domain::ok(client_process_output(
                "status",
                "running",
                discovered->pid,
                paths.value(),
                discovered_process_note(*discovered, "tracked pid file was stale but a TeamSpeak client is running")
            ));
        }
        return domain::ok(client_process_missing_output(
            "status",
            paths.value(),
            "tracked client pid file was stale"
        ));
    }

    return domain::ok(client_process_output(
        "status",
        "running",
        *pid,
        paths.value(),
        "tracked client process is running"
    ));
}

auto start_client_process(
    const ParsedCommand& command,
    const config::ConfigStore& store,
    const CommandRouter::ProgressSink& progress = {}
)
    -> domain::Result<output::CommandOutput> {
    auto paths = resolve_client_process_paths();
    if (!paths) {
        return domain::fail<output::CommandOutput>(paths.error());
    }

    if (progress) {
        progress("Checking whether a local TeamSpeak client is already running.");
    }

    auto launch_socket_path = resolve_client_launch_socket_path(command, store);
    if (!launch_socket_path) {
        return domain::fail<output::CommandOutput>(launch_socket_path.error());
    }

    auto headless_launch = resolve_client_headless_launch();
    if (!headless_launch) {
        return domain::fail<output::CommandOutput>(headless_launch.error());
    }

    auto launch_preflight = preflight_client_launch_runtime(paths.value());
    if (!launch_preflight) {
        return domain::fail<output::CommandOutput>(launch_preflight.error());
    }

    if (progress) {
        progress("The new TeamSpeak client session will use control socket " + launch_socket_path.value() + ".");
        if (headless_launch.value().has_value()) {
            progress(
                "No GUI display was detected, so the TeamSpeak client will start headlessly on DISPLAY " +
                headless_launch.value()->display + "."
            );
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(paths.value().state_dir, ec);
    if (ec) {
        return domain::fail<output::CommandOutput>(client_error(
            "state_dir_create_failed",
            "failed to create client state directory: " + ec.message(),
            domain::ExitCode::internal
        ));
    }

    if (const auto unit_name = read_client_unit_file(paths.value().unit_file); unit_name.has_value()) {
        if (const auto systemd_paths = resolve_client_systemd_paths(); systemd_paths.has_value()) {
            if (const auto unit_status = read_client_systemd_unit_status(*systemd_paths, *unit_name);
                unit_status.has_value()) {
                const bool unit_active =
                    unit_status->active_state == "active" || unit_status->active_state == "activating";
                if (unit_active && unit_status->main_pid > 0 && process_is_running(unit_status->main_pid)) {
                    auto wrote_pid = write_pid_file(paths.value().pid_file, unit_status->main_pid);
                    (void)wrote_pid;
                    return domain::ok(client_process_output(
                        "start",
                        "already-running",
                        unit_status->main_pid,
                        paths.value(),
                        "tracked transient user systemd unit " + *unit_name + " is " + unit_status->active_state
                    ));
                }
                if (!unit_active) {
                    remove_pid_file(paths.value().pid_file);
                    remove_client_unit_file(paths.value().unit_file);
                    remove_client_headless_script_file(paths.value().headless_script_file);
                }
            }
        }
    }

    if (const auto pid = read_pid_file(paths.value().pid_file); pid.has_value()) {
        if (process_is_running(*pid)) {
            return domain::ok(client_process_output(
                "start",
                "already-running",
                *pid,
                paths.value(),
                "client process is already running"
            ));
        }
        remove_pid_file(paths.value().pid_file);
    }

    if (const auto discovered = discover_running_client_process(); discovered.has_value()) {
        return domain::ok(client_process_output(
            "start",
            "already-running",
            discovered->pid,
            paths.value(),
            discovered_process_note(*discovered, "detected existing TeamSpeak client process")
        ));
    }

    if (progress) {
        progress("Launching the TeamSpeak client process.");
    }

    remove_client_unit_file(paths.value().unit_file);
    remove_client_headless_script_file(paths.value().headless_script_file);

    domain::Result<ClientLaunchResult> launched = domain::fail<ClientLaunchResult>(client_error(
        "launch_failed",
        "failed to launch TeamSpeak client",
        domain::ExitCode::internal
    ));
    bool used_systemd_launch = false;
    const bool prefer_systemd = client_should_use_systemd_run(headless_launch.value());
    if (prefer_systemd) {
        if (progress) {
            progress("Launching the TeamSpeak client through a transient user systemd unit.");
        }
        launched = launch_client_process_via_systemd(
            paths.value(), launch_socket_path.value(), headless_launch.value()
        );
        if (launched) {
            used_systemd_launch = true;
        } else if (resolve_client_systemd_run_mode() == ClientSystemdRunMode::enabled) {
            return domain::fail<output::CommandOutput>(launched.error());
        } else if (progress) {
            progress(
                "The transient user systemd launch path was unavailable, so the TeamSpeak client will be started as a local detached process instead."
            );
        }
    }

    if (!launched) {
        launched = launch_client_process_direct(paths.value(), launch_socket_path.value(), headless_launch.value());
        if (!launched) {
            return domain::fail<output::CommandOutput>(launched.error());
        }
    }

    auto wrote_pid = write_pid_file(paths.value().pid_file, launched.value().pid);
    if (!wrote_pid) {
        if (used_systemd_launch) {
            if (const auto unit_name = read_client_unit_file(paths.value().unit_file); unit_name.has_value()) {
                if (const auto systemd_paths = resolve_client_systemd_paths(); systemd_paths.has_value()) {
                    (void)stop_client_systemd_unit(*systemd_paths, *unit_name);
                }
            }
            remove_client_unit_file(paths.value().unit_file);
            remove_client_headless_script_file(paths.value().headless_script_file);
        } else {
            (void)::kill(launched.value().pid, SIGTERM);
        }
        return domain::fail<output::CommandOutput>(wrote_pid.error());
    }

    if (headless_launch.value().has_value()) {
        dismiss_headless_client_dialogs(*headless_launch.value(), launched.value().pid);
    }

    if (progress) {
        progress("The TeamSpeak client started as PID " + std::to_string(launched.value().pid) + ".");
    }

    return domain::ok(client_process_output(
        "start",
        "started",
        launched.value().pid,
        paths.value(),
        launched.value().note
    ));
}

auto stop_client_process(bool force, const CommandRouter::ProgressSink& progress = {})
    -> domain::Result<output::CommandOutput> {
    auto paths = resolve_client_process_paths();
    if (!paths) {
        return domain::fail<output::CommandOutput>(paths.error());
    }

    if (progress) {
        progress("Looking for a running local TeamSpeak client process.");
    }

    if (const auto unit_name = read_client_unit_file(paths.value().unit_file); unit_name.has_value()) {
        if (const auto systemd_paths = resolve_client_systemd_paths(); systemd_paths.has_value()) {
            const auto unit_status = read_client_systemd_unit_status(*systemd_paths, *unit_name);
            const bool unit_active = unit_status.has_value() &&
                                     (unit_status->active_state == "active" ||
                                      unit_status->active_state == "activating");
            if (unit_active) {
                if (progress) {
                    progress("Stopping the transient user systemd unit " + *unit_name + ".");
                }
                if (stop_client_systemd_unit(*systemd_paths, *unit_name)) {
                    const pid_t stopped_pid = unit_status->main_pid > 0 ? unit_status->main_pid : 0;
                    remove_pid_file(paths.value().pid_file);
                    remove_client_unit_file(paths.value().unit_file);
                    remove_client_headless_script_file(paths.value().headless_script_file);
                    return domain::ok(client_process_output(
                        "stop",
                        "stopped",
                        stopped_pid,
                        paths.value(),
                        "stopped transient user systemd unit " + *unit_name
                    ));
                }
            } else if (!unit_status.has_value() ||
                       unit_status->active_state == "inactive" ||
                       unit_status->active_state == "failed") {
                remove_client_unit_file(paths.value().unit_file);
                remove_pid_file(paths.value().pid_file);
                remove_client_headless_script_file(paths.value().headless_script_file);
            }
        }
    }

    auto pid = read_pid_file(paths.value().pid_file);
    std::string discovered_note;
    if (!pid.has_value()) {
        remove_pid_file(paths.value().pid_file);
        if (const auto discovered = discover_running_client_process(); discovered.has_value()) {
            pid = discovered->pid;
            discovered_note = discovered_process_note(
                *discovered, "stopping detected TeamSpeak client without a ts pid file"
            );
            if (progress) {
                progress("No tracked pid file was available, so the detected TeamSpeak client process will be stopped.");
            }
        } else {
            remove_client_headless_script_file(paths.value().headless_script_file);
            return domain::ok(client_process_missing_output(
                "stop",
                paths.value(),
                "no tracked client pid file was found"
            ));
        }
    }

    if (!process_is_running(*pid)) {
        remove_pid_file(paths.value().pid_file);
        if (const auto discovered = discover_running_client_process(); discovered.has_value()) {
            pid = discovered->pid;
            discovered_note = discovered_process_note(
                *discovered, "tracked pid file was stale; stopping detected TeamSpeak client"
            );
            if (progress) {
                progress("The tracked pid file was stale, so the detected TeamSpeak client process will be stopped.");
            }
        } else {
            remove_client_headless_script_file(paths.value().headless_script_file);
            return domain::ok(client_process_missing_output(
                "stop",
                paths.value(),
                "tracked client pid file was stale"
            ));
        }
    }

    if (progress) {
        progress("Sending SIGTERM to the TeamSpeak client process group rooted at PID " + std::to_string(*pid) + ".");
    }

    if (!signal_client_target(*pid, SIGTERM)) {
        if (errno == ESRCH) {
            remove_pid_file(paths.value().pid_file);
            remove_client_headless_script_file(paths.value().headless_script_file);
            return domain::ok(client_process_missing_output(
                "stop",
                paths.value(),
                "tracked client process was already gone"
            ));
        }
        return domain::fail<output::CommandOutput>(client_error(
            "stop_failed",
            "failed to stop TeamSpeak client: " + std::string(std::strerror(errno)),
            domain::ExitCode::internal
        ));
    }

    const auto wait_for_exit = [&](std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (process_is_running(*pid) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    };

    wait_for_exit(std::chrono::seconds(5));
    std::string note =
        discovered_note.empty() ? "client process group stopped with SIGTERM"
                                : discovered_note + "; stopped with SIGTERM";
    if (process_is_running(*pid) && force) {
        if (progress) {
            progress("The TeamSpeak client is still running, so SIGKILL is being sent because --force was set.");
        }
        if (!signal_client_target(*pid, SIGKILL) && errno != ESRCH) {
            return domain::fail<output::CommandOutput>(client_error(
                "force_stop_failed",
                "failed to force-stop TeamSpeak client: " + std::string(std::strerror(errno)),
                domain::ExitCode::internal
            ));
        }
        wait_for_exit(std::chrono::seconds(2));
        note = discovered_note.empty() ? "client process group stopped with SIGKILL"
                                       : discovered_note + "; stopped with SIGKILL";
    }

    if (process_is_running(*pid)) {
        return domain::fail<output::CommandOutput>(client_error(
            "stop_timeout",
            force ? "TeamSpeak client did not exit after SIGTERM and SIGKILL"
                  : "TeamSpeak client did not exit after SIGTERM; retry with ts client stop --force",
            domain::ExitCode::internal
        ));
    }

    remove_pid_file(paths.value().pid_file);
    remove_client_headless_script_file(paths.value().headless_script_file);
    return domain::ok(client_process_output(
        "stop",
        "stopped",
        *pid,
        paths.value(),
        note
    ));
}

auto parse_server_override(const std::string& raw, domain::Profile& profile) -> domain::Result<void> {
    if (raw.empty()) {
        return domain::fail(cli_error("invalid_server", "invalid --server value"));
    }

    if (raw.front() == '[') {
        const auto close = raw.find(']');
        if (close == std::string::npos || close == 1) {
            return domain::fail(cli_error("invalid_server", "invalid --server value"));
        }

        profile.host = raw.substr(0, close + 1);
        if (close + 1 == raw.size()) {
            return domain::ok();
        }

        if (raw[close + 1] != ':') {
            return domain::fail(cli_error("invalid_server", "invalid --server value"));
        }

        const auto port_text = raw.substr(close + 2);
        if (port_text.empty()) {
            return domain::fail(cli_error("invalid_server_port", "invalid port in --server"));
        }

        const auto port = util::parse_u16(port_text);
        if (!port.has_value()) {
            return domain::fail(cli_error("invalid_server_port", "invalid port in --server"));
        }
        profile.port = *port;
        return domain::ok();
    }

    const auto first_colon = raw.find(':');
    const auto last_colon = raw.rfind(':');
    if (first_colon == std::string::npos) {
        profile.host = raw;
        return domain::ok();
    }

    if (first_colon != last_colon) {
        profile.host = raw;
        return domain::ok();
    }

    profile.host = raw.substr(0, first_colon);
    const auto port_text = raw.substr(first_colon + 1);
    if (port_text.empty()) {
        return domain::ok();
    }

    const auto port = util::parse_u16(port_text);
    if (!port.has_value()) {
        return domain::fail(cli_error("invalid_server_port", "invalid port in --server"));
    }
    profile.port = *port;
    return domain::ok();
}

auto parse_positive_size(const std::map<std::string, std::string>& options, const std::string& key, std::size_t fallback)
    -> domain::Result<std::size_t> {
    const auto it = options.find(key);
    if (it == options.end()) {
        return domain::ok(fallback);
    }
    const auto parsed = util::parse_u64(it->second);
    if (!parsed.has_value() || *parsed == 0) {
        return domain::fail<std::size_t>(cli_error("invalid_number", "invalid value for --" + key));
    }
    return domain::ok(static_cast<std::size_t>(*parsed));
}

auto parse_timeout_ms(const std::map<std::string, std::string>& options, std::uint64_t fallback)
    -> domain::Result<std::chrono::milliseconds> {
    const auto it = options.find("timeout-ms");
    if (it == options.end()) {
        return domain::ok(std::chrono::milliseconds(fallback));
    }
    const auto parsed = util::parse_u64(it->second);
    if (!parsed.has_value()) {
        return domain::fail<std::chrono::milliseconds>(cli_error(
            "invalid_timeout", "invalid value for --timeout-ms"
        ));
    }
    return domain::ok(std::chrono::milliseconds(*parsed));
}

auto require_positional(const ParsedCommand& command, std::size_t index, const std::string& name)
    -> domain::Result<std::string> {
    if (command.positionals.size() <= index) {
        return domain::fail<std::string>(cli_error(
            "missing_argument", "missing required argument: " + name
        ));
    }
    return domain::ok(command.positionals[index]);
}

auto require_option(const ParsedCommand& command, const std::string& key) -> domain::Result<std::string> {
    const auto it = command.options.find(key);
    if (it == command.options.end() || it->second.empty()) {
        return domain::fail<std::string>(cli_error(
            "missing_option", "missing required option: --" + key
        ));
    }
    return domain::ok(it->second);
}

auto load_profile(
    const config::ConfigStore& store,
    const ParsedCommand& command
) -> domain::Result<ResolvedProfile> {
    const auto config_path =
        command.global.config_path.empty() ? store.default_path() : command.global.config_path;
    auto loaded = store.load_or_default(config_path);
    if (!loaded) {
        return domain::fail<ResolvedProfile>(loaded.error());
    }

    auto config = std::move(loaded.value());
    const std::string selected_name =
        command.global.profile.empty() ? config.active_profile : command.global.profile;
    auto found = store.find_profile(config, selected_name);
    if (!found) {
        return domain::fail<ResolvedProfile>(found.error());
    }

    domain::Profile profile = *found.value();
    if (command.global.server.has_value()) {
        const auto overridden = parse_server_override(*command.global.server, profile);
        if (!overridden) {
            return domain::fail<ResolvedProfile>(overridden.error());
        }
    }
    if (command.global.nickname.has_value()) {
        profile.nickname = *command.global.nickname;
    }
    if (command.global.identity.has_value()) {
        profile.identity = *command.global.identity;
    }

    return domain::ok(ResolvedProfile{
        .config_path = config_path,
        .config = std::move(config),
        .profile = std::move(profile),
    });
}

auto build_connect_request(const domain::Profile& profile) -> sdk::ConnectRequest {
    return sdk::ConnectRequest{
        .host = profile.host,
        .port = profile.port,
        .nickname = profile.nickname,
        .identity = profile.identity,
        .server_password = profile.server_password,
        .channel_password = profile.channel_password,
        .default_channel = profile.default_channel,
        .profile_name = profile.name,
    };
}

auto format_connect_target(const domain::Profile& profile) -> std::string {
    return profile.host + ":" + std::to_string(profile.port);
}

auto connect_start_message(const domain::Profile& profile) -> std::string {
    const std::string nickname = profile.nickname.empty() ? "the configured nickname" : profile.nickname;
    return "Connecting to " + format_connect_target(profile) + " as " + nickname +
           " using profile " + profile.name + "...";
}

auto build_init_options(const ParsedCommand& command, const domain::Profile& profile) -> sdk::InitOptions {
    return sdk::InitOptions{
        .verbose = command.global.verbose,
        .debug = command.global.debug,
        .socket_path = profile.control_socket_path,
    };
}

auto parse_poll_ms(const std::map<std::string, std::string>& options, std::uint64_t fallback)
    -> domain::Result<std::chrono::milliseconds> {
    const auto it = options.find("poll-ms");
    if (it == options.end()) {
        return domain::ok(std::chrono::milliseconds(fallback));
    }
    const auto parsed = util::parse_u64(it->second);
    if (!parsed.has_value() || *parsed == 0) {
        return domain::fail<std::chrono::milliseconds>(cli_error(
            "invalid_poll_ms", "invalid value for --poll-ms"
        ));
    }
    return domain::ok(std::chrono::milliseconds(*parsed));
}

auto daemon_command_error(
    std::string code,
    std::string message,
    domain::ExitCode exit_code = domain::ExitCode::internal
) -> domain::Error {
    return domain::make_error("daemon", std::move(code), std::move(message), exit_code);
}

auto resolve_daemon_paths() -> domain::Result<daemon::StatePaths> {
    auto state_dir = daemon::resolve_state_dir();
    if (!state_dir) {
        return domain::fail<daemon::StatePaths>(state_dir.error());
    }
    return domain::ok(daemon::state_paths_for(state_dir.value()));
}

auto ensure_directory(const std::filesystem::path& path, std::string_view what) -> domain::Result<void> {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        return domain::fail(daemon_command_error(
            "mkdir_failed", "failed to create " + std::string(what) + ": " + ec.message()
        ));
    }
    return domain::ok();
}

auto write_daemon_pid_file(const std::filesystem::path& pid_file, pid_t pid) -> domain::Result<void> {
    std::ofstream output(pid_file, std::ios::trunc);
    if (!output) {
        return domain::fail(daemon_command_error(
            "pid_file_write_failed", "failed to write daemon pid file: " + pid_file.string()
        ));
    }
    output << pid << '\n';
    return domain::ok();
}

auto daemon_status_value(const daemon::Status& status, const daemon::StatePaths& paths) -> output::ValueHolder {
    return output::make_object({
        {"running", output::make_bool(status.running)},
        {"pid", output::make_int(status.pid)},
        {"profile", output::make_string(status.profile)},
        {"backend", output::make_string(status.backend)},
        {"started_at", output::make_string(status.started_at)},
        {"last_event_at", output::make_string(status.last_event_at)},
        {"last_event_type", output::make_string(status.last_event_type)},
        {"last_error", output::make_string(status.last_error)},
        {"poll_ms", output::make_int(status.poll_interval.count())},
        {"inbox_count", output::make_int(static_cast<std::int64_t>(status.inbox_count))},
        {"hook_count", output::make_int(static_cast<std::int64_t>(status.hook_count))},
        {"state_dir", output::make_string(paths.state_dir.string())},
        {"log_file", output::make_string(paths.log_file.string())},
        {"pid_file", output::make_string(paths.pid_file.string())},
    });
}

auto daemon_status_human(const daemon::Status& status, const daemon::StatePaths& paths) -> std::string {
    std::ostringstream out;
    if (status.running && status.pid > 0) {
        out << "The TeamSpeak event daemon is running as PID " << status.pid << '.';
    } else {
        out << "The TeamSpeak event daemon is not running.";
    }

    if (!status.last_error.empty()) {
        out << "\n\nLast Error\n" << status.last_error;
    }

    out << "\n\nDaemon Context\n";
    out << output::render_details_block(output::Details{{
        {"State", status.running ? "running" : "stopped"},
        {"PID", status.pid > 0 ? std::to_string(status.pid) : "-"},
        {"Profile", status.profile.empty() ? "-" : status.profile},
        {"Backend", status.backend.empty() ? "-" : status.backend},
        {"StartedAt", status.started_at.empty() ? "-" : status.started_at},
        {"LastEventAt", status.last_event_at.empty() ? "-" : status.last_event_at},
        {"LastEventType", status.last_event_type.empty() ? "-" : status.last_event_type},
        {"PollMS", std::to_string(status.poll_interval.count())},
        {"InboxMessages", std::to_string(status.inbox_count)},
        {"Hooks", std::to_string(status.hook_count)},
        {"StateDir", paths.state_dir.string()},
        {"LogFile", paths.log_file.string()},
    }});
    return out.str();
}

auto hook_table(const std::vector<daemon::Hook>& hooks) -> output::Table {
    output::Table table{{"ID", "Event", "Kind", "Exec"}, {}};
    for (const auto& hook : hooks) {
        table.rows.push_back({
            hook.id,
            hook.event_type,
            hook.message_kind.empty() ? "*" : hook.message_kind,
            hook.command,
        });
    }
    return table;
}

auto message_sender(const domain::Event& event) -> std::string {
    if (const auto it = event.fields.find("from_name"); it != event.fields.end()) {
        return it->second;
    }
    if (const auto it = event.fields.find("from"); it != event.fields.end()) {
        return it->second;
    }
    return "-";
}

auto message_kind_text(const domain::Event& event) -> std::string {
    if (const auto it = event.fields.find("message_kind"); it != event.fields.end()) {
        return it->second;
    }
    if (const auto it = event.fields.find("target_mode"); it != event.fields.end()) {
        return it->second;
    }
    return "-";
}

auto message_text(const domain::Event& event) -> std::string {
    if (const auto it = event.fields.find("text"); it != event.fields.end()) {
        return it->second;
    }
    return event.summary;
}

auto inbox_table(const std::vector<domain::Event>& events) -> output::Table {
    output::Table table{{"Time", "From", "Kind", "Text"}, {}};
    for (const auto& event : events) {
        auto value = output::to_value(event);
        const auto* object = std::get_if<std::map<std::string, output::ValueHolder>>(&value.value);
        const auto* timestamp = object == nullptr ? nullptr : std::get_if<std::string>(&object->at("timestamp").value);
        table.rows.push_back({
            timestamp == nullptr ? "" : *timestamp,
            message_sender(event),
            message_kind_text(event),
            message_text(event),
        });
    }
    return table;
}

auto hook_value(const daemon::Hook& hook) -> output::ValueHolder {
    return output::make_object({
        {"id", output::make_string(hook.id)},
        {"type", output::make_string(hook.event_type)},
        {"message_kind", output::make_string(hook.message_kind)},
        {"exec", output::make_string(hook.command)},
    });
}

auto hooks_value(const std::vector<daemon::Hook>& hooks) -> output::ValueHolder {
    std::vector<output::ValueHolder> values;
    values.reserve(hooks.size());
    for (const auto& hook : hooks) {
        values.push_back(hook_value(hook));
    }
    return output::make_array(std::move(values));
}

auto next_hook_id(const std::vector<daemon::Hook>& hooks) -> std::string {
    std::uint64_t best = 0;
    for (const auto& hook : hooks) {
        if (const auto parsed = util::parse_u64(hook.id); parsed.has_value()) {
            best = std::max(best, *parsed);
        }
    }
    return std::to_string(best + 1);
}

auto size_to_i64(std::size_t value) -> std::int64_t {
    if (value > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(value);
}

auto playback_send_value(const bridge::PlaybackSendResult& result) -> output::ValueHolder {
    return output::make_object({
        {"result", output::make_string("sent")},
        {"file", output::make_string(result.file_path.string())},
        {"media_format", output::make_string(result.media_format)},
        {"media_socket_path", output::make_string(result.socket_path)},
        {"frames_sent", output::make_int(size_to_i64(result.frames_sent))},
        {"bytes_sent", output::make_int(size_to_i64(result.bytes_sent))},
        {"cleared_first", output::make_bool(result.cleared_first)},
        {"stop_reason", output::make_string(result.stop_reason)},
    });
}

auto playback_send_view(const bridge::PlaybackSendResult& result) -> std::string {
    std::ostringstream out;
    out << "Sent playback audio from " << result.file_path.string() << '.';
    out << "\n\nPlayback Context\n";
    out << output::render_details_block(output::Details{{
        {"Format", result.media_format},
        {"Frames", std::to_string(result.frames_sent)},
        {"Bytes", std::to_string(result.bytes_sent)},
        {"MediaSocketPath", result.socket_path},
        {"ClearedFirst", result.cleared_first ? "yes" : "no"},
        {"StopReason", result.stop_reason.empty() ? "-" : result.stop_reason},
    }});
    return out.str();
}

auto playback_status_view(const domain::MediaDiagnostics& diagnostics) -> std::string {
    std::ostringstream out;
    if (diagnostics.injected_playback_attached_to_capture) {
        out << "Injected playback is attached to the TeamSpeak custom capture transmit path.";
    } else if (diagnostics.playback_active) {
        out << "Playback is queued or active, but it is not currently attached to the custom capture transmit path.";
    } else if (diagnostics.transmit_path_ready) {
        out << "The TeamSpeak custom capture transmit path is available for playback injection.";
    } else {
        out << "The TeamSpeak custom capture transmit path is not ready.";
    }
    out << "\n\nMedia Diagnostics\n";
    out << output::render_details_block(output::media_diagnostics_view(diagnostics));
    return out.str();
}

template <typename Func>
auto with_session(
    const ParsedCommand& command,
    const config::ConfigStore& store,
    const sdk::BackendFactory& factory,
    Func&& callback
) -> domain::Result<output::CommandOutput> {
    auto resolved = load_profile(store, command);
    if (!resolved) {
        return domain::fail<output::CommandOutput>(resolved.error());
    }

    auto backend = factory.create(resolved.value().profile.backend);
    if (!backend) {
        return domain::fail<output::CommandOutput>(backend.error());
    }

    session::SessionService session(std::move(backend.value()));
    const auto init_result = session.initialize(build_init_options(command, resolved.value().profile));
    if (!init_result) {
        return domain::fail<output::CommandOutput>(init_result.error());
    }
    auto shutdown_guard = util::make_scope_exit([&] {
        const auto ignored = session.shutdown();
        (void)ignored;
    });

    return callback(session, resolved.value());
}

auto group_clients_by_channel(
    const std::vector<domain::Channel>& channels,
    const std::vector<domain::Client>& clients
) -> std::vector<ChannelClientGroup> {
    std::vector<ChannelClientGroup> groups;
    groups.reserve(channels.size());

    for (const auto& channel : channels) {
        ChannelClientGroup group{
            .channel = channel,
            .clients = {},
        };
        for (const auto& client : clients) {
            if (client.channel_id.has_value() && *client.channel_id == channel.id) {
                group.clients.push_back(client);
            }
        }
        groups.push_back(std::move(group));
    }

    return groups;
}

auto to_value(const ChannelClientGroup& group) -> output::ValueHolder {
    return output::make_object({
        {"channel", output::to_value(group.channel)},
        {"clients", output::to_value(group.clients)},
    });
}

auto to_value(const std::vector<ChannelClientGroup>& groups) -> output::ValueHolder {
    std::vector<output::ValueHolder> items;
    items.reserve(groups.size());
    for (const auto& group : groups) {
        items.push_back(to_value(group));
    }
    return output::make_array(std::move(items));
}

auto channel_clients_table(const std::vector<ChannelClientGroup>& groups) -> output::Table {
    output::Table table{{"ChannelID", "Channel", "ClientID", "Nickname", "Self", "Talking"}, {}};

    for (const auto& group : groups) {
        if (group.clients.empty()) {
            table.rows.push_back({
                domain::to_string(group.channel.id),
                group.channel.name,
                "-",
                "-",
                "-",
                "-",
            });
            continue;
        }

        for (const auto& client : group.clients) {
            table.rows.push_back({
                domain::to_string(group.channel.id),
                group.channel.name,
                domain::to_string(client.id),
                client.nickname,
                client.self ? "yes" : "no",
                client.talking ? "yes" : "no",
            });
        }
    }

    return table;
}

auto resolve_client_launch_socket_path(
    const ParsedCommand& command,
    const config::ConfigStore& store
) -> domain::Result<std::string> {
    auto resolved = load_profile(store, command);
    if (!resolved) {
        return domain::fail<std::string>(resolved.error());
    }
    return domain::ok(bridge::resolve_socket_path(resolved.value().profile.control_socket_path));
}

auto top_level_help() -> std::string {
    std::ostringstream out;
    out << "TeamSpeak client plugin CLI\n\n";
    out << "Usage:\n";
    out << "  ts [global options] <command> [args]\n\n";
    out << "Global options:\n";
    out << "  --output <table|json|yaml>  yaml is experimental\n";
    out << "  --json\n";
    out << "  --field <path>  extract a scalar field from JSON output\n";
    out << "  --profile <name>\n";
    out << "  --server <host[:port]>\n";
    out << "  --nickname <name>\n";
    out << "  --identity <value>\n";
    out << "  --config <path>\n";
    out << "  --verbose\n";
    out << "  --debug\n";
    out << "  --help\n\n";
    out << "Commands:\n";
    for (const auto& doc : command_docs()) {
        if (doc.path.size() != 1) {
            continue;
        }
        out << "  " << doc.path.front() << "  " << doc.summary << '\n';
    }
    out << "\nRun `ts <command> --help` for command-specific help.\n";
    out << "\nThis CLI talks to a TeamSpeak 3 client plugin over a local control socket.\n";
    out << "It is not ServerQuery, WebQuery, or a standalone TeamSpeak SDK ClientLib client.\n";
    return out.str();
}

}  // namespace

auto read_install_receipt_value_for_test(std::string_view value) -> std::string {
    if (const auto decoded = decode_install_receipt_value(value); decoded.has_value()) {
        return *decoded;
    }
    return std::string(value);
}

auto find_executable_with_fallbacks_for_test(
    std::string_view executable_name,
    const std::vector<std::filesystem::path>& fallback_paths
) -> std::optional<std::filesystem::path> {
    return find_executable(executable_name, fallback_paths);
}

CommandRouter::CommandRouter() = default;

auto CommandRouter::parse(int argc, char** argv) const -> domain::Result<ParsedCommand> {
    ParsedCommand parsed;
    parsed.global.config_path = config_store_.default_path();

    std::vector<std::string> args;
    for (int index = 1; index < argc; ++index) {
        args.emplace_back(argv[index]);
    }

    if (args.empty()) {
        parsed.show_help = true;
        return domain::ok(parsed);
    }

    auto consume_option = [&](std::size_t& index, const std::string& flag) -> domain::Result<std::string> {
        if (index + 1 >= args.size()) {
            return domain::fail<std::string>(cli_error(
                "missing_option_value", "missing value for " + flag
            ));
        }
        ++index;
        return domain::ok(args[index]);
    };

    std::size_t index = 0;
    for (; index < args.size(); ++index) {
        const auto& token = args[index];
        if (token == "--help" || token == "-h") {
            parsed.show_help = true;
            continue;
        }
        if (token == "--version") {
            parsed.path = {"version"};
            return domain::ok(parsed);
        }
        if (token == "--json") {
            parsed.global.format = output::Format::json;
            continue;
        }
        if (token == "--verbose") {
            parsed.global.verbose = true;
            continue;
        }
        if (token == "--debug") {
            parsed.global.debug = true;
            continue;
        }
        if (token == "--output") {
            auto value = consume_option(index, token);
            if (!value) {
                return domain::fail<ParsedCommand>(value.error());
            }
            auto format = output::parse_format(value.value());
            if (!format) {
                return domain::fail<ParsedCommand>(format.error());
            }
            parsed.global.format = format.value();
            continue;
        }
        if (token == "--field") {
            auto value = consume_option(index, token);
            if (!value) {
                return domain::fail<ParsedCommand>(value.error());
            }
            parsed.global.field_path = value.value();
            continue;
        }
        if (token == "--profile") {
            auto value = consume_option(index, token);
            if (!value) {
                return domain::fail<ParsedCommand>(value.error());
            }
            parsed.global.profile = value.value();
            continue;
        }
        if (token == "--server") {
            auto value = consume_option(index, token);
            if (!value) {
                return domain::fail<ParsedCommand>(value.error());
            }
            parsed.global.server = value.value();
            continue;
        }
        if (token == "--nickname") {
            auto value = consume_option(index, token);
            if (!value) {
                return domain::fail<ParsedCommand>(value.error());
            }
            parsed.global.nickname = value.value();
            continue;
        }
        if (token == "--identity") {
            auto value = consume_option(index, token);
            if (!value) {
                return domain::fail<ParsedCommand>(value.error());
            }
            parsed.global.identity = value.value();
            continue;
        }
        if (token == "--config") {
            auto value = consume_option(index, token);
            if (!value) {
                return domain::fail<ParsedCommand>(value.error());
            }
            parsed.global.config_path = value.value();
            continue;
        }
        break;
    }

    if (index >= args.size()) {
        parsed.show_help = true;
        return domain::ok(parsed);
    }

    std::vector<std::string> remaining(args.begin() + static_cast<long>(index), args.end());
    std::size_t longest_match = 0;
    for (std::size_t candidate_size = 1; candidate_size <= remaining.size(); ++candidate_size) {
        const std::vector<std::string> candidate(
            remaining.begin(), remaining.begin() + static_cast<long>(candidate_size)
        );
        if (!has_command_prefix(candidate)) {
            break;
        }
        if (find_command_doc(candidate) != nullptr) {
            longest_match = candidate_size;
        }
    }

    if (longest_match == 0) {
        return domain::fail<ParsedCommand>(cli_error(
            "unknown_command", "unknown command: " + remaining.front()
        ));
    }

    parsed.path.assign(remaining.begin(), remaining.begin() + static_cast<long>(longest_match));
    std::vector<std::string> tail(remaining.begin() + static_cast<long>(longest_match), remaining.end());
    const bool group_command = has_subcommands(parsed.path);

    if (group_command && !tail.empty()) {
        const auto& next = tail.front();
        if (next != "--help" && next != "-h" && next.rfind("--", 0) != 0) {
            std::vector<std::string> attempted = parsed.path;
            attempted.push_back(next);
            return domain::fail<ParsedCommand>(cli_error(
                "unknown_command", "unknown command: " + format_command_path(attempted)
            ));
        }
    }

    for (std::size_t tail_index = 0; tail_index < tail.size(); ++tail_index) {
        const auto& token = tail[tail_index];
        if (token == "--help" || token == "-h") {
            parsed.show_help = true;
            continue;
        }
        if (token == "--json") {
            parsed.global.format = output::Format::json;
            continue;
        }
        if (token == "--verbose") {
            parsed.global.verbose = true;
            continue;
        }
        if (token == "--debug") {
            parsed.global.debug = true;
            continue;
        }
        if (token.rfind("--", 0) == 0) {
            const std::string option = token.substr(2);
            const bool takes_value =
                option == "output" || option == "profile" || option == "server" ||
                option == "nickname" || option == "identity" || option == "config" ||
                option == "field" ||
                option == "target" || option == "id" || option == "text" ||
                option == "count" || option == "timeout-ms" || option == "poll-ms" ||
                option == "type" || option == "exec" || option == "message-kind" ||
                option == "copy-from" || option == "message" || option == "file" ||
                option == "release-tag";

            if (!takes_value) {
                parsed.flags.insert(option);
                continue;
            }
            if (tail_index + 1 >= tail.size()) {
                return domain::fail<ParsedCommand>(cli_error(
                    "missing_option_value", "missing value for " + token
                ));
            }
            const std::string value = tail[++tail_index];
            if (option == "output") {
                auto format = output::parse_format(value);
                if (!format) {
                    return domain::fail<ParsedCommand>(format.error());
                }
                parsed.global.format = format.value();
            } else if (option == "field") {
                parsed.global.field_path = value;
            } else if (option == "profile") {
                parsed.global.profile = value;
            } else if (option == "server") {
                parsed.global.server = value;
            } else if (option == "nickname") {
                parsed.global.nickname = value;
            } else if (option == "identity") {
                parsed.global.identity = value;
            } else if (option == "config") {
                parsed.global.config_path = value;
            } else {
                parsed.options[option] = value;
            }
            continue;
        }

        parsed.positionals.push_back(token);
    }

    if (group_command) {
        parsed.show_help = true;
    }

    if (parsed.global.field_path.has_value() && parsed.global.format != output::Format::json &&
        !parsed.show_help) {
        return domain::fail<ParsedCommand>(cli_error(
            "field_requires_json", "--field requires JSON output; use --json or --output json"
        ));
    }

    return domain::ok(parsed);
}

auto CommandRouter::render_help(const std::vector<std::string>& path) const -> std::string {
    if (path.empty()) {
        return top_level_help();
    }

    const auto* doc = find_command_doc(path);
    if (doc == nullptr) {
        return top_level_help();
    }

    std::ostringstream out;
    out << doc->summary << "\n\n";
    out << "Usage:\n";
    out << "  " << doc->usage << '\n';
    if (!doc->options.empty()) {
        out << "\nOptions:\n";
        for (const auto& option : doc->options) {
            out << "  " << option << '\n';
        }
    }

    const auto children = immediate_subcommands(path);
    if (!children.empty()) {
        out << "\nSubcommands:\n";
        for (const auto* child : children) {
            out << "  " << child->path.back() << "  " << child->summary << '\n';
        }

        out << "\nExamples:\n";
        for (const auto* child : children) {
            out << "  " << child->usage << '\n';
        }
    }

    out << "\nGlobal options: --output (yaml experimental) --json --field --profile --server --nickname --identity --config --verbose --debug --help\n";
    return out.str();
}

auto CommandRouter::dispatch(const ParsedCommand& command, const ProgressSink& progress)
    -> domain::Result<output::CommandOutput> {
    const auto path = util::join(command.path, " ");

    const auto result = [&]() -> domain::Result<output::CommandOutput> {
        if (path == "version") {
            const std::vector<std::string> backends = {"mock", "plugin"};
            std::vector<output::ValueHolder> backend_values;
            for (const auto& backend : backends) {
                backend_values.push_back(output::make_string(backend));
            }
            return domain::ok(output::CommandOutput{
                .data = output::make_object({
                    {"name", output::make_string("ts")},
                    {"version", output::make_string(TSCLI_VERSION)},
                    {"compiled_backends", output::make_array(std::move(backend_values))},
                }),
                .human = std::string("ts " TSCLI_VERSION),
            });
        }

        if (path == "update") {
            return update_current_release_install(
                command,
                command.global.format == output::Format::table ? progress : ProgressSink{}
            );
        }

        if (path == "config init") {
            const bool force = command.flags.contains("force");
            const auto config_path =
                command.global.config_path.empty() ? config_store_.default_path() : command.global.config_path;
            auto config = config_store_.init(config_path, force);
            if (!config) {
                return domain::fail<output::CommandOutput>(config.error());
            }
            return domain::ok(output::CommandOutput{
                .data = output::make_object({
                    {"path", output::make_string(config_path.string())},
                    {"active_profile", output::make_string(config.value().active_profile)},
                }),
                .human = std::string("initialized config at " + config_path.string()),
            });
        }

        if (path == "config view") {
            const auto config_path =
                command.global.config_path.empty() ? config_store_.default_path() : command.global.config_path;
            auto loaded = config_store_.load_or_default(config_path);
            if (!loaded) {
                return domain::fail<output::CommandOutput>(loaded.error());
            }
            return domain::ok(output::CommandOutput{
                .data = output::make_object({
                    {"path", output::make_string(config_path.string())},
                    {"active_profile", output::make_string(loaded.value().active_profile)},
                    {"profiles", output::to_value(loaded.value().profiles)},
                }),
                .human = output::profile_table(loaded.value().profiles, loaded.value().active_profile),
            });
        }

        if (path == "profile list") {
            auto resolved = load_profile(config_store_, command);
            if (!resolved) {
                return domain::fail<output::CommandOutput>(resolved.error());
            }
            return domain::ok(output::CommandOutput{
                .data = output::to_value(resolved.value().config.profiles),
                .human = output::profile_table(
                    resolved.value().config.profiles, resolved.value().config.active_profile
                ),
            });
        }

        if (path == "profile create") {
            auto name = require_positional(command, 0, "name");
            if (!name) {
                return domain::fail<output::CommandOutput>(name.error());
            }
            const auto config_path =
                command.global.config_path.empty() ? config_store_.default_path() : command.global.config_path;
            auto loaded = config_store_.load_or_default(config_path);
            if (!loaded) {
                return domain::fail<output::CommandOutput>(loaded.error());
            }

            const std::string source_name =
                command.options.contains("copy-from") ? command.options.at("copy-from")
                                                      : loaded.value().active_profile;
            auto source = config_store_.find_profile(loaded.value(), source_name);
            if (!source) {
                return domain::fail<output::CommandOutput>(source.error());
            }

            domain::Profile profile = *source.value();
            profile.name = name.value();

            auto created = config_store_.create_profile(loaded.value(), std::move(profile));
            if (!created) {
                return domain::fail<output::CommandOutput>(created.error());
            }

            if (command.flags.contains("activate")) {
                loaded.value().active_profile = created.value()->name;
            }

            const auto saved = config_store_.save(config_path, loaded.value());
            if (!saved) {
                return domain::fail<output::CommandOutput>(saved.error());
            }

            return domain::ok(output::CommandOutput{
                .data = output::make_object({
                    {"path", output::make_string(config_path.string())},
                    {"active_profile", output::make_string(loaded.value().active_profile)},
                    {"copied_from", output::make_string(source_name)},
                    {"profile", output::to_value(*created.value())},
                }),
                .human =
                    command.flags.contains("activate")
                        ? std::string(
                              "created profile " + created.value()->name + " from " + source_name +
                              " and set it as the default"
                          )
                        : std::string("created profile " + created.value()->name + " from " + source_name),
            });
        }

        if (path == "profile use") {
            auto name = require_positional(command, 0, "name");
            if (!name) {
                return domain::fail<output::CommandOutput>(name.error());
            }
            const auto config_path =
                command.global.config_path.empty() ? config_store_.default_path() : command.global.config_path;
            auto loaded = config_store_.load_or_default(config_path);
            if (!loaded) {
                return domain::fail<output::CommandOutput>(loaded.error());
            }
            auto found = config_store_.find_profile(loaded.value(), name.value());
            if (!found) {
                return domain::fail<output::CommandOutput>(found.error());
            }
            loaded.value().active_profile = found.value()->name;
            const auto saved = config_store_.save(config_path, loaded.value());
            if (!saved) {
                return domain::fail<output::CommandOutput>(saved.error());
            }
            return domain::ok(output::CommandOutput{
                .data = output::make_object({
                    {"active_profile", output::make_string(found.value()->name)},
                    {"path", output::make_string(config_path.string())},
                }),
                .human = std::string("active profile set to " + found.value()->name),
            });
        }

        if (path == "daemon start") {
            auto resolved = load_profile(config_store_, command);
            if (!resolved) {
                return domain::fail<output::CommandOutput>(resolved.error());
            }
            auto poll_interval = parse_poll_ms(command.options, 500);
            if (!poll_interval) {
                return domain::fail<output::CommandOutput>(poll_interval.error());
            }
            auto paths = resolve_daemon_paths();
            if (!paths) {
                return domain::fail<output::CommandOutput>(paths.error());
            }
            auto ensured = ensure_directory(paths.value().state_dir, "daemon state directory");
            if (!ensured) {
                return domain::fail<output::CommandOutput>(ensured.error());
            }

            if (const auto pid = read_pid_file(paths.value().pid_file); pid.has_value()) {
                if (process_is_running(*pid)) {
                    return domain::fail<output::CommandOutput>(daemon_command_error(
                        "already_running",
                        "the TeamSpeak event daemon is already running as PID " + std::to_string(*pid),
                        domain::ExitCode::unsupported
                    ));
                }
                remove_pid_file(paths.value().pid_file);
            }

            auto backend = backend_factory_.create(resolved.value().profile.backend);
            if (!backend) {
                return domain::fail<output::CommandOutput>(backend.error());
            }

            const auto init_options = build_init_options(command, resolved.value().profile);
            const bool foreground = command.flags.contains("foreground");

            if (foreground) {
                auto wrote_pid = write_daemon_pid_file(paths.value().pid_file, ::getpid());
                if (!wrote_pid) {
                    return domain::fail<output::CommandOutput>(wrote_pid.error());
                }
                auto pid_guard = util::make_scope_exit([&] { remove_pid_file(paths.value().pid_file); });

                g_daemon_stop_requested = 0;
                auto* previous_int = std::signal(SIGINT, daemon_signal_handler);
                auto* previous_term = std::signal(SIGTERM, daemon_signal_handler);
                auto signal_guard = util::make_scope_exit([&] {
                    std::signal(SIGINT, previous_int);
                    std::signal(SIGTERM, previous_term);
                });

                auto ran = daemon::run_event_daemon(
                    resolved.value().profile,
                    init_options,
                    paths.value(),
                    poll_interval.value(),
                    [] { return g_daemon_stop_requested != 0; },
                    backend_factory_
                );
                if (!ran) {
                    return domain::fail<output::CommandOutput>(ran.error());
                }

                auto status = daemon::read_status(paths.value());
                if (!status) {
                    return domain::fail<output::CommandOutput>(status.error());
                }
                return domain::ok(output::CommandOutput{
                    .data = daemon_status_value(status.value(), paths.value()),
                    .human = daemon_status_human(status.value(), paths.value()),
                });
            }

            const pid_t child_pid = ::fork();
            if (child_pid < 0) {
                return domain::fail<output::CommandOutput>(daemon_command_error(
                    "fork_failed",
                    "failed to launch the TeamSpeak event daemon: " + std::string(std::strerror(errno))
                ));
            }

            if (child_pid == 0) {
                if (::setsid() < 0) {
                    _exit(1);
                }

                const int stdin_fd = ::open("/dev/null", O_RDONLY);
                const int log_fd =
                    ::open(paths.value().log_file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (stdin_fd < 0 || log_fd < 0 || ::dup2(stdin_fd, STDIN_FILENO) == -1 ||
                    ::dup2(log_fd, STDOUT_FILENO) == -1 || ::dup2(log_fd, STDERR_FILENO) == -1) {
                    _exit(1);
                }
                ::close(stdin_fd);
                ::close(log_fd);

                auto wrote_pid = write_daemon_pid_file(paths.value().pid_file, ::getpid());
                if (!wrote_pid) {
                    _exit(static_cast<int>(wrote_pid.error().exit_code));
                }
                auto pid_guard = util::make_scope_exit([&] { remove_pid_file(paths.value().pid_file); });

                g_daemon_stop_requested = 0;
                std::signal(SIGINT, daemon_signal_handler);
                std::signal(SIGTERM, daemon_signal_handler);

                auto ran = daemon::run_event_daemon(
                    resolved.value().profile,
                    init_options,
                    paths.value(),
                    poll_interval.value(),
                    [] { return g_daemon_stop_requested != 0; },
                    backend_factory_
                );
                if (!ran) {
                    std::cerr << ran.error().message << '\n';
                    _exit(static_cast<int>(ran.error().exit_code));
                }
                _exit(0);
            }

            auto wrote_pid = write_daemon_pid_file(paths.value().pid_file, child_pid);
            if (!wrote_pid) {
                (void)::kill(child_pid, SIGTERM);
                return domain::fail<output::CommandOutput>(wrote_pid.error());
            }

            return domain::ok(output::CommandOutput{
                .data = output::make_object({
                    {"result", output::make_string("started")},
                    {"pid", output::make_int(child_pid)},
                    {"profile", output::make_string(resolved.value().profile.name)},
                    {"backend", output::make_string(resolved.value().profile.backend)},
                    {"poll_ms", output::make_int(poll_interval.value().count())},
                    {"state_dir", output::make_string(paths.value().state_dir.string())},
                    {"log_file", output::make_string(paths.value().log_file.string())},
                }),
                .human = std::string(
                    "Started the TeamSpeak event daemon as PID " + std::to_string(child_pid) + '.'
                ),
            });
        }

        if (path == "daemon stop") {
            auto paths = resolve_daemon_paths();
            if (!paths) {
                return domain::fail<output::CommandOutput>(paths.error());
            }
            auto timeout = parse_timeout_ms(command.options, 3000);
            if (!timeout) {
                return domain::fail<output::CommandOutput>(timeout.error());
            }

            const auto pid = read_pid_file(paths.value().pid_file);
            if (!pid.has_value()) {
                auto status = daemon::read_status(paths.value());
                if (!status) {
                    return domain::fail<output::CommandOutput>(status.error());
                }
                return domain::ok(output::CommandOutput{
                    .data = daemon_status_value(status.value(), paths.value()),
                    .human = daemon_status_human(status.value(), paths.value()),
                });
            }

            if (!process_is_running(*pid)) {
                remove_pid_file(paths.value().pid_file);
            } else if (::kill(*pid, SIGTERM) != 0 && errno != ESRCH) {
                return domain::fail<output::CommandOutput>(daemon_command_error(
                    "signal_failed",
                    "failed to stop the TeamSpeak event daemon: " + std::string(std::strerror(errno))
                ));
            }

            const auto deadline = std::chrono::steady_clock::now() + timeout.value();
            while (process_is_running(*pid) && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            if (process_is_running(*pid)) {
                return domain::fail<output::CommandOutput>(daemon_command_error(
                    "stop_timeout",
                    "the TeamSpeak event daemon did not stop within " +
                        std::to_string(timeout.value().count()) + " ms",
                    domain::ExitCode::connection
                ));
            }

            remove_pid_file(paths.value().pid_file);
            auto status = daemon::read_status(paths.value());
            if (!status) {
                return domain::fail<output::CommandOutput>(status.error());
            }
            status.value().running = false;
            status.value().pid = 0;
            const auto wrote = daemon::write_status(paths.value(), status.value());
            if (!wrote) {
                return domain::fail<output::CommandOutput>(wrote.error());
            }
            return domain::ok(output::CommandOutput{
                .data = daemon_status_value(status.value(), paths.value()),
                .human = daemon_status_human(status.value(), paths.value()),
            });
        }

        if (path == "daemon status") {
            auto paths = resolve_daemon_paths();
            if (!paths) {
                return domain::fail<output::CommandOutput>(paths.error());
            }
            auto status = daemon::read_status(paths.value());
            if (!status) {
                return domain::fail<output::CommandOutput>(status.error());
            }

            if (const auto pid = read_pid_file(paths.value().pid_file); pid.has_value()) {
                if (process_is_running(*pid)) {
                    status.value().running = true;
                    status.value().pid = *pid;
                } else {
                    remove_pid_file(paths.value().pid_file);
                    status.value().running = false;
                    status.value().pid = 0;
                    const auto wrote = daemon::write_status(paths.value(), status.value());
                    if (!wrote) {
                        return domain::fail<output::CommandOutput>(wrote.error());
                    }
                }
            } else {
                status.value().running = false;
                status.value().pid = 0;
            }

            return domain::ok(output::CommandOutput{
                .data = daemon_status_value(status.value(), paths.value()),
                .human = daemon_status_human(status.value(), paths.value()),
            });
        }

        if (path == "message inbox") {
            auto count = parse_positive_size(command.options, "count", 20);
            if (!count) {
                return domain::fail<output::CommandOutput>(count.error());
            }
            auto paths = resolve_daemon_paths();
            if (!paths) {
                return domain::fail<output::CommandOutput>(paths.error());
            }
            auto messages = daemon::read_inbox(paths.value(), count.value());
            if (!messages) {
                return domain::fail<output::CommandOutput>(messages.error());
            }
            return domain::ok(output::CommandOutput{
                .data = output::to_value(messages.value()),
                .human = inbox_table(messages.value()),
            });
        }

        if (path == "events hook add") {
            auto type = require_option(command, "type");
            auto exec = require_option(command, "exec");
            if (!type) {
                return domain::fail<output::CommandOutput>(type.error());
            }
            if (!exec) {
                return domain::fail<output::CommandOutput>(exec.error());
            }

            std::string message_kind;
            if (const auto it = command.options.find("message-kind"); it != command.options.end()) {
                if (it->second != "client" && it->second != "channel" && it->second != "server") {
                    return domain::fail<output::CommandOutput>(cli_error(
                        "invalid_message_kind", "--message-kind must be client, channel, or server"
                    ));
                }
                message_kind = it->second;
            }

            auto paths = resolve_daemon_paths();
            if (!paths) {
                return domain::fail<output::CommandOutput>(paths.error());
            }
            auto hooks = daemon::load_hooks(paths.value());
            if (!hooks) {
                return domain::fail<output::CommandOutput>(hooks.error());
            }

            daemon::Hook hook{
                .id = next_hook_id(hooks.value()),
                .event_type = type.value(),
                .message_kind = std::move(message_kind),
                .command = exec.value(),
            };
            hooks.value().push_back(hook);
            auto saved = daemon::save_hooks(paths.value(), hooks.value());
            if (!saved) {
                return domain::fail<output::CommandOutput>(saved.error());
            }
            return domain::ok(output::CommandOutput{
                .data = hook_value(hook),
                .human = std::string("added daemon hook " + hook.id),
            });
        }

        if (path == "events hook list") {
            auto paths = resolve_daemon_paths();
            if (!paths) {
                return domain::fail<output::CommandOutput>(paths.error());
            }
            auto hooks = daemon::load_hooks(paths.value());
            if (!hooks) {
                return domain::fail<output::CommandOutput>(hooks.error());
            }
            return domain::ok(output::CommandOutput{
                .data = hooks_value(hooks.value()),
                .human = hook_table(hooks.value()),
            });
        }

        if (path == "events hook remove") {
            auto id = require_positional(command, 0, "id");
            if (!id) {
                return domain::fail<output::CommandOutput>(id.error());
            }
            auto paths = resolve_daemon_paths();
            if (!paths) {
                return domain::fail<output::CommandOutput>(paths.error());
            }
            auto hooks = daemon::load_hooks(paths.value());
            if (!hooks) {
                return domain::fail<output::CommandOutput>(hooks.error());
            }

            const auto original_size = hooks.value().size();
            hooks.value().erase(
                std::remove_if(
                    hooks.value().begin(),
                    hooks.value().end(),
                    [&](const daemon::Hook& hook) { return hook.id == id.value(); }
                ),
                hooks.value().end()
            );
            if (hooks.value().size() == original_size) {
                return domain::fail<output::CommandOutput>(daemon_command_error(
                    "hook_not_found", "hook not found: " + id.value(), domain::ExitCode::not_found
                ));
            }

            auto saved = daemon::save_hooks(paths.value(), hooks.value());
            if (!saved) {
                return domain::fail<output::CommandOutput>(saved.error());
            }
            return domain::ok(output::CommandOutput{
                .data = output::make_object({{"id", output::make_string(id.value())}}),
                .human = std::string("removed daemon hook " + id.value()),
            });
        }

        if (path == "completion") {
            auto shell = require_positional(command, 0, "shell");
            if (!shell) {
                return domain::fail<output::CommandOutput>(shell.error());
            }
            auto script = completion::generate(shell.value());
            if (!script) {
                return domain::fail<output::CommandOutput>(script.error());
            }
            return domain::ok(output::CommandOutput{
                .data = output::make_object({
                    {"shell", output::make_string(shell.value())},
                    {"script", output::make_string(script.value())},
                }),
                .human = script.value(),
            });
        }

        if (path == "plugin info" || path == "sdk info") {
            return with_session(command, config_store_, backend_factory_, [](auto& session, const auto&) {
                auto info = session.plugin_info();
                if (!info) {
                    return domain::fail<output::CommandOutput>(info.error());
                }
                return domain::ok(output::CommandOutput{
                    .data = output::to_value(info.value()),
                    .human = output::plugin_info_view(info.value()),
                });
            });
        }

        if (path == "connect") {
            return with_session(command, config_store_, backend_factory_, [&](auto& session, const auto& resolved) {
                const bool stream_progress = static_cast<bool>(progress) && command.global.format == output::Format::table;
                if (stream_progress) {
                    progress(connect_start_message(resolved.profile));
                }
                auto connected = session.connect_and_wait(
                    build_connect_request(resolved.profile),
                    kConnectCompletionTimeout,
                    stream_progress ? session::ConnectEventCallback([&](const domain::Event& event) {
                        progress(output::connect_progress_message(event));
                    })
                                    : session::ConnectEventCallback{}
                );
                if (!connected) {
                    return domain::fail<output::CommandOutput>(connected.error());
                }
                const auto result_name = connected.value().connected
                    ? "connected"
                    : connected.value().timed_out ? "timeout" : "failed";
                const auto exit_code = connected.value().connected ? domain::ExitCode::ok
                                                                   : domain::ExitCode::connection;
                return domain::ok(output::CommandOutput{
                    .data = output::make_object({
                        {"result", output::make_string(result_name)},
                        {"connected", output::make_bool(connected.value().connected)},
                        {"timed_out", output::make_bool(connected.value().timed_out)},
                        {"timeout_ms", output::make_int(connected.value().timeout.count())},
                        {"state", output::to_value(connected.value().state)},
                        {"lifecycle", output::to_value(connected.value().lifecycle)},
                    }),
                    .human = output::connect_view(
                        connected.value().state,
                        connected.value().lifecycle,
                        connected.value().connected,
                        connected.value().timed_out,
                        connected.value().timeout,
                        !stream_progress
                    ),
                    .exit_code = exit_code,
                });
            });
        }

        if (path == "disconnect") {
            return with_session(command, config_store_, backend_factory_, [&](auto& session, const auto&) {
                const bool stream_progress = static_cast<bool>(progress) && command.global.format == output::Format::table;
                if (stream_progress) {
                    progress("Requesting disconnect from the current TeamSpeak server.");
                }
                auto disconnected = session.disconnect_and_wait(
                    "ts disconnect",
                    kDisconnectCompletionTimeout,
                    stream_progress ? session::ConnectEventCallback([&](const domain::Event& event) {
                        progress(output::disconnect_progress_message(event));
                    })
                                    : session::ConnectEventCallback{}
                );
                if (!disconnected) {
                    return domain::fail<output::CommandOutput>(disconnected.error());
                }
                const auto result_name = disconnected.value().disconnected
                    ? "disconnected"
                    : disconnected.value().timed_out ? "timeout" : "failed";
                const auto exit_code = disconnected.value().disconnected ? domain::ExitCode::ok
                                                                         : domain::ExitCode::connection;
                return domain::ok(output::CommandOutput{
                    .data = output::make_object({
                        {"result", output::make_string(result_name)},
                        {"disconnected", output::make_bool(disconnected.value().disconnected)},
                        {"timed_out", output::make_bool(disconnected.value().timed_out)},
                        {"timeout_ms", output::make_int(disconnected.value().timeout.count())},
                        {"state", output::to_value(disconnected.value().state)},
                        {"lifecycle", output::to_value(disconnected.value().lifecycle)},
                    }),
                    .human = output::disconnect_view(
                        disconnected.value().state,
                        disconnected.value().lifecycle,
                        disconnected.value().disconnected,
                        disconnected.value().timed_out,
                        disconnected.value().timeout,
                        !stream_progress
                    ),
                    .exit_code = exit_code,
                });
            });
        }

        if (path == "mute") {
            return with_session(command, config_store_, backend_factory_, [](auto& session, const auto&) {
                auto muted = session.set_self_muted(true);
                if (!muted) {
                    return domain::fail<output::CommandOutput>(muted.error());
                }
                return domain::ok(output::CommandOutput{
                    .data = output::make_object({
                        {"muted", output::make_bool(true)},
                    }),
                    .human = std::string("Muted your TeamSpeak microphone."),
                });
            });
        }

        if (path == "unmute") {
            return with_session(command, config_store_, backend_factory_, [](auto& session, const auto&) {
                auto muted = session.set_self_muted(false);
                if (!muted) {
                    return domain::fail<output::CommandOutput>(muted.error());
                }
                return domain::ok(output::CommandOutput{
                    .data = output::make_object({
                        {"muted", output::make_bool(false)},
                    }),
                    .human = std::string("Unmuted your TeamSpeak microphone."),
                });
            });
        }

        if (path == "away") {
            const std::string message = command.options.contains("message") ? command.options.at("message")
                                                                            : std::string{};
            return with_session(command, config_store_, backend_factory_, [&](auto& session, const auto&) {
                auto away = session.set_self_away(true, message);
                if (!away) {
                    return domain::fail<output::CommandOutput>(away.error());
                }
                return domain::ok(output::CommandOutput{
                    .data = output::make_object({
                        {"away", output::make_bool(true)},
                        {"message", output::make_string(message)},
                    }),
                    .human = message.empty() ? std::string("Set your TeamSpeak status to away.")
                                             : "Set your TeamSpeak status to away: " + message,
                });
            });
        }

        if (path == "back") {
            return with_session(command, config_store_, backend_factory_, [](auto& session, const auto&) {
                auto away = session.set_self_away(false, "");
                if (!away) {
                    return domain::fail<output::CommandOutput>(away.error());
                }
                return domain::ok(output::CommandOutput{
                    .data = output::make_object({
                        {"away", output::make_bool(false)},
                        {"message", output::make_string("")},
                    }),
                    .human = std::string("Cleared your TeamSpeak away status."),
                });
            });
        }

        if (path == "status") {
            return with_session(command, config_store_, backend_factory_, [](auto& session, const auto&) {
                auto state = session.connection_state();
                if (!state) {
                    return domain::fail<output::CommandOutput>(state.error());
                }
                return domain::ok(output::CommandOutput{
                    .data = output::to_value(state.value()),
                    .human = output::connection_status_view(state.value()),
                });
            });
        }

        if (path == "server info") {
            return with_session(command, config_store_, backend_factory_, [](auto& session, const auto&) {
                auto info = session.server_info();
                if (!info) {
                    return domain::fail<output::CommandOutput>(info.error());
                }
                return domain::ok(output::CommandOutput{
                    .data = output::to_value(info.value()),
                    .human = output::server_view(info.value()),
                });
            });
        }

        if (path == "channel list") {
            return with_session(command, config_store_, backend_factory_, [](auto& session, const auto&) {
                auto channels = session.list_channels();
                if (!channels) {
                    return domain::fail<output::CommandOutput>(channels.error());
                }
                return domain::ok(output::CommandOutput{
                    .data = output::to_value(channels.value()),
                    .human = output::channel_table(channels.value()),
                });
            });
        }

        if (path == "channel clients") {
            return with_session(command, config_store_, backend_factory_, [&](auto& session, const auto&) {
                auto clients = session.list_clients();
                if (!clients) {
                    return domain::fail<output::CommandOutput>(clients.error());
                }

                if (!command.positionals.empty()) {
                    auto selector = require_positional(command, 0, "id-or-name");
                    if (!selector) {
                        return domain::fail<output::CommandOutput>(selector.error());
                    }

                    auto channel = session.get_channel(domain::Selector{selector.value()});
                    if (!channel) {
                        return domain::fail<output::CommandOutput>(channel.error());
                    }

                    const std::vector<domain::Channel> channels = {channel.value()};
                    const auto groups = group_clients_by_channel(channels, clients.value());
                    return domain::ok(output::CommandOutput{
                        .data = to_value(groups.front()),
                        .human = channel_clients_table(groups),
                    });
                }

                auto channels = session.list_channels();
                if (!channels) {
                    return domain::fail<output::CommandOutput>(channels.error());
                }

                const auto groups = group_clients_by_channel(channels.value(), clients.value());
                return domain::ok(output::CommandOutput{
                    .data = to_value(groups),
                    .human = channel_clients_table(groups),
                });
            });
        }

        if (path == "channel get") {
            auto selector = require_positional(command, 0, "id-or-name");
            if (!selector) {
                return domain::fail<output::CommandOutput>(selector.error());
            }
            return with_session(command, config_store_, backend_factory_, [&](auto& session, const auto&) {
                auto channel = session.get_channel(domain::Selector{selector.value()});
                if (!channel) {
                    return domain::fail<output::CommandOutput>(channel.error());
                }
                return domain::ok(output::CommandOutput{
                    .data = output::to_value(channel.value()),
                    .human = output::channel_details(channel.value()),
                });
            });
        }

        if (path == "channel join") {
            auto selector = require_positional(command, 0, "id-or-name");
            if (!selector) {
                return domain::fail<output::CommandOutput>(selector.error());
            }
            return with_session(command, config_store_, backend_factory_, [&](auto& session, const auto&) {
                auto joined = session.join_channel(domain::Selector{selector.value()});
                if (!joined) {
                    return domain::fail<output::CommandOutput>(joined.error());
                }
                auto state = session.server_info();
                if (!state) {
                    return domain::fail<output::CommandOutput>(state.error());
                }
                return domain::ok(output::CommandOutput{
                    .data = output::to_value(state.value()),
                    .human = output::server_view(state.value()),
                });
            });
        }

        if (path == "client status") {
            return client_status_process();
        }

        if (path == "client start") {
            return start_client_process(
                command,
                config_store_,
                command.global.format == output::Format::table ? progress : ProgressSink{}
            );
        }

        if (path == "client stop") {
            return stop_client_process(
                command.flags.contains("force"),
                command.global.format == output::Format::table ? progress : ProgressSink{}
            );
        }

        if (path == "client logs") {
            auto count = parse_positive_size(command.options, "count", 80);
            if (!count) {
                return domain::fail<output::CommandOutput>(count.error());
            }
            return client_logs_output(count.value());
        }

        if (path == "client list") {
            return with_session(command, config_store_, backend_factory_, [](auto& session, const auto&) {
                auto clients = session.list_clients();
                if (!clients) {
                    return domain::fail<output::CommandOutput>(clients.error());
                }
                return domain::ok(output::CommandOutput{
                    .data = output::to_value(clients.value()),
                    .human = output::client_table(clients.value()),
                });
            });
        }

        if (path == "client get") {
            auto selector = require_positional(command, 0, "id-or-name");
            if (!selector) {
                return domain::fail<output::CommandOutput>(selector.error());
            }
            return with_session(command, config_store_, backend_factory_, [&](auto& session, const auto&) {
                auto client = session.get_client(domain::Selector{selector.value()});
                if (!client) {
                    return domain::fail<output::CommandOutput>(client.error());
                }
                return domain::ok(output::CommandOutput{
                    .data = output::to_value(client.value()),
                    .human = output::client_details(client.value()),
                });
            });
        }

        if (path == "message send") {
            auto target = require_option(command, "target");
            auto id = require_option(command, "id");
            auto text = require_option(command, "text");
            if (!target) {
                return domain::fail<output::CommandOutput>(target.error());
            }
            if (!id) {
                return domain::fail<output::CommandOutput>(id.error());
            }
            if (!text) {
                return domain::fail<output::CommandOutput>(text.error());
            }

            domain::MessageTargetKind kind{};
            if (target.value() == "client") {
                kind = domain::MessageTargetKind::client;
            } else if (target.value() == "channel") {
                kind = domain::MessageTargetKind::channel;
            } else {
                return domain::fail<output::CommandOutput>(cli_error(
                    "invalid_target", "--target must be client or channel"
                ));
            }

            return with_session(command, config_store_, backend_factory_, [&](auto& session, const auto&) {
                auto sent = session.send_message(domain::MessageRequest{
                    .target_kind = kind,
                    .target = id.value(),
                    .text = text.value(),
                });
                if (!sent) {
                    return domain::fail<output::CommandOutput>(sent.error());
                }
                return domain::ok(output::CommandOutput{
                    .data = output::make_object({
                        {"target_kind", output::make_string(target.value())},
                        {"target", output::make_string(id.value())},
                        {"text", output::make_string(text.value())},
                    }),
                    .human = std::string("message sent"),
                });
            });
        }

        if (path == "playback status") {
            return with_session(command, config_store_, backend_factory_, [](auto& session, const auto&) {
                auto info = session.plugin_info();
                if (!info) {
                    return domain::fail<output::CommandOutput>(info.error());
                }
                return domain::ok(output::CommandOutput{
                    .data = output::to_value(info.value().media_diagnostics),
                    .human = playback_status_view(info.value().media_diagnostics),
                });
            });
        }

        if (path == "playback send") {
            auto file = require_option(command, "file");
            if (!file) {
                return domain::fail<output::CommandOutput>(file.error());
            }
            auto timeout = parse_timeout_ms(command.options, 60000);
            if (!timeout) {
                return domain::fail<output::CommandOutput>(timeout.error());
            }

            return with_session(command, config_store_, backend_factory_, [&](auto& session, const auto& resolved) {
                if (!util::iequals(resolved.profile.backend, "plugin")) {
                    return domain::fail<output::CommandOutput>(app_error(
                        "media",
                        "plugin_backend_required",
                        "playback send requires the plugin backend because media injection is exposed by the TeamSpeak client plugin",
                        domain::ExitCode::unsupported
                    ));
                }

                auto info = session.plugin_info();
                if (!info) {
                    return domain::fail<output::CommandOutput>(info.error());
                }
                if (!info.value().plugin_available) {
                    return domain::fail<output::CommandOutput>(app_error(
                        "media",
                        "plugin_unavailable",
                        "playback send requires a running TeamSpeak client with the ts3cli plugin bridge available",
                        domain::ExitCode::connection
                    ));
                }
                if (info.value().media_socket_path.empty() || info.value().media_transport.empty()) {
                    return domain::fail<output::CommandOutput>(app_error(
                        "media",
                        "media_bridge_unavailable",
                        "plugin info did not report an available media bridge",
                        domain::ExitCode::connection
                    ));
                }
                if (info.value().media_format != bridge::media_format_description()) {
                    return domain::fail<output::CommandOutput>(app_error(
                        "media",
                        "unsupported_media_format",
                        "plugin media bridge reports unsupported playback format: " + info.value().media_format,
                        domain::ExitCode::unsupported
                    ));
                }

                auto sent = bridge::send_playback_file(bridge::PlaybackSendRequest{
                    .socket_path = info.value().media_socket_path,
                    .file_path = file.value(),
                    .timeout = timeout.value(),
                    .clear_first = command.flags.contains("clear"),
                });
                if (!sent) {
                    return domain::fail<output::CommandOutput>(sent.error());
                }
                return domain::ok(output::CommandOutput{
                    .data = playback_send_value(sent.value()),
                    .human = playback_send_view(sent.value()),
                });
            });
        }

        if (path == "events watch") {
            auto count = parse_positive_size(command.options, "count", 5);
            if (!count) {
                return domain::fail<output::CommandOutput>(count.error());
            }
            auto timeout = parse_timeout_ms(command.options, 1000);
            if (!timeout) {
                return domain::fail<output::CommandOutput>(timeout.error());
            }
            return with_session(command, config_store_, backend_factory_, [&](auto& session, const auto&) {
                auto events = session.watch_events(count.value(), timeout.value());
                if (!events) {
                    return domain::fail<output::CommandOutput>(events.error());
                }
                return domain::ok(output::CommandOutput{
                    .data = output::to_value(events.value()),
                    .human = output::event_table(events.value()),
                });
            });
        }

        return domain::fail<output::CommandOutput>(app_error(
            "cli", "unhandled_command", "command not implemented: " + path, domain::ExitCode::internal
        ));
    }();

    if (!result) {
        return domain::fail<output::CommandOutput>(contextualize_error(command, result.error()));
    }
    return result;
}

}  // namespace teamspeak_cli::cli
