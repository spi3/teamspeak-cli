# teamspeak-cli

`teamspeak-cli` is a terminal-first CLI for controlling a TeamSpeak 3 client through a local plugin bridge. The `ts` binary talks to a local control socket, and the plugin loaded inside the official TeamSpeak 3 client talks to regular TeamSpeak servers through the TeamSpeak 3 Client Plugin SDK.

This project is not ServerQuery, not WebQuery, and not a standalone TeamSpeak `ClientLib` client.

Quick links: [Highlights](#highlights) • [Project Status](#project-status) • [Quick Start](#quick-start) • [Installation](#installation) • [Usage](#usage) • [Configuration](#configuration-and-profiles) • [Development](#development) • [Troubleshooting](#troubleshooting) • [Documentation](#documentation) • [Contributing](CONTRIBUTING.md) • [Security](SECURITY.md) • [License](#license)

Docs: [Architecture](docs/architecture.md) • [Plugin integration](docs/sdk-integration.md) • [Command reference](docs/commands.md) • [Output format](docs/output-format.md) • [Event catalog](docs/events.md) • [Roadmap](docs/roadmap.md) • [Contributing](CONTRIBUTING.md) • [Security](SECURITY.md)

## Highlights

- `ts`: the CLI users run
- `ts3cli_plugin.so`: the optional TeamSpeak 3 client plugin used for live integration
- `ts_mock_bridge_host`: a local mock bridge host used by tests and CI
- a fully local `mock-local` profile for development without TeamSpeak installed
- install and uninstall scripts for a user-level Linux `x86_64` setup

## Project Status

- the primary supported install surface is user-level Linux `x86_64`
- `mock-local` is the fastest path for normal development and the most deterministic CI surface
- GitHub Actions runs `make test` on Ubuntu 22.04 for pushes to `main` and pull requests
- `make test-e2e` and `make env-up` are available, but they are still host-sensitive local integration tools rather than the primary always-green workflow

## Quick Start

### Released Linux Install

Use this when you want a normal user install from the latest published GitHub release without cloning the repo:

```bash
curl -fsSL https://raw.githubusercontent.com/spi3/teamspeak-cli/main/scripts/install-release.sh | bash
```

```bash
ts version
ts client start
```

### Offline Development

Use this when you want to work on the CLI, config, rendering, socket protocol, or session layer without any proprietary TeamSpeak runtime on your machine.

```bash
make build-mock
make test-mock

./build-mock/ts config init
./build-mock/ts --profile mock-local status
./build-mock/ts --profile mock-local channel list
```

### TeamSpeak-backed Development

Use this when you need the real TeamSpeak 3 client plugin and runtime bridge.

```bash
make build
make test

ts --profile plugin-local plugin info
ts --profile plugin-local status
```

`make build` does more than raw CMake defaults:

- bootstraps managed TeamSpeak inputs under `third_party/teamspeak/managed`
- configures the plugin-backed build
- builds `ts`, `ts3cli_plugin.so`, tests, and helper binaries

If you want to prefetch the managed runtime inputs first, run:

```bash
make deps
```

## Installation

### Install From GitHub

For normal user installs on Linux `x86_64`, use the published GitHub release installer directly without cloning the repo:

```bash
curl -fsSL https://raw.githubusercontent.com/spi3/teamspeak-cli/main/scripts/install-release.sh | bash
```

That installer:

- resolves the latest published GitHub release from `spi3/teamspeak-cli`
- downloads the release archive and checksum
- caches release downloads and TeamSpeak runtime assets under `~/.cache/teamspeak-cli/install`
- installs `ts` and the bundled docs/examples under `~/.local`
- installs the TeamSpeak client and `ts3cli_plugin.so` under `~/.local/share/teamspeak-cli/teamspeak3-client`
- installs `~/.local/bin/ts3client` as a wrapper launcher for the installed client
- resolves `Xvfb` for headless client launches, bootstrapping it into the managed cache when needed
- installs PulseAudio-compatible audio tooling for safe headless/media routing when it is missing
- installs `~/.local/bin/ts-uninstall`
- initializes `~/.config/ts/config.ini` when that file does not already exist

To pin a specific release instead of the latest one, run:

```bash
curl -fsSL https://raw.githubusercontent.com/spi3/teamspeak-cli/main/scripts/install-release.sh | bash -s -- --release-tag vX.Y.Z
```

If you already have a checkout and want to inspect or customize the release installer locally first, run:

```bash
./scripts/install-release.sh --help
```

Remove a user-level install later with:

```bash
ts-uninstall
```

Use `ts-uninstall --keep-config` if you want to preserve a config file that the installer created.

After installing from a published release, update the installed CLI, bundled plugin, and managed TeamSpeak client
bundle with:

```bash
ts update
```

`ts update` reads the install receipt, then reruns the release installer against the same install paths using the
latest published release from `spi3/teamspeak-cli`. To pin a specific release, run `ts update --release-tag vX.Y.Z`.

### Local Checkout Install

If you want the installer to build directly from your local checkout instead of using published release artifacts, run:

```bash
./scripts/install.sh
```

By default the local-checkout installer:

- caches managed downloads under `~/.cache/teamspeak-cli/install`
- builds a `Release` tree in `build-install`
- installs `ts` and the bundled docs/examples under `~/.local`
- installs the TeamSpeak client and `ts3cli_plugin.so` under `~/.local/share/teamspeak-cli/teamspeak3-client`
- installs `~/.local/bin/ts3client` as a wrapper launcher for the installed client
- resolves `Xvfb` for headless client launches, bootstrapping it into the managed cache when needed
- installs PulseAudio-compatible audio tooling for safe headless/media routing when it is missing
- installs `~/.local/bin/ts-uninstall`
- initializes `~/.config/ts/config.ini` when that file does not already exist

Inspect local-build installer overrides with:

```bash
./scripts/install.sh --help
```

## Usage

The CLI is organized into small command groups:

- `version`
- `update`
- `plugin info`
- `config init`, `config path`, `config view`
- `profile create`, `profile list`, `profile show`, `profile set`, `profile unset`, `profile delete`, `profile use`
- `connect`, `disconnect`, `status`, `server info`, `server group apply`
- `daemon start`, `daemon stop`, `daemon status`
- `channel list`, `channel get`, `channel join`, `channel rename`, `channel clients`
- `client status`, `client start`, `client inspect-windows`, `client stop`, `client list`, `client get`
- `message send`, `message inbox`
- `playback status`, `playback send`
- `events watch`, `events hook add`, `events hook list`, `events hook remove`
- `completion bash|zsh|fish|powershell`

Examples against the offline backend:

```bash
./build-mock/ts --profile mock-local status
./build-mock/ts --profile mock-local channel list --json
./build-mock/ts --profile mock-local channel clients Engineering
./build-mock/ts --profile mock-local channel rename Engineering --name Platform
./build-mock/ts --profile mock-local server group apply --group Operator --client alice
./build-mock/ts daemon start
./build-mock/ts message inbox
./build-mock/ts --profile mock-local events watch --count 5
./build-mock/ts --profile mock-local events watch --count 5 --output ndjson
```

Examples against the real plugin backend after the TeamSpeak client is running and the plugin is enabled:

```bash
ts --profile plugin-local plugin info
ts --profile plugin-local status
ts --profile plugin-local channel list
ts --profile plugin-local client list
ts --profile plugin-local channel rename Engineering --name Platform
ts --profile plugin-local server group apply --group Operator --client alice
ts daemon start
ts message inbox
ts --profile plugin-local message send --target channel --id Lobby --text "hello"
ts --profile plugin-local playback status
ts --profile plugin-local playback send --file ./message.wav
```

To connect through the real client:

```bash
ts --profile plugin-local \
  --server voice.example.com:9987 \
  --nickname terminal \
  connect
```

When output is `table`, these commands stream human-readable progress by default:

- `connect`
- `disconnect`
- `client start`
- `client stop`

When output is `json` or experimental `yaml`, they print one structured result at the end instead. Use JSON for stable automation.

`events watch --output ndjson` prints one JSON event object per line for line-oriented consumers. It currently waits for the requested count or timeout, then writes and flushes each returned event line.

For shell scripts that need one JSON scalar, use `--field <path>` with JSON output:

```bash
ts --json status --field phase
ts --json plugin info --field media_diagnostics.transmit_path_ready
```

For human tables, `--no-headers` removes header rows and `--wide` adds extra columns to supported list tables. These options do not change JSON, YAML, NDJSON, or `--field` output.

To capture messages and trigger local scripts without keeping `ts events watch` attached, start the local daemon:

```bash
ts daemon start
ts events hook add --type message.received --message-kind client --exec 'notify-send "TeamSpeak DM" "$TS_MESSAGE_FROM: $TS_MESSAGE_TEXT"'
ts message inbox --count 20
```

See [docs/events.md](docs/events.md) for the supported event types, backend availability, payload fields, and the
separate media bridge event surface.

See [docs/output-format.md](docs/output-format.md) for the stdout/stderr and machine-output contract.

## Configuration And Profiles

`ts` uses an INI config file.

- path precedence: `--config /path/to/config.ini`, then `TS_CONFIG_PATH`, then `$XDG_CONFIG_HOME/ts/config.ini` or the default `~/.config/ts/config.ini`
- inspect the resolved path with `ts config path`
- initialize a starter file with `ts config init`

Config files can include `server_password=` and `channel_password=` values. Treat them as private; `ts` writes config
files with owner-only permissions on Unix. Profile list/show output redacts or omits secret values.

The starter config ships with two profiles:

- `mock-local`: fully local mock backend
- `plugin-local`: local socket backend for a real TeamSpeak client plugin

Useful profile commands:

```bash
ts config path
ts config init
ts config view
ts profile list
ts profile show plugin-local
ts profile set plugin-local nickname terminal
ts profile unset plugin-local default_channel
ts profile delete old-profile
ts profile use mock-local
```

The plugin socket path resolves in this order:

- `control_socket_path=` in the selected profile, if non-empty
- `TS_CONTROL_SOCKET_PATH`, if set
- otherwise a runtime-local default such as `$XDG_RUNTIME_DIR/ts3cli.sock` or `/tmp/ts3cli-<uid>.sock`

Leave `control_socket_path=` blank unless you intentionally want to pin one fixed socket path in config.

## Development

### Common Make Workflows

- `make help`: list the supported top-level workflows
- `make build-mock`: build the offline development tree
- `make test-mock`: run the offline suite
- `make build`: build the TeamSpeak-backed tree and bootstrap managed dependencies
- `make test`: run the default automated suite without the Docker/`Xvfb` E2E case
- `make test-e2e`: run the TeamSpeak-backed local integration harness
- `make env-up`, `make env-info`, `make env-down`: keep the TeamSpeak-backed environment running for manual checks

### Release

Cut a tagged release from a clean branch with:

```bash
./scripts/release.sh patch
./scripts/release.sh minor
./scripts/release.sh 1.2.3
```

That script bumps the version in the CMake version files, runs `make test`,
commits the release change, pushes an annotated `vX.Y.Z` tag, and creates the
GitHub release entry. The tag push then triggers the release workflow to build
and upload the packaged release assets.

### Raw CMake

There are two important build surfaces:

- `make ...`: the repo's high-level workflow for normal use
- raw `cmake ...`: the low-level workflow when you want precise control

Raw CMake defaults do not build the TeamSpeak plugin target. They build the CLI, the mock bridge host, and the test binaries.

Example plain CMake configure:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_MAKE_PROGRAM=ninja
```

To build the TeamSpeak plugin with raw CMake, opt in explicitly:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_MAKE_PROGRAM=ninja \
  -DTS_ENABLE_TS3_PLUGIN=ON \
  -DTS_ENABLE_TS3_E2E=ON \
  -DTS3_MANAGED_DIR=third_party/teamspeak/managed
```

If SDK auto-discovery is not enough, pass `-DTS3_PLUGIN_SDK_INCLUDE_DIR=/path/to/ts3client-pluginsdk/include`.

If `cmake` or `ninja` are not on `PATH`, prefer the top-level `make` targets instead of hardcoding one machine's fallback toolchain path.

### TeamSpeak-backed Runtime Notes

The repo-managed TeamSpeak runtime is intentionally pinned, not floating:

- the Linux client bootstrap defaults live in `tests/e2e/runtime_common.sh`
- the managed cache lives under `third_party/teamspeak/managed`
- `make deps` writes resolved paths to `third_party/teamspeak/managed/deps.mk` and `deps.env`

Useful overrides:

- `TS3_MANAGED_DIR`
- `TS3_CLIENT_DIR`
- `TS3_CLIENT_VERSION`
- `TS3_CLIENT_URL`
- `TS3_CLIENT_SHA256`
- `TS3_XDOTOOL`
- `TS3_XDOTOOL_LIBRARY_PATH`
- `TS3_XVFB`
- `TS3_XVFB_LIBRARY_PATH`
- `TS3_XVFB_XKB_DIR`
- `TS3_XVFB_BINARY_DIR`

The TeamSpeak-backed local harness starts:

- a TeamSpeak 3 server in Docker
- a TeamSpeak 3 client under `Xvfb`
- `ts3cli_plugin.so` inside that client

Run it directly:

```bash
make test-e2e
```

Or keep it running for manual checks:

```bash
make env-up
make env-info
make env-ts ARGS='plugin info'
make env-ts ARGS='status'
make env-down
```

This path is still host-sensitive. TeamSpeak first-run dialogs, display quirks, audio backends, or upstream UI changes can still break automation even when the plugin itself loads correctly.

### Development Notes

- format C++ with `clang-format -i`
- prefer `rg` for searches
- keep CLI parsing and command dispatch in `src/teamspeak_cli/cli/`
- keep TeamSpeak-specific details behind the backend and socket bridge layers
- prefer extending the backend seam over leaking TeamSpeak callback details into the CLI

## Troubleshooting

If `ts plugin info` or `ts status` says the plugin bridge is unavailable:

- run `ts client start`
- make sure the TeamSpeak client plugin is enabled
- confirm the CLI and client agree on the same socket path
- use `ts plugin info` again to inspect the resolved socket path and backend note

If `ts client start` cannot find a launcher:

- install the user-level bundle with `./scripts/install.sh`
- or set `TS_CLIENT_LAUNCHER`
- or set `TS3_CLIENT_DIR` to a TeamSpeak client tree containing `ts3client_runscript.sh`

If headless launch fails:

- rerun the installer so it can resolve or bootstrap `Xvfb`
- on Debian/Ubuntu, make sure `x11-xkb-utils` and `xkb-data` are installed if `/usr/bin/xkbcomp` or XKB data is missing
- on Debian/Ubuntu, make sure `pulseaudio-utils` and either `pulseaudio` or `pipewire-pulse` are installed if the launcher reports that audio preflight is unavailable
- or set `TS_CLIENT_HEADLESS=0` to force a GUI launch on an existing display
- or set `TS_CLIENT_XVFB` and `TS_CLIENT_HEADLESS_DISPLAY` explicitly

If headless `ts connect` times out while `ts plugin info` is responsive:

- run `ts client inspect-windows` to list visible TeamSpeak dialogs on the tracked X11 display
- complete any required TeamSpeak license or identity setup before retrying `ts connect`
- use `ts client start --accept-license` or `TS_CLIENT_ACCEPT_LICENSE=1` only if you accept the TeamSpeak license terms and want first-run headless automation to click the license dialog
- use `ts client logs` alongside the window list when the display cannot be inspected

If `ts client start` succeeds in a headless non-interactive session but the TeamSpeak client disappears again when that
session ends:

- leave `TS_CLIENT_SYSTEMD_RUN` unset or set it to `1` so `ts client start` can use a transient `systemd-run --user` unit
- if your user does not have a working user `systemd` manager, set `TS_CLIENT_SYSTEMD_RUN=0` to fall back to the legacy detached launcher path
- confirm `systemd-run --user true` works for the current user if you expect the launched client to survive session teardown

If `ts client start` reports a missing shared library such as `libXi.so.6`:

- install a system package that provides that library
- or add the library under the TeamSpeak client `runtime-libs` tree
- if you used the bundled installer, rerun it to refresh the managed runtime library bundle

If the installed `ts3client` launcher prints `QCoreApplication::applicationDirPath` and then the TeamSpeak client segfaults:

- this is usually an upstream TeamSpeak Linux audio-stack crash, not a `ts3cli_plugin.so` startup failure
- check custom PipeWire or PulseAudio sinks and sources for missing `device.description` or `node.description`
- the generated `ts3client` wrapper now warns when `pactl` reports sinks or sources without a `Description:` field
- set `TS3CLIENT_SKIP_AUDIO_PREFLIGHT=1` only if you need to suppress that warning and have already ruled audio metadata out

If `make test-e2e` fails after the client starts:

- read the temp directory that the harness prints on failure
- inspect the client log, visible dialogs, and socket path in that temp dir
- expect to debug first-run TeamSpeak dialogs and host-specific runtime issues before treating the harness as stable

## Documentation

- [Architecture](docs/architecture.md)
- [Plugin integration](docs/sdk-integration.md)
- [Command reference](docs/commands.md)
- [Output format](docs/output-format.md)
- [Event catalog](docs/events.md)
- [Roadmap](docs/roadmap.md)

## License

Released under the [MIT License](LICENSE).
