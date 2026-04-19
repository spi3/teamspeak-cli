#include "teamspeak_cli/cli/command_router.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>

#include "teamspeak_cli/bridge/socket_paths.hpp"
#include "teamspeak_cli/build/version.hpp"
#include "teamspeak_cli/cli/completion.hpp"
#include "teamspeak_cli/session/session_service.hpp"
#include "teamspeak_cli/util/scope_exit.hpp"
#include "teamspeak_cli/util/strings.hpp"

namespace teamspeak_cli::cli {
namespace {

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
        {{"profile", "list"}, "ts profile list", "List config profiles", {}},
        {{"profile", "use"}, "ts profile use <name>", "Set the active profile", {}},
        {{"connect"}, "ts connect", "Ask the TeamSpeak client plugin to open a server connection", {}},
        {{"disconnect"}, "ts disconnect", "Ask the TeamSpeak client plugin to close the current connection", {}},
        {{"status"}, "ts status", "Show current TeamSpeak client connection status", {}},
        {{"server"}, "ts server <subcommand>", "Inspect the current server session", {}},
        {{"server", "info"}, "ts server info", "Show server details", {}},
        {{"channel"}, "ts channel <subcommand>", "Inspect and join channels", {}},
        {{"channel", "list"}, "ts channel list", "List channels", {}},
        {{"channel", "get"}, "ts channel get <id-or-name>", "Show one channel", {}},
        {{"channel", "join"}, "ts channel join <id-or-name>", "Join a channel if supported", {}},
        {{"client"}, "ts client <subcommand>", "Inspect connected clients and manage the local TeamSpeak client", {}},
        {{"client", "status"}, "ts client status", "Show the tracked local TeamSpeak client process status", {}},
        {{"client", "start"}, "ts client start", "Launch the local TeamSpeak client process", {}},
        {{"client", "stop"}, "ts client stop [--force]", "Stop the tracked local TeamSpeak client process", {"--force"}},
        {{"client", "list"}, "ts client list", "List connected clients", {}},
        {{"client", "get"}, "ts client get <id-or-name>", "Show one client", {}},
        {{"message"}, "ts message <subcommand>", "Send TeamSpeak text messages", {}},
        {{"message", "send"}, "ts message send --target <client|channel> --id <id-or-name> --text <message>", "Send a text message if supported", {"--target", "--id", "--text"}},
        {{"events"}, "ts events <subcommand>", "Watch translated async events", {}},
        {{"events", "watch"}, "ts events watch [--count N] [--timeout-ms N]", "Watch translated async events", {"--count", "--timeout-ms"}},
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
    if (path == "client list") {
        return "list TeamSpeak clients";
    }
    if (path == "client get") {
        return "read TeamSpeak client details";
    }
    if (path == "message send") {
        return "send the TeamSpeak message";
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

    if (error.code == "channel_not_found") {
        add_error_hint(error, "Run `ts channel list` to see available channel IDs and names.");
    }

    if (error.code == "client_not_found") {
        add_error_hint(error, "Run `ts client list` to see available client IDs and nicknames.");
    }

    if (error.code == "profile_not_found") {
        add_error_hint(error, "Run `ts profile list` to see the profiles available in the selected config.");
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

    if (path == "client start" &&
        (error.code == "xvfb_not_found" || error.code == "invalid_xvfb" || error.code == "display_not_found" ||
         error.code == "invalid_display")) {
        add_error_hint(error, "Install Xvfb or set `TS_CLIENT_HEADLESS=0`, then rerun `ts client start`.");
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
};

struct ClientHeadlessLaunch {
    std::filesystem::path xvfb_path;
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

auto read_install_receipt_value(std::string_view key) -> std::optional<std::string> {
    const auto share_dir = installed_share_dir();
    if (!share_dir.has_value()) {
        return std::nullopt;
    }

    std::ifstream input(*share_dir / "install-receipt.env");
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
        return util::trim(line.substr(separator + 1));
    }
    return std::nullopt;
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
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(search_root, ec)) {
            if (ec) {
                break;
            }
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            const auto name = entry.path().filename().string();
            if (name.rfind("libxdo.so", 0) == 0) {
                library_path = entry.path().parent_path().string();
                break;
            }
        }
    }

    return XdotoolPaths{
        .binary_path = std::move(binary_path),
        .library_path = std::move(library_path),
    };
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

    const std::array<std::string_view, 4> escape_titles = {
        "Introducing the next generation of TeamSpeak",
        "myTeamSpeak Account",
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

    auto state_dir = resolve_client_state_dir();
    if (!state_dir) {
        return domain::fail<ClientProcessPaths>(state_dir.error());
    }

    return domain::ok(ClientProcessPaths{
        .launcher_path = std::move(launcher_path),
        .state_dir = state_dir.value(),
        .pid_file = state_dir.value() / "client.pid",
        .log_file = state_dir.value() / "client.log",
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

    std::filesystem::path xvfb_path;
    if (const char* explicit_xvfb = std::getenv("TS_CLIENT_XVFB"); explicit_xvfb != nullptr &&
                                                            *explicit_xvfb != '\0') {
        xvfb_path = explicit_xvfb;
        if (!is_executable_file(xvfb_path)) {
            return domain::fail<std::optional<ClientHeadlessLaunch>>(client_error(
                "invalid_xvfb",
                "TS_CLIENT_XVFB is not executable: " + xvfb_path.string(),
                domain::ExitCode::not_found
            ));
        }
    } else if (const auto found = find_executable_on_path("Xvfb"); found.has_value()) {
        xvfb_path = *found;
    } else {
        return domain::fail<std::optional<ClientHeadlessLaunch>>(client_error(
            "xvfb_not_found",
            "Xvfb is required to launch the TeamSpeak client headlessly; install Xvfb or set TS_CLIENT_HEADLESS=0",
            domain::ExitCode::not_found
        ));
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

    return domain::ok(std::optional<ClientHeadlessLaunch>{ClientHeadlessLaunch{
        .xvfb_path = std::move(xvfb_path),
        .display = std::move(display),
    }});
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
        .human = output::Details{
            .fields = {
                {"Action", std::move(action)},
                {"Status", std::move(status)},
                {"PID", std::to_string(pid)},
                {"Launcher", paths.launcher_path.string()},
                {"PIDFile", paths.pid_file.string()},
                {"LogFile", paths.log_file.string()},
                {"Note", std::move(note)},
            },
        },
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
        .human = output::Details{
            .fields = {
                {"Action", std::move(action)},
                {"Status", "not-running"},
                {"PID", "0"},
                {"Launcher", paths.launcher_path.string()},
                {"PIDFile", paths.pid_file.string()},
                {"LogFile", paths.log_file.string()},
                {"Note", std::move(note)},
            },
        },
    };
}

auto client_status_process() -> domain::Result<output::CommandOutput> {
    auto paths = resolve_client_process_paths();
    if (!paths) {
        return domain::fail<output::CommandOutput>(paths.error());
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

auto start_client_process(const ParsedCommand& command, const config::ConfigStore& store)
    -> domain::Result<output::CommandOutput> {
    auto paths = resolve_client_process_paths();
    if (!paths) {
        return domain::fail<output::CommandOutput>(paths.error());
    }

    auto launch_socket_path = resolve_client_launch_socket_path(command, store);
    if (!launch_socket_path) {
        return domain::fail<output::CommandOutput>(launch_socket_path.error());
    }

    auto headless_launch = resolve_client_headless_launch();
    if (!headless_launch) {
        return domain::fail<output::CommandOutput>(headless_launch.error());
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

    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) {
        return domain::fail<output::CommandOutput>(client_error(
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
        return domain::fail<output::CommandOutput>(client_error(
            "pipe_cloexec_failed",
            "failed to mark launcher status pipe close-on-exec: " + std::string(std::strerror(errno)),
            domain::ExitCode::internal
        ));
    }

    const pid_t child_pid = ::fork();
    if (child_pid < 0) {
        return domain::fail<output::CommandOutput>(client_error(
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
        const int log_fd = ::open(
            paths.value().log_file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644
        );
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

        if (::setenv("TS_CONTROL_SOCKET_PATH", launch_socket_path.value().c_str(), 1) != 0) {
            const int child_errno = errno;
            (void)::write(pipe_fds[1], &child_errno, sizeof(child_errno));
            _exit(127);
        }

        if (headless_launch.value().has_value()) {
            const auto& headless = *headless_launch.value();
            const pid_t xvfb_pid = ::fork();
            if (xvfb_pid < 0) {
                const int child_errno = errno;
                (void)::write(pipe_fds[1], &child_errno, sizeof(child_errno));
                _exit(127);
            }

            if (xvfb_pid == 0) {
                char* const xvfb_argv[] = {
                    const_cast<char*>(headless.xvfb_path.c_str()),
                    const_cast<char*>(headless.display.c_str()),
                    const_cast<char*>("-screen"),
                    const_cast<char*>("0"),
                    const_cast<char*>("1280x1024x24"),
                    const_cast<char*>("-ac"),
                    nullptr,
                };
                ::execv(headless.xvfb_path.c_str(), xvfb_argv);

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

        char* const argv[] = {const_cast<char*>(paths.value().launcher_path.c_str()), nullptr};
        ::execv(paths.value().launcher_path.c_str(), argv);

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
        return domain::fail<output::CommandOutput>(client_error(
            "launch_failed",
            "failed to launch TeamSpeak client: " + std::string(std::strerror(child_errno)),
            domain::ExitCode::internal
        ));
    }

    auto wrote_pid = write_pid_file(paths.value().pid_file, child_pid);
    if (!wrote_pid) {
        (void)::kill(child_pid, SIGTERM);
        return domain::fail<output::CommandOutput>(wrote_pid.error());
    }

    if (headless_launch.value().has_value()) {
        dismiss_headless_client_dialogs(*headless_launch.value(), child_pid);
    }

    return domain::ok(client_process_output(
        "start",
        "started",
        child_pid,
        paths.value(),
        headless_launch.value().has_value()
            ? "client process launched headlessly on DISPLAY " + headless_launch.value()->display +
                  " using socket " + launch_socket_path.value()
            : "client process launched using socket " + launch_socket_path.value()
    ));
}

auto stop_client_process(bool force) -> domain::Result<output::CommandOutput> {
    auto paths = resolve_client_process_paths();
    if (!paths) {
        return domain::fail<output::CommandOutput>(paths.error());
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
        } else {
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
        } else {
            return domain::ok(client_process_missing_output(
                "stop",
                paths.value(),
                "tracked client pid file was stale"
            ));
        }
    }

    if (!signal_client_target(*pid, SIGTERM)) {
        if (errno == ESRCH) {
            remove_pid_file(paths.value().pid_file);
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
    return domain::ok(client_process_output(
        "stop",
        "stopped",
        *pid,
        paths.value(),
        note
    ));
}

auto parse_server_override(const std::string& raw, domain::Profile& profile) -> domain::Result<void> {
    const auto parts = util::split(raw, ':');
    if (parts.empty()) {
        return domain::fail(cli_error("invalid_server", "invalid --server value"));
    }
    profile.host = parts[0];
    if (parts.size() > 1 && !parts[1].empty()) {
        const auto port = util::parse_u16(parts[1]);
        if (!port.has_value()) {
            return domain::fail(cli_error("invalid_server_port", "invalid port in --server"));
        }
        profile.port = *port;
    }
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

auto build_init_options(const ParsedCommand& command, const domain::Profile& profile) -> sdk::InitOptions {
    return sdk::InitOptions{
        .verbose = command.global.verbose,
        .debug = command.global.debug,
        .socket_path = profile.control_socket_path,
    };
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
    out << "  --output <table|json|yaml>\n";
    out << "  --json\n";
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
                option == "target" || option == "id" || option == "text" ||
                option == "count" || option == "timeout-ms";

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

    out << "\nGlobal options: --output --json --profile --server --nickname --identity --config --verbose --debug --help\n";
    return out.str();
}

auto CommandRouter::dispatch(const ParsedCommand& command) -> domain::Result<output::CommandOutput> {
    const auto path = util::join(command.path, " ");

    const auto result = [&]() -> domain::Result<output::CommandOutput> {
        if (path == "version") {
            const std::vector<std::string> backends = {"fake", "plugin"};
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
            loaded.value().active_profile = name.value();
            const auto saved = config_store_.save(config_path, loaded.value());
            if (!saved) {
                return domain::fail<output::CommandOutput>(saved.error());
            }
            return domain::ok(output::CommandOutput{
                .data = output::make_object({
                    {"active_profile", output::make_string(name.value())},
                    {"path", output::make_string(config_path.string())},
                }),
                .human = std::string("active profile set to " + name.value()),
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
                auto connected = session.connect(build_connect_request(resolved.profile));
                if (!connected) {
                    return domain::fail<output::CommandOutput>(connected.error());
                }
                auto state = session.connection_state();
                if (!state) {
                    return domain::fail<output::CommandOutput>(state.error());
                }
                return domain::ok(output::CommandOutput{
                    .data = output::to_value(state.value()),
                    .human = output::status_view(state.value()),
                });
            });
        }

        if (path == "disconnect") {
            return with_session(command, config_store_, backend_factory_, [](auto& session, const auto&) {
                auto disconnected = session.disconnect("ts disconnect");
                if (!disconnected) {
                    return domain::fail<output::CommandOutput>(disconnected.error());
                }
                auto state = session.connection_state();
                if (!state) {
                    return domain::fail<output::CommandOutput>(state.error());
                }
                return domain::ok(output::CommandOutput{
                    .data = output::to_value(state.value()),
                    .human = output::status_view(state.value()),
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
                    .human = output::status_view(state.value()),
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
            return start_client_process(command, config_store_);
        }

        if (path == "client stop") {
            return stop_client_process(command.flags.contains("force"));
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
