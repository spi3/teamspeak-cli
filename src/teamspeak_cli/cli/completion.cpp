#include "teamspeak_cli/cli/completion.hpp"

namespace teamspeak_cli::cli::completion {
namespace {

constexpr const char* kTopLevelCommands =
    "version update plugin sdk daemon config profile connect disconnect mute unmute away back status server channel client message playback events completion";

constexpr const char* kGlobalFlags =
    "--help --output --json --field --no-headers --wide --profile --server --nickname --identity --config --verbose --debug";

constexpr const char* kOutputValues = "table json yaml ndjson";
constexpr const char* kConfigCompletions = "init path view --force --help";
constexpr const char* kProfileCompletions =
    "create list show set unset delete use --copy-from --activate backend host port nickname identity server_password channel_password default_channel control_socket_path --help";
constexpr const char* kCompletionShells = "bash zsh fish powershell";

}  // namespace

auto generate(const std::string& shell) -> domain::Result<std::string> {
    const std::string top_level_words = std::string(kTopLevelCommands) + " " + kGlobalFlags;
    const std::string config_words = kConfigCompletions;
    const std::string profile_words = kProfileCompletions;

    if (shell == "bash") {
        std::string script =
            "# bash completion for ts\n"
            "_ts_complete() {\n"
            "  local cur prev\n"
            "  cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
            "  prev=\"${COMP_WORDS[COMP_CWORD-1]}\"\n"
            "\n"
            "  case \"$prev\" in\n"
            "    --output) COMPREPLY=( $(compgen -W \"";
        script += kOutputValues;
        script +=
            "\" -- \"$cur\") ); return ;;\n"
            "    --config|--file) COMPREPLY=( $(compgen -f -- \"$cur\") ); return ;;\n"
            "  esac\n"
            "\n"
            "  if [[ ${COMP_CWORD} -eq 1 ]]; then\n"
            "    COMPREPLY=( $(compgen -W \"";
        script += top_level_words;
        script +=
            "\" -- \"$cur\") )\n"
            "    return\n"
            "  fi\n"
            "\n"
            "  case \"${COMP_WORDS[1]}\" in\n"
            "    plugin) COMPREPLY=( $(compgen -W \"info --help\" -- \"$cur\") ) ;;\n"
            "    sdk) COMPREPLY=( $(compgen -W \"info --help\" -- \"$cur\") ) ;;\n"
            "    daemon) COMPREPLY=( $(compgen -W \"start stop status --foreground --poll-ms --timeout-ms --help\" -- \"$cur\") ) ;;\n"
            "    config) COMPREPLY=( $(compgen -W \"";
        script += kConfigCompletions;
        script +=
            "\" -- \"$cur\") ) ;;\n"
            "    profile) COMPREPLY=( $(compgen -W \"";
        script += kProfileCompletions;
        script +=
            "\" -- \"$cur\") ) ;;\n"
            "    server) COMPREPLY=( $(compgen -W \"info --help\" -- \"$cur\") ) ;;\n"
            "    channel) COMPREPLY=( $(compgen -W \"list clients get join --help\" -- \"$cur\") ) ;;\n"
            "    client) COMPREPLY=( $(compgen -W \"status start inspect-windows stop logs list get --accept-license --force --count --help\" -- \"$cur\") ) ;;\n"
            "    message) COMPREPLY=( $(compgen -W \"send inbox --target --id --text --count --help\" -- \"$cur\") ) ;;\n"
            "    playback) COMPREPLY=( $(compgen -W \"status send --file --clear --timeout-ms --help\" -- \"$cur\") ) ;;\n"
            "    events) COMPREPLY=( $(compgen -W \"watch hook --count --timeout-ms --output --type --exec --message-kind --help\" -- \"$cur\") ) ;;\n"
            "    completion) COMPREPLY=( $(compgen -W \"";
        script += kCompletionShells;
        script +=
            "\" -- \"$cur\") ) ;;\n"
            "    *) COMPREPLY=( $(compgen -W \"";
        script += kGlobalFlags;
        script +=
            "\" -- \"$cur\") ) ;;\n"
            "  esac\n"
            "}\n"
            "complete -F _ts_complete ts\n";
        return domain::ok(script);
    }

    if (shell == "zsh") {
        std::string script =
            "#compdef ts\n"
            "_arguments '*: :->args'\n"
            "case $words[2] in\n"
            "  config)\n"
            "    _values 'config commands' ";
        script += kConfigCompletions;
        script +=
            "\n"
            "    ;;\n"
            "  profile)\n"
            "    _values 'profile commands' ";
        script += kProfileCompletions;
        script +=
            "\n"
            "    ;;\n"
            "  completion)\n"
            "    _values 'shells' ";
        script += kCompletionShells;
        script +=
            "\n"
            "    ;;\n"
            "  *)\n"
            "    _values 'commands' ";
        script += top_level_words;
        script +=
            "\n"
            "    ;;\n"
            "esac\n";
        return domain::ok(script);
    }

    if (shell == "fish") {
        std::string script =
            "complete -c ts -f\n"
            "for cmd in ";
        script += kTopLevelCommands;
        script +=
            "; complete -c ts -a \"$cmd\"; end\n"
            "complete -c ts -l help\n"
            "complete -c ts -l output -r -a '";
        script += kOutputValues;
        script +=
            "'\n"
            "complete -c ts -l json\n"
            "complete -c ts -l field -r\n"
            "complete -c ts -l no-headers\n"
            "complete -c ts -l wide\n"
            "complete -c ts -l profile -r\n"
            "complete -c ts -l server -r\n"
            "complete -c ts -l nickname -r\n"
            "complete -c ts -l identity -r\n"
            "complete -c ts -l config -r -F\n"
            "complete -c ts -l verbose\n"
            "complete -c ts -l debug\n"
            "complete -c ts -n '__fish_seen_subcommand_from config' -a '";
        script += kConfigCompletions;
        script +=
            "'\n"
            "complete -c ts -n '__fish_seen_subcommand_from profile' -a '";
        script += kProfileCompletions;
        script +=
            "'\n"
            "complete -c ts -n '__fish_seen_subcommand_from completion' -a '";
        script += kCompletionShells;
        script += "'\n";
        return domain::ok(script);
    }

    if (shell == "powershell") {
        std::string script =
            "Register-ArgumentCompleter -CommandName ts -ScriptBlock {\n"
            "  param($commandName, $parameterName, $wordToComplete, $commandAst, $fakeBoundParameters)\n"
            "  $words = @($commandAst.CommandElements | ForEach-Object { $_.Extent.Text })\n"
            "  $items = '";
        script += top_level_words;
        script +=
            "' -split ' '\n"
            "  if ($words.Count -ge 2) {\n"
            "    switch ($words[1]) {\n"
            "      'config' { $items = '";
        script += config_words;
        script +=
            "' -split ' ' }\n"
            "      'profile' { $items = '";
        script += profile_words;
        script +=
            "' -split ' ' }\n"
            "      'completion' { $items = '";
        script += kCompletionShells;
        script +=
            "' -split ' ' }\n"
            "    }\n"
            "  }\n"
            "  $items | Where-Object { $_ -like \"$wordToComplete*\" } | ForEach-Object {\n"
            "    [System.Management.Automation.CompletionResult]::new($_, $_, 'ParameterValue', $_)\n"
            "  }\n"
            "}\n";
        return domain::ok(script);
    }

    return domain::fail<std::string>(domain::make_error(
        "cli",
        "unsupported_shell",
        "unsupported shell for completion: " + shell,
        domain::ExitCode::usage
    ));
}

}  // namespace teamspeak_cli::cli::completion
