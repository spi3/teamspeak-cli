#include "teamspeak_cli/cli/command_router.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>

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
        {{"client"}, "ts client <subcommand>", "Inspect connected clients", {}},
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

struct ResolvedProfile {
    std::filesystem::path config_path;
    domain::AppConfig config;
    domain::Profile profile;
};

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
}

}  // namespace teamspeak_cli::cli
