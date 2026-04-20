#include "teamspeak_cli/daemon/runtime.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "teamspeak_cli/bridge/protocol.hpp"
#include "teamspeak_cli/output/render.hpp"
#include "teamspeak_cli/util/scope_exit.hpp"
#include "teamspeak_cli/util/strings.hpp"

namespace teamspeak_cli::daemon {
namespace {

auto daemon_error(
    std::string code,
    std::string message,
    domain::ExitCode exit_code = domain::ExitCode::internal
) -> domain::Error {
    return domain::make_error("daemon", std::move(code), std::move(message), exit_code);
}

auto format_timestamp(std::chrono::system_clock::time_point at) -> std::string {
    const auto time = std::chrono::system_clock::to_time_t(at);
    std::tm local_tm{};
#if defined(_POSIX_VERSION)
    localtime_r(&time, &local_tm);
#else
    local_tm = *std::localtime(&time);
#endif
    std::ostringstream out;
    out << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

auto now_string() -> std::string {
    return format_timestamp(std::chrono::system_clock::now());
}

auto ensure_state_dir(const StatePaths& paths) -> domain::Result<void> {
    std::error_code ec;
    std::filesystem::create_directories(paths.state_dir, ec);
    if (ec) {
        return domain::fail(daemon_error(
            "state_dir_create_failed",
            "failed to create daemon state directory: " + ec.message()
        ));
    }
    return domain::ok();
}

auto write_key_values(
    const std::filesystem::path& path,
    const std::map<std::string, std::string>& fields
) -> domain::Result<void> {
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        return domain::fail(daemon_error(
            "write_failed", "failed to write daemon state file: " + path.string()
        ));
    }

    for (const auto& [key, value] : fields) {
        output << bridge::protocol::join_fields({key, bridge::protocol::hex_encode(value)});
    }
    return domain::ok();
}

auto read_key_values(const std::filesystem::path& path) -> domain::Result<std::map<std::string, std::string>> {
    std::ifstream input(path);
    if (!input) {
        return domain::ok(std::map<std::string, std::string>{});
    }

    std::map<std::string, std::string> fields;
    std::string line;
    while (std::getline(input, line)) {
        if (util::trim(line).empty()) {
            continue;
        }
        const auto parsed = bridge::protocol::split_fields(line);
        if (parsed.size() != 2) {
            return domain::fail<std::map<std::string, std::string>>(daemon_error(
                "invalid_state_file", "invalid daemon state record: " + path.string()
            ));
        }
        auto decoded = bridge::protocol::hex_decode(parsed[1]);
        if (!decoded) {
            return domain::fail<std::map<std::string, std::string>>(daemon_error(
                "invalid_state_file", "invalid daemon state encoding: " + path.string()
            ));
        }
        fields.emplace(parsed[0], decoded.value());
    }
    return domain::ok(std::move(fields));
}

auto count_inbox_messages(const StatePaths& paths) -> domain::Result<std::size_t> {
    std::ifstream input(paths.inbox_file);
    if (!input) {
        return domain::ok(std::size_t{0});
    }

    std::size_t count = 0;
    std::string line;
    while (std::getline(input, line)) {
        if (!util::trim(line).empty()) {
            ++count;
        }
    }
    return domain::ok(count);
}

auto normalize_message_event(domain::Event event) -> domain::Event {
    if (event.type != "message.received") {
        return event;
    }

    if (!event.fields.contains("from_name")) {
        if (const auto from = event.fields.find("from"); from != event.fields.end()) {
            event.fields.emplace("from_name", from->second);
        }
    }

    if (!event.fields.contains("message_kind")) {
        if (const auto kind = event.fields.find("target_kind"); kind != event.fields.end()) {
            event.fields.emplace("message_kind", kind->second);
        } else if (event.fields.contains("channel_id")) {
            event.fields.emplace("message_kind", "channel");
        } else if (const auto target_mode = event.fields.find("target_mode"); target_mode != event.fields.end()) {
            if (target_mode->second == "1") {
                event.fields.emplace("message_kind", "client");
            } else if (target_mode->second == "2") {
                event.fields.emplace("message_kind", "channel");
            } else if (target_mode->second == "3") {
                event.fields.emplace("message_kind", "server");
            }
        }
    }

    return event;
}

auto matches_hook(const Hook& hook, const domain::Event& event) -> bool {
    if (hook.event_type != "*" && hook.event_type != event.type) {
        return false;
    }

    if (!hook.message_kind.empty()) {
        const auto kind = event.fields.find("message_kind");
        if (kind == event.fields.end() || kind->second != hook.message_kind) {
            return false;
        }
    }

    return true;
}

auto event_json(const domain::Event& event) -> std::string {
    return output::render(output::CommandOutput{
                              .data = output::to_value(event),
                              .human = output::HumanView{},
                          },
                          output::Format::json);
}

auto set_child_env(std::string_view key, const std::string& value) -> bool {
    return ::setenv(std::string(key).c_str(), value.c_str(), 1) == 0;
}

void spawn_hook_process(const Hook& hook, const domain::Event& event) {
    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) {
        return;
    }
    auto close_pipe = util::make_scope_exit([&] {
        if (pipe_fds[0] >= 0) {
            ::close(pipe_fds[0]);
        }
        if (pipe_fds[1] >= 0) {
            ::close(pipe_fds[1]);
        }
    });

