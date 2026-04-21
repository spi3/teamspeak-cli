# Command Reference

## Global Options

Every command accepts these global flags:

- `--output table|json|yaml`
- `--json`
- `--profile <name>`
- `--server <host[:port]>`
- `--nickname <name>`
- `--identity <value>`
- `--config <path>`
- `--verbose`
- `--debug`
- `--help`

`--json` is shorthand for `--output json`.

## Command Groups

### Core

- `ts version`
- `ts plugin info`
- `ts sdk info`
- `ts daemon start [--foreground] [--poll-ms N]`
- `ts daemon stop [--timeout-ms N]`
- `ts daemon status`
- `ts connect`
- `ts disconnect`
- `ts status`
- `ts server info`

`ts sdk info` is a compatibility alias for `ts plugin info`.

### Config And Profiles

- `ts config init [--force]`
- `ts config view`
- `ts profile create <name> [--copy-from <name>] [--activate]`
- `ts profile list`
- `ts profile use <name>`

### Channels

- `ts channel list`
- `ts channel clients [id-or-name]`
- `ts channel get <id-or-name>`
- `ts channel join <id-or-name>`

### Clients

- `ts client status`
- `ts client start`
- `ts client stop [--force]`
- `ts client list`
- `ts client get <id-or-name>`

### Messaging And Events

- `ts message send --target <client|channel> --id <id-or-name> --text "<message>"`
- `ts message inbox [--count N]`
- `ts events watch [--count N] [--timeout-ms N]`
- `ts events hook add --type <event-type> --exec <command> [--message-kind <client|channel|server>]`
- `ts events hook list`
- `ts events hook remove <id>`

### Shell Completion

- `ts completion bash`
- `ts completion zsh`
- `ts completion fish`
- `ts completion powershell`

## Behavior That Matters

- Running a group command such as `ts channel`, `ts config`, or `ts client` with no subcommand prints contextual help for that group.
- `ts <command> --help` prints command-specific usage and examples generated from the command router.
- `status`, `server info`, `channel list`, `channel clients`, and `client list` inspect the current backend state. They do not auto-connect for you.
- `connect` waits for completion for up to 15 seconds.
- `disconnect` waits for completion for up to 10 seconds.
- `client start` and `client stop` inspect, launch, and stop the local TeamSpeak client process tracked by `ts`.
- `daemon start` launches a local background watcher that polls translated TeamSpeak events, journals incoming messages, and executes matching hook commands.
- `daemon stop` only stops the local watcher process. It does not disconnect the TeamSpeak client from the current server.
- `message inbox` reads the daemon-managed message journal, so it can show captured messages even when no `ts` command is actively watching events.

## Progress Streaming

These commands stream human-readable progress when output is `table`:

- `connect`
- `disconnect`
- `client start`
- `client stop`

The same commands return one structured result at the end when output is `json` or `yaml`.

## Profile And Socket Notes

- the default profile is `plugin-local`
- the offline mock profile is `mock-local`
- `ts profile create <name>` clones the active profile by default
- `ts profile create <name> --copy-from <source>` clones a specific existing profile instead
- `ts profile create <name> --activate` also sets the new profile as the default
- `ts profile use <name>` sets the default profile without creating a new one
- the `plugin` backend expects the TeamSpeak client plugin bridge to already be running
- the selected profile can be overridden with `--profile`
- `--server`, `--nickname`, and `--identity` override the active profile for one command

Socket path resolution for the plugin backend is:

- `control_socket_path=` in the selected profile, if set
- `TS_CONTROL_SOCKET_PATH`, if set
- otherwise the runtime default path

## Client Process Management

`ts client start` looks for a launcher in this order:

- `TS_CLIENT_LAUNCHER`
- an installed sibling `ts3client` next to the running `ts` binary
- `ts3client` or `teamspeak3-client` on `PATH`
- `TS3_CLIENT_DIR/ts3client_runscript.sh`

`ts client start` uses headless launch automatically when no display is available. Useful overrides:

- `TS_CLIENT_HEADLESS=0` to disable headless launch
- `TS_CLIENT_HEADLESS=1` to force headless launch
- `TS_CLIENT_XVFB` to point at a specific `Xvfb` binary
- `TS_CLIENT_HEADLESS_DISPLAY` to choose a specific display such as `:140`

Client process state is stored under:

- `TS_CLIENT_STATE_DIR`, if set
- otherwise `$XDG_STATE_HOME/teamspeak-cli`
- otherwise `$HOME/.local/state/teamspeak-cli`

The tracked state includes a pid file and client log path that `ts client status` renders back to the user.

## Event Daemon State

The local TeamSpeak event daemon stores its pid file, log, hooks, and message inbox under:

- `TS_DAEMON_STATE_DIR`, if set
- otherwise `$XDG_STATE_HOME/teamspeak-cli/daemon`
- otherwise `$HOME/.local/state/teamspeak-cli/daemon`

Hook commands run through `/bin/sh -c` with the event JSON written to `stdin`. Useful environment variables include:

- `TS_HOOK_ID`
- `TS_EVENT_TYPE`
- `TS_EVENT_SUMMARY`
- `TS_EVENT_TIMESTAMP`
- `TS_MESSAGE_KIND`, when available
- `TS_MESSAGE_FROM`, when available
- `TS_MESSAGE_TEXT`, when available
