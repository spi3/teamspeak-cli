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
- `ts update [--release-tag TAG]`
- `ts plugin info`
- `ts sdk info`
- `ts daemon start [--foreground] [--poll-ms N]`
- `ts daemon stop [--timeout-ms N]`
- `ts daemon status`
- `ts connect`
- `ts disconnect`
- `ts mute`
- `ts unmute`
- `ts away [--message "<text>"]`
- `ts back`
- `ts status`
- `ts server info`

`ts sdk info` is a compatibility alias for `ts plugin info`.

`ts update` is for release-installer based user installs. It reads the install receipt, then reruns the official
release installer against the same prefix, TeamSpeak client directory, managed cache, and config path. By default it
installs the latest published release from `spi3/teamspeak-cli`; use `--release-tag TAG` to install a specific tag.

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
- `ts client logs [--count N]`
- `ts client list`
- `ts client get <id-or-name>`

### Messaging And Events

- `ts message send --target <client|channel> --id <id-or-name> --text "<message>"`
- `ts message inbox [--count N]`
- `ts playback status`
- `ts playback send --file <wav> [--clear] [--timeout-ms N]`
- `ts events watch [--count N] [--timeout-ms N]`
- `ts events hook add --type <event-type> --exec <command> [--message-kind <client|channel|server>]`
- `ts events hook list`
- `ts events hook remove <id>`

See [events.md](events.md) for the supported event types, backend availability, payload fields, hook filtering behavior,
and the boundary between domain events and media bridge frames.

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
- `mute` and `unmute` update your own TeamSpeak microphone mute state.
- `away` sets your own TeamSpeak away status and optionally an away message.
- `back` clears your own TeamSpeak away status and any away message set through `ts away`.
- `client start` and `client stop` inspect, launch, and stop the local TeamSpeak client process tracked by `ts`.
- `update` refreshes the installed `ts` binary, bundled plugin, and managed TeamSpeak client bundle from the official
  published release artifacts.
- `client logs` shows the tracked launcher log plus the most recent files under `~/.ts3client/logs`.
- `daemon start` launches a local background watcher that polls translated TeamSpeak events, journals incoming messages, and executes matching hook commands.
- `daemon stop` only stops the local watcher process. It does not disconnect the TeamSpeak client from the current server.
- `message inbox` reads the daemon-managed message journal, so it can show captured messages even when no `ts` command is actively watching events.
- `playback status` reports effective TeamSpeak capture/playback bindings, PulseAudio/PipeWire launch overrides, media queue counters, dropped chunks, and whether injected playback is attached to the custom capture transmit path.
- `playback send` streams a WAV file through the plugin media bridge and waits for the bridge to drain the queued playback before returning.
- the daemon hook matcher is generic, but the daemon-managed inbox currently journals only `message.received` events.

## Progress Streaming

These commands stream human-readable progress when output is `table`:

- `connect`
- `disconnect`
- `update`
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

The plugin media socket resolves in this order:

- `TS_MEDIA_SOCKET_PATH`, if set
- otherwise a path derived from the resolved control socket by replacing `.sock` with `-media.sock`
- if the control socket does not end in `.sock`, `.media` is appended instead

`ts plugin info` reports the control socket path, media socket path, media transport, accepted playback format, and the same media diagnostics shown by `ts playback status`.

`ts playback send --file message.wav` is the first-class CLI wrapper around playback injection. The current version is deliberately strict: the file must already be WAV PCM signed 16-bit little-endian, 48000 Hz, mono. Use `--clear` to interrupt any active bridge playback before sending the file, and `--timeout-ms N` to bound the total send and drain wait.

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
- `TS_CLIENT_XVFB_LIBRARY_PATH` to point at companion runtime libraries for a managed `Xvfb` binary
- `TS_CLIENT_XVFB_XKB_DIR` to point at companion XKB data for a managed `Xvfb` binary
- `TS_CLIENT_XVFB_BINARY_DIR` to point at companion helper binaries for a managed `Xvfb` binary
- `TS_CLIENT_HEADLESS_DISPLAY` to choose a specific display such as `:140`
- `TS_CLIENT_XDOTOOL` to point at a specific `xdotool` binary used to dismiss known onboarding dialogs
- `TS_CLIENT_XDOTOOL_LIBRARY_PATH` to point at the companion runtime libraries for that `xdotool` binary
- `TS_CLIENT_SYSTEMD_RUN=0|1|auto` to disable, force, or auto-detect the transient user `systemd-run` launch path
- `TS3_CLIENT_LDCONFIG` to point the runtime preflight at a specific `ldconfig` binary
- `TS3CLIENT_SKIP_AUDIO_PREFLIGHT=1` to skip the wrapper's PulseAudio/PipeWire metadata and routing preflight entirely
- `TS3CLIENT_SKIP_AUDIO_ROUTING=1` to keep the wrapper from provisioning isolated virtual audio devices automatically
- `PULSE_SINK` and `PULSE_SOURCE` to override the TeamSpeak playback and capture devices manually

When headless launch is selected and the current user has a working `systemd-run --user` environment, `ts client start`
prefers a transient user service so the TeamSpeak client can survive non-interactive session teardown. Set
`TS_CLIENT_SYSTEMD_RUN=0` to force the legacy detached launcher path, or `TS_CLIENT_SYSTEMD_RUN=1` to require the
transient service path.

On the first headless TeamSpeak launch, hidden onboarding dialogs can still block the plugin bridge. `ts client start`
tries to dismiss known license and identity dialogs with `xdotool` when available. If a headless session still wedges,
complete the TeamSpeak license and initial identity setup once on a visible display before relying on headless mode.

The installed `ts3client` wrapper also inspects the current PulseAudio/PipeWire defaults before launch. When the
default capture source is an output monitor such as `auto_null.monitor`, the wrapper provisions dedicated
`teamspeak_cli.playback` and `teamspeak_cli.capture` null sinks and launches TeamSpeak with `PULSE_SINK` set to the
playback sink and `PULSE_SOURCE` set to `teamspeak_cli.capture.monitor`. The detected defaults, routing decision, and
any provisioning warnings are written to the tracked launcher log that `ts client logs` reads back.

Client process state is stored under:

- `TS_CLIENT_STATE_DIR`, if set
- otherwise `$XDG_STATE_HOME/teamspeak-cli`
- otherwise `$HOME/.local/state/teamspeak-cli`

The tracked state includes a pid file and client log path that `ts client status` renders back to the user. When the
client was launched through transient user `systemd`, the state directory also records the unit name so `ts client status`
and `ts client stop` can keep following the managed session reliably.

`ts client logs` reads:

- the tracked launcher log from the client state directory
- up to three recent TeamSpeak log files from `$HOME/.ts3client/logs`
- the last 80 lines per file by default, or the last `N` lines with `--count N`

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