    const pid_t child = ::fork();
    if (child < 0) {
        return;
    }

    if (child == 0) {
        if (::dup2(pipe_fds[0], STDIN_FILENO) == -1) {
            _exit(127);
        }
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);

        if (!set_child_env("TS_HOOK_ID", hook.id) || !set_child_env("TS_EVENT_TYPE", event.type) ||
            !set_child_env("TS_EVENT_SUMMARY", event.summary) ||
            !set_child_env("TS_EVENT_TIMESTAMP", format_timestamp(event.at))) {
            _exit(127);
        }

        if (const auto kind = event.fields.find("message_kind"); kind != event.fields.end()) {
            if (!set_child_env("TS_MESSAGE_KIND", kind->second)) {
                _exit(127);
            }
        }
        if (const auto from = event.fields.find("from_name"); from != event.fields.end()) {
            if (!set_child_env("TS_MESSAGE_FROM", from->second)) {
                _exit(127);
            }
        }
        if (const auto text = event.fields.find("text"); text != event.fields.end()) {
            if (!set_child_env("TS_MESSAGE_TEXT", text->second)) {
                _exit(127);
            }
        }

        execl("/bin/sh", "sh", "-c", hook.command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    ::close(pipe_fds[0]);
    pipe_fds[0] = -1;

    const std::string payload = event_json(event);
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const ssize_t written = ::write(
            pipe_fds[1], payload.data() + static_cast<std::ptrdiff_t>(offset), payload.size() - offset
        );
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    ::close(pipe_fds[1]);
    pipe_fds[1] = -1;
}

void reap_hook_children() {
    while (true) {
        int status = 0;
        const pid_t child = ::waitpid(-1, &status, WNOHANG);
        if (child <= 0) {
            return;
        }
    }
}

void sleep_with_stop(std::chrono::milliseconds duration, const std::function<bool()>& stop_requested) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (!stop_requested() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

auto parse_optional_u64(
    const std::map<std::string, std::string>& fields,
    const std::string& key
) -> std::optional<std::uint64_t> {
    const auto it = fields.find(key);
    if (it == fields.end()) {
        return std::nullopt;
    }
    return util::parse_u64(it->second);
}

auto count_hooks(const StatePaths& paths) -> domain::Result<std::size_t> {
    auto hooks = load_hooks(paths);
    if (!hooks) {
        return domain::fail<std::size_t>(hooks.error());
    }
    return domain::ok(hooks.value().size());
}

}  // namespace

auto resolve_state_dir() -> domain::Result<std::filesystem::path> {
    if (const char* explicit_path = std::getenv("TS_DAEMON_STATE_DIR");
        explicit_path != nullptr && *explicit_path != '\0') {
        return domain::ok(std::filesystem::path(explicit_path));
    }
    if (const char* xdg_state = std::getenv("XDG_STATE_HOME"); xdg_state != nullptr && *xdg_state != '\0') {
        return domain::ok(std::filesystem::path(xdg_state) / "teamspeak-cli" / "daemon");
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return domain::ok(std::filesystem::path(home) / ".local" / "state" / "teamspeak-cli" / "daemon");
    }
    return domain::fail<std::filesystem::path>(daemon_error(
        "state_dir_not_found",
        "HOME or XDG_STATE_HOME must be set to manage the TeamSpeak event daemon",
        domain::ExitCode::config
    ));
}

auto state_paths_for(const std::filesystem::path& state_dir) -> StatePaths {
    return StatePaths{
        .state_dir = state_dir,
        .pid_file = state_dir / "daemon.pid",
        .log_file = state_dir / "daemon.log",
        .status_file = state_dir / "daemon.status",
        .inbox_file = state_dir / "message-inbox.log",
        .hooks_file = state_dir / "hooks.db",
    };
}

auto load_hooks(const StatePaths& paths) -> domain::Result<std::vector<Hook>> {
    std::ifstream input(paths.hooks_file);
    if (!input) {
        return domain::ok(std::vector<Hook>{});
    }

    std::vector<Hook> hooks;
    std::string line;
    while (std::getline(input, line)) {
        if (util::trim(line).empty()) {
            continue;
        }
        const auto fields = bridge::protocol::split_fields(line);
        if (fields.size() != 5 || fields[0] != "hook") {
            return domain::fail<std::vector<Hook>>(daemon_error(
                "invalid_hook_file", "invalid hook record in " + paths.hooks_file.string()
            ));
        }

        auto event_type = bridge::protocol::hex_decode(fields[2]);
        auto message_kind = bridge::protocol::hex_decode(fields[3]);
        auto command = bridge::protocol::hex_decode(fields[4]);
        if (!event_type || !message_kind || !command) {
            return domain::fail<std::vector<Hook>>(daemon_error(
                "invalid_hook_file", "invalid hook encoding in " + paths.hooks_file.string()
            ));
        }

        hooks.push_back(Hook{
            .id = fields[1],
            .event_type = event_type.value(),
            .message_kind = message_kind.value(),
            .command = command.value(),
        });
    }
    return domain::ok(std::move(hooks));
}

auto save_hooks(const StatePaths& paths, const std::vector<Hook>& hooks) -> domain::Result<void> {
    auto ensured = ensure_state_dir(paths);
    if (!ensured) {
        return ensured;
    }

    std::ofstream output(paths.hooks_file, std::ios::trunc);
    if (!output) {
        return domain::fail(daemon_error(
            "write_failed", "failed to write hook file: " + paths.hooks_file.string()
        ));
    }

    for (const auto& hook : hooks) {
        output << bridge::protocol::join_fields(
            {"hook",
             hook.id,
             bridge::protocol::hex_encode(hook.event_type),
             bridge::protocol::hex_encode(hook.message_kind),
             bridge::protocol::hex_encode(hook.command)}
        );
    }
    return domain::ok();
}

auto read_inbox(const StatePaths& paths, std::size_t limit) -> domain::Result<std::vector<domain::Event>> {
    std::ifstream input(paths.inbox_file);
    if (!input) {
        return domain::ok(std::vector<domain::Event>{});
    }

    std::vector<domain::Event> messages;
    std::string line;
    while (std::getline(input, line)) {
        if (util::trim(line).empty()) {
            continue;
        }
        auto decoded = bridge::protocol::decode_event({bridge::protocol::split_fields(line)});
        if (!decoded || !decoded.value().has_value()) {
            return domain::fail<std::vector<domain::Event>>(daemon_error(
                "invalid_inbox_file", "invalid message inbox record in " + paths.inbox_file.string()
            ));
        }
        messages.push_back(normalize_message_event(std::move(*decoded.value())));
    }

    if (limit == 0 || messages.size() <= limit) {
        return domain::ok(std::move(messages));
    }

    return domain::ok(std::vector<domain::Event>(
        messages.end() - static_cast<std::ptrdiff_t>(limit), messages.end()
    ));
}

auto append_inbox_event(const StatePaths& paths, const domain::Event& event) -> domain::Result<void> {
    auto ensured = ensure_state_dir(paths);
    if (!ensured) {
        return ensured;
    }

    std::ofstream output(paths.inbox_file, std::ios::app);
    if (!output) {
        return domain::fail(daemon_error(
            "write_failed", "failed to append message inbox file: " + paths.inbox_file.string()
        ));
    }

    output << bridge::protocol::join_fields(bridge::protocol::encode_event(event).front());
    return domain::ok();
}

auto read_status(const StatePaths& paths) -> domain::Result<Status> {
    auto fields = read_key_values(paths.status_file);
    if (!fields) {
        return domain::fail<Status>(fields.error());
    }

    Status status;
    if (const auto running = fields.value().find("running"); running != fields.value().end()) {
        status.running = util::iequals(running->second, "1") || util::iequals(running->second, "true");
    }
    if (const auto pid = parse_optional_u64(fields.value(), "pid"); pid.has_value()) {
        status.pid = static_cast<pid_t>(*pid);
    }
    if (const auto profile = fields.value().find("profile"); profile != fields.value().end()) {
        status.profile = profile->second;
    }
    if (const auto backend = fields.value().find("backend"); backend != fields.value().end()) {
        status.backend = backend->second;
    }
    if (const auto started_at = fields.value().find("started_at"); started_at != fields.value().end()) {
        status.started_at = started_at->second;
    }
    if (const auto last_event_at = fields.value().find("last_event_at"); last_event_at != fields.value().end()) {
        status.last_event_at = last_event_at->second;
    }
    if (const auto last_event_type = fields.value().find("last_event_type"); last_event_type != fields.value().end()) {
        status.last_event_type = last_event_type->second;
    }
    if (const auto last_error = fields.value().find("last_error"); last_error != fields.value().end()) {
        status.last_error = last_error->second;
    }
    if (const auto poll_ms = parse_optional_u64(fields.value(), "poll_ms"); poll_ms.has_value()) {
        status.poll_interval = std::chrono::milliseconds(*poll_ms);
    }

    auto inbox_count = count_inbox_messages(paths);
    if (!inbox_count) {
        return domain::fail<Status>(inbox_count.error());
    }
    status.inbox_count = inbox_count.value();

    auto hook_count = count_hooks(paths);
    if (!hook_count) {
        return domain::fail<Status>(hook_count.error());
    }
    status.hook_count = hook_count.value();

    return domain::ok(std::move(status));
}

auto write_status(const StatePaths& paths, const Status& status) -> domain::Result<void> {
    auto ensured = ensure_state_dir(paths);
    if (!ensured) {
        return ensured;
    }

    return write_key_values(paths.status_file, {
                                               {"running", status.running ? "1" : "0"},
                                               {"pid", std::to_string(status.pid)},
                                               {"profile", status.profile},
                                               {"backend", status.backend},
                                               {"started_at", status.started_at},
                                               {"last_event_at", status.last_event_at},
                                               {"last_event_type", status.last_event_type},
                                               {"last_error", status.last_error},
                                               {"poll_ms", std::to_string(status.poll_interval.count())},
                                           });
}

auto run_event_daemon(
    const domain::Profile& profile,
    const sdk::InitOptions& init_options,
    const StatePaths& paths,
    std::chrono::milliseconds poll_interval,
    const std::function<bool()>& stop_requested,
    const sdk::BackendFactory& factory
) -> domain::Result<void> {
    auto ensured = ensure_state_dir(paths);
    if (!ensured) {
        return ensured;
    }

    Status status{
        .running = true,
        .pid = ::getpid(),
        .profile = profile.name,
        .backend = profile.backend,
        .started_at = now_string(),
        .last_event_at = "",
        .last_event_type = "",
        .last_error = "",
        .poll_interval = poll_interval,
        .inbox_count = 0,
        .hook_count = 0,
    };

    if (const auto current_status = read_status(paths); current_status) {
        if (!current_status.value().started_at.empty()) {
            status.started_at = current_status.value().started_at;
        }
        status.inbox_count = current_status.value().inbox_count;
        status.hook_count = current_status.value().hook_count;
    }

    auto write_current_status = [&]() -> domain::Result<void> {
        auto inbox_count = count_inbox_messages(paths);
        if (!inbox_count) {
            return domain::fail(inbox_count.error());
        }
        status.inbox_count = inbox_count.value();

        auto hook_count = count_hooks(paths);
        if (!hook_count) {
            return domain::fail(hook_count.error());
        }
        status.hook_count = hook_count.value();
        return write_status(paths, status);
    };

    auto initial_write = write_current_status();
    if (!initial_write) {
        return initial_write;
    }

    std::unique_ptr<sdk::Backend> backend;
    auto shutdown_backend = util::make_scope_exit([&] {
        if (backend) {
            const auto ignored = backend->shutdown();
            (void)ignored;
        }
        status.running = false;
        status.pid = 0;
        status.last_error = stop_requested() ? "" : status.last_error;
        const auto ignored = write_current_status();
        (void)ignored;
    });

    while (!stop_requested()) {
        reap_hook_children();

        if (!backend) {
            auto created = factory.create(profile.backend);
            if (!created) {
                return domain::fail(created.error());
            }
            backend = std::move(created.value());
            auto initialized = backend->initialize(init_options);
            if (!initialized) {
                status.last_error = initialized.error().message;
                const auto wrote = write_current_status();
                if (!wrote) {
                    return wrote;
                }
                backend.reset();
                sleep_with_stop(std::max(poll_interval, std::chrono::milliseconds(500)), stop_requested);
                continue;
            }
        }

        auto next = backend->next_event(poll_interval);
        if (!next) {
            status.last_error = next.error().message;
            const auto wrote = write_current_status();
            if (!wrote) {
                return wrote;
            }
            const auto ignored = backend->shutdown();
            (void)ignored;
            backend.reset();
            sleep_with_stop(std::max(poll_interval, std::chrono::milliseconds(500)), stop_requested);
            continue;
        }

        if (!next.value().has_value()) {
            continue;
        }

        auto event = normalize_message_event(std::move(*next.value()));
        status.last_event_at = format_timestamp(event.at);
        status.last_event_type = event.type;
        status.last_error.clear();

        if (event.type == "message.received") {
            auto appended = append_inbox_event(paths, event);
            if (!appended) {
                return appended;
            }
        }

        auto hooks = load_hooks(paths);
        if (!hooks) {
            status.last_error = hooks.error().message;
        } else {
            for (const auto& hook : hooks.value()) {
                if (matches_hook(hook, event)) {
                    spawn_hook_process(hook, event);
                }
            }
        }

        auto wrote = write_current_status();
        if (!wrote) {
            return wrote;
        }
    }

    return domain::ok();
}

}  // namespace teamspeak_cli::daemon
