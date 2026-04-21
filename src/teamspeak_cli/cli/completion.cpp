#include "teamspeak_cli/cli/completion.hpp"

namespace teamspeak_cli::cli::completion {
namespace {

constexpr const char* kTopLevelCommands =
    "version sdk config profile connect disconnect status server channel client message events completion";

constexpr const char* kGlobalFlags =
    "--help --output --json --profile --server --nickname --identity --config --verbose --debug";

}  // namespace

auto generate(const std::string& shell) -> domain::Result<std::string> {
    if (shell == "bash") {
        return domain::ok(
            std::string("# bash completion for ts\n"
                        "_ts_complete() {\n"
                        "  local cur prev\n"
                        "  cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
                        "  prev=\"${COMP_WORDS[COMP_CWORD-1]}\"\n"
                        "\n"
                        "  if [[ ${COMP_CWORD} -eq 1 ]]; then\n"
                        "    COMPREPLY=( $(compgen -W \"") +
            kTopLevelCommands + " " + kGlobalFlags +
            "\" -- \"$cur\") )\n"
            "    return\n"
            "  fi\n"
            "\n"
            "  case \"${COMP_WORDS[1]}\" in\n"
            "    sdk) COMPREPLY=( $(compgen -W \"info --help\" -- \"$cur\") ) ;;\n"
            "    config) COMPREPLY=( $(compgen -W \"init view --help\" -- \"$cur\") ) ;;\n"
            "    profile) COMPREPLY=( $(compgen -W \"create list use --help\" -- \"$cur\") ) ;;\n"
            "    server) COMPREPLY=( $(compgen -W \"info --help\" -- \"$cur\") ) ;;\n"
            "    channel) COMPREPLY=( $(compgen -W \"list clients get join --help\" -- \"$cur\") ) ;;\n"
            "    client) COMPREPLY=( $(compgen -W \"status start stop logs list get --help\" -- \"$cur\") ) ;;\n"
            "    message) COMPREPLY=( $(compgen -W \"send --help\" -- \"$cur\") ) ;;\n"
            "    events) COMPREPLY=( $(compgen -W \"watch --help\" -- \"$cur\") ) ;;\n"
            "    completion) COMPREPLY=( $(compgen -W \"bash zsh fish powershell\" -- \"$cur\") ) ;;\n"
            "    *) COMPREPLY=( $(compgen -W \"" +
            std::string(kGlobalFlags) + "\" -- \"$cur\") ) ;;\n"
                                        "  esac\n"
                                        "}\n"
                                        "complete -F _ts_complete ts\n"
        );
    }

    if (shell == "zsh") {
        return domain::ok(
            std::string("#compdef ts\n"
                        "_arguments '*: :->cmds'\n"
                        "case $state in\n"
                        "  cmds)\n"
                        "    _values 'commands' ") +
            kTopLevelCommands + " bash zsh fish powershell\n    ;;\nesac\n"
        );
    }

    if (shell == "fish") {
        return domain::ok(
            std::string("complete -c ts -f\n"
                        "for cmd in ") +
            kTopLevelCommands +
            "; complete -c ts -a \"$cmd\"; end\n"
            "complete -c ts -n '__fish_seen_subcommand_from completion' -a 'bash zsh fish powershell'\n"
        );
    }

    if (shell == "powershell") {
        return domain::ok(
            std::string("Register-ArgumentCompleter -CommandName ts -ScriptBlock {\n"
                        "  param($commandName, $wordToComplete, $cursorPosition)\n"
                        "  '") +
            kTopLevelCommands +
            "' -split ' ' | Where-Object { $_ -like \"$wordToComplete*\" } | ForEach-Object {\n"
            "    [System.Management.Automation.CompletionResult]::new($_, $_, 'ParameterValue', $_)\n"
            "  }\n"
            "}\n"
        );
    }

    return domain::fail<std::string>(domain::make_error(
        "cli",
        "unsupported_shell",
        "unsupported shell for completion: " + shell,
        domain::ExitCode::usage
    ));
}

}  // namespace teamspeak_cli::cli::completion
