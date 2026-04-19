# teamspeak-cli

`teamspeak-cli` is a terminal-first CLI for controlling a TeamSpeak 3 client through a local plugin bridge. The `ts` binary talks to a local control socket, and the plugin loaded inside the official TeamSpeak 3 client talks to regular TeamSpeak servers through the TeamSpeak 3 Client Plugin SDK.

This project is not ServerQuery, not WebQuery, and not a standalone TeamSpeak `ClientLib` client.

## What You Get

- `ts`: the CLI users run
- `ts3cli_plugin.so`: the optional TeamSpeak 3 client plugin used for live integration
- `ts_built_test_plugin_host`: a local fake plugin host used by tests and CI
- a fully local `built-test` profile for development without TeamSpeak installed
- install and uninstall scripts for a user-level Linux `x86_64` setup

## Verified Paths

The following flows were checked against the current repository state during this documentation audit:

- `make build-built-test`
- `make test-built-test`
- `make build`
- `make test`
- `./scripts/install.sh --help`
- `./scripts/uninstall.sh --help`

The TeamSpeak-backed Docker and `Xvfb` harness is available, but it is still the least stable path in the repo. Treat `make test-e2e` and `make env-up` as local integration tools, not as the primary always-green workflow.

## Choose A Path

### Offline Development

Use this when you want to work on the CLI, config, rendering, socket protocol, or session layer without any proprietary TeamSpeak runtime on your machine.

```bash
make build-built-test
make test-built-test

./build-built-test/ts config init
./build-built-test/ts profile use built-test
./build-built-test/ts status
./build-built-test/ts channel list
```

The `built-test` profile uses the in-process fake backend. It is the fastest path for normal development and the path exercised in CI.

### TeamSpeak-backed Development

Use this when you need the real TeamSpeak 3 client plugin and runtime bridge.

```bash
make build
make test
```

`make build` does more than raw CMake defaults:

- bootstraps managed TeamSpeak inputs under `third_party/teamspeak/managed`
- configures the plugin-backed build
- builds `ts`, `ts3cli_plugin.so`, tests, and helper binaries

If you want to prefetch the managed runtime inputs first, run:

```bash
make deps
```

### User Install

On Linux `x86_64`, install the CLI, TeamSpeak client bundle, plugin, launcher, docs, and starter config for the current user with:

```bash
./scripts/install.sh
```

By default the installer:

- caches managed downloads under `~/.cache/teamspeak-cli/install`
- builds a `Release` tree in `build-install`
- installs `ts` and the bundled docs/examples under `~/.local`
- installs the TeamSpeak client and `ts3cli_plugin.so` under `~/.local/share/teamspeak-cli/teamspeak3-client`
- installs `~/.local/bin/ts3client` as a wrapper launcher for the installed client
- installs `~/.local/bin/ts-uninstall`
- initializes `~/.config/ts/config.ini` when that file does not already exist

Inspect overrides with:

```bash
./scripts/install.sh --help
```

Remove a user-level install later with:

```bash
ts-uninstall
```

Use `ts-uninstall --keep-config` if you want to preserve a config file that the installer created.

## Build Matrix

There are two important build surfaces:

- `make ...`: the repo's high-level workflow for normal use
- raw `cmake ...`: the low-level workflow when you want precise control

Raw CMake defaults do not build the TeamSpeak plugin target. They build the CLI, the built-test host, and the test binaries.

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

## Configuration And Profiles

`ts` uses an INI config file.

- default path: `~/.config/ts/config.ini`
- override per command with `--config /path/to/config.ini`
- initialize a starter file with `ts config init`

The starter config ships with two profiles:

- `built-test`: fully local fake backend
- `plugin-local`: local socket backend for a real TeamSpeak client plugin

Useful profile commands:

```bash
ts config init
ts config view
ts profile list
ts profile use built-test
```

The plugin socket path resolves in this order:

- `control_socket_path=` in the selected profile, if non-empty
- `TS_CONTROL_SOCKET_PATH`, if set
- otherwise a runtime-local default such as `$XDG_RUNTIME_DIR/ts3cli.sock` or `/tmp/ts3cli-<uid>.sock`

Leave `control_socket_path=` blank unless you intentionally want to pin one fixed socket path in config.

## Everyday Commands

The CLI is organized into small command groups:

- `version`
- `plugin info`
- `config init`, `config view`
- `profile list`, `profile use`
- `connect`, `disconnect`, `status`, `server info`
- `channel list`, `channel get`, `channel join`, `channel clients`
- `client status`, `client start`, `client stop`, `client list`, `client get`
- `message send`
- `events watch`
- `completion bash|zsh|fish|powershell`

Examples against the offline backend:

```bash
./build-built-test/ts --profile built-test status
./build-built-test/ts --profile built-test channel list --json
./build-built-test/ts --profile built-test channel clients Engineering
./build-built-test/ts --profile built-test events watch --count 5
```

Examples against the real plugin backend after the TeamSpeak client is running and the plugin is enabled:

```bash
ts --profile plugin-local plugin info
ts --profile plugin-local status
ts --profile plugin-local channel list
ts --profile plugin-local client list
ts --profile plugin-local message send --target channel --id Lobby --text "hello"
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

When output is `json` or `yaml`, they print one structured result at the end instead.

## TeamSpeak-backed Runtime Notes

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

- install `Xvfb`
- or set `TS_CLIENT_HEADLESS=0` to force a GUI launch on an existing display
- or set `TS_CLIENT_XVFB` and `TS_CLIENT_HEADLESS_DISPLAY` explicitly

If `make test-e2e` fails after the client starts:

- read the temp directory that the harness prints on failure
- inspect the client log, visible dialogs, and socket path in that temp dir
- expect to debug first-run TeamSpeak dialogs and host-specific runtime issues before treating the harness as stable

## Development Notes

- format C++ with `clang-format -i`
- prefer `rg` for searches
- keep CLI parsing and command dispatch in `src/teamspeak_cli/cli/`
- keep TeamSpeak-specific details behind the backend and socket bridge layers
- prefer extending the backend seam over leaking TeamSpeak callback details into the CLI

## Documentation

- [Architecture](docs/architecture.md)
- [Plugin integration](docs/sdk-integration.md)
- [Command reference](docs/commands.md)
- [Roadmap](docs/roadmap.md)
