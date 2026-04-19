# teamspeak-cli

`teamspeak-cli` is a companion CLI for a TeamSpeak 3 client plugin. The `ts` binary talks to a local plugin over a control socket, and that plugin talks to regular TeamSpeak servers through the official TeamSpeak 3 client plugin API.

This is not ServerQuery, not WebQuery, and not a standalone TeamSpeak SDK `ClientLib` client.

## Vision

The goal is a polished CLI in the style of `kubectl` or `gh` for client-side TeamSpeak workflows:

- inspect the current TeamSpeak client session
- connect the client to a server
- list channels and clients
- inspect a specific channel or client
- join channels
- send text messages
- stream asynchronous events
- grow later into a persistent shell or TUI

## Current State

The repository currently ships three important pieces:

- `ts`: the CLI application
- `built-test` backend: fully local development backend used in tests and CI
- `plugin` backend: a local control-socket backend that talks to a TeamSpeak client plugin

The default build targets the TeamSpeak-backed plugin runtime and auto-fetches the TeamSpeak-managed inputs it needs into `third_party/teamspeak/managed`. The `built-test` path keeps the project buildable and runnable without TeamSpeak installed.

## One-shot Install

On Linux `x86_64`, you can download the TeamSpeak 3 client, build the CLI and plugin, and install everything for the current user with:

```bash
./scripts/install.sh
```

By default the installer:

- caches TeamSpeak-managed downloads under `~/.cache/teamspeak-cli/install`
- builds a `Release` tree in `build-install`
- installs `ts` plus the repo docs/examples under `~/.local`
- installs the TeamSpeak client plus `ts3cli_plugin.so` under `~/.local/share/teamspeak-cli/teamspeak3-client`
- creates `~/.local/bin/ts3client` as a launcher for the installed TeamSpeak client
- installs `~/.local/bin/ts-uninstall` to remove the user-level install later
- initializes `~/.config/ts/config.ini` if it does not already exist

Run `./scripts/install.sh --help` for path overrides such as `--prefix`, `--client-dir`, and `--config-path`.

After install, make sure `~/.local/bin` is on `PATH`. `ts client start` will launch the TeamSpeak client in a managed headless `Xvfb` session when no GUI display is available, and it exports the active profile's control socket path so the started client and `ts plugin info` agree on the same socket. `ts3client` launches the installed wrapper directly. Use `ts client status` to check the tracked local process, enable the plugin if TeamSpeak has not already enabled it, and verify the bridge with `ts plugin info`. Set `TS_CLIENT_HEADLESS=0` to disable the managed headless launch or `TS_CLIENT_HEADLESS=1` to force it.

To remove the install later, run:

```bash
ts-uninstall
```

The uninstaller removes the installed binaries, launcher, TeamSpeak client bundle, managed download cache, installer build directory, and the generated config file when the installer created it. Use `ts-uninstall --keep-config` if you want to keep `~/.config/ts/config.ini`.

## Build

If you do not want to remember the CMake and test commands, use the top-level `Makefile`:

```bash
make help
```

To prefetch the TeamSpeak-managed assets without launching the environment:

```bash
make deps
```

That target caches the TeamSpeak 3 client bundle, the TeamSpeak 3 Client Plugin SDK, and `xdotool`, then writes default paths into `third_party/teamspeak/managed/deps.mk` and `third_party/teamspeak/managed/deps.env`.

### CLI and tests

```bash
make deps
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure -E ts_plugin_server_e2e_test
```

Equivalent Make targets:

```bash
make build
make test
```

### Built-test fallback

If you want the fully local fake/offline path instead of the TeamSpeak-backed default:

```bash
make build-built-test
make test-built-test
```

### TeamSpeak-backed build

The TeamSpeak 3 Client Plugin SDK is not bundled here. The default Makefile flow downloads it into `third_party/teamspeak/managed` and builds the plugin-backed tree for you:

```bash
make deps
make build
```

If you want to drive CMake directly, configure the default build like this:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DTS_ENABLE_TS3_PLUGIN=ON \
  -DTS_ENABLE_TS3_E2E=ON \
  -DTS3_MANAGED_DIR=third_party/teamspeak/managed

cmake --build build
```

If auto-discovery is not enough, pass:

- `-DTS3_PLUGIN_SDK_INCLUDE_DIR=/path/to/ts3client-pluginsdk/include`

## TeamSpeak-backed Runtime Prerequisites

To use the default `plugin` backend against a regular TeamSpeak server, you need:

- a TeamSpeak 3 client installation
- the TeamSpeak 3 Client Plugin SDK to build the plugin
- the built `ts3cli_plugin` shared library installed into the TeamSpeak client’s plugin directory
- the plugin enabled in the TeamSpeak client

The TeamSpeak client and the plugin SDK are proprietary TeamSpeak components and are not redistributed in this repository.

## Config and Profiles

`ts` uses an INI config file:

- default path: `~/.config/ts/config.ini`
- override with `--config /path/to/config.ini`

Initialize it with:

```bash
./build/ts config init
```

The starter config includes:

- `plugin-local`
- `built-test`

Switch profiles with:

```bash
./build/ts profile use plugin-local
```

The `plugin` backend uses a local control socket path. By default it resolves from:

- `TS_CONTROL_SOCKET_PATH`, if set
- otherwise an OS-appropriate local runtime path such as `/tmp/ts3cli-<uid>.sock`

Leave `control_socket_path=` blank in config unless you intentionally want to pin the plugin bridge to one fixed socket path.

## Example Commands

Initialize config and inspect profiles:

```bash
./build/ts config init
./build/ts profile list
```

Talk to the `built-test` backend:

```bash
./build/ts profile use built-test
./build/ts status
./build/ts channel list --json
./build/ts events watch --count 5
```

Talk to the plugin backend after the TeamSpeak client plugin is running:

```bash
./build/ts profile use plugin-local
./build/ts plugin info
./build/ts status
./build/ts channel list
./build/ts client list
./build/ts message send --target channel --id Lobby --text "hello"
```

Ask the TeamSpeak client plugin to open a new server connection:

```bash
./build/ts --profile plugin-local \
  --server voice.example.com:9987 \
  --nickname terminal \
  connect
```

`ts connect` now waits for the connection attempt to complete, with a 15 second timeout, and streams human-readable progress as TeamSpeak reports it. `ts disconnect`, `ts client start`, and `ts client stop` follow the same default pattern for human-oriented terminal use. Use `--json` or `--output yaml` if you explicitly want one structured result at the end instead of streamed prose.

## Sample Output

```text
$ ts plugin info
Backend      fake
Transport    in-process
Plugin       fake-plugin-host
Version      development
Available    yes
SocketPath   /tmp/ts3cli-1000.sock
Note         built-test plugin host for local development and CI
```

```text
$ ts channel list
ID  Name        Parent  Clients  Default
1   Lobby       -       1        yes
2   Engineering -       2        no
3   Operations  -       1        no
4   Breakout    2       0        no
```

## Testing

The default test suite covers:

- config parsing and persistence
- rendering
- session orchestration
- event translation
- CLI parse and dispatch
- socket backend round-trips
- end-to-end CLI execution against the `built-test` backend
- end-to-end CLI execution against a built-test plugin host over the same control socket used by the default plugin path

Run everything with:

```bash
ctest --test-dir build --output-on-failure
```

The repository does not yet automate a full TeamSpeak-client-plus-regular-server runtime test in CI. That path requires a local TeamSpeak client installation and plugin loading, which is documented as a manual integration step for now.

An optional TeamSpeak-backed E2E harness is available for local runs. It starts:

- a TeamSpeak 3 server in Docker
- a TeamSpeak 3 client under `Xvfb`
- the `ts3cli_plugin` shared library inside that client

and then drives the `ts` CLI against that TeamSpeak-backed session.

Example:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DTS_ENABLE_TS3_PLUGIN=ON \
  -DTS_ENABLE_TS3_E2E=ON \
  -DTS3_MANAGED_DIR=third_party/teamspeak/managed

cmake --build build

ctest --test-dir build --output-on-failure -R ts_plugin_server_e2e_test
```

Equivalent Make target:

```bash
make test-e2e
```

`make build`, `make test-e2e`, and `make env-up` automatically verify the managed TeamSpeak-backed runtime dependencies and bootstrap them into `third_party/teamspeak/managed` when they are missing.

If you want to pre-download the TeamSpeak client bundle, the TeamSpeak 3 Client Plugin SDK, and `xdotool` first:

```bash
make deps
```

After that, the generated defaults are enough for:

```bash
make build
make test-e2e
```

The live harness is intentionally opt-in. It still depends on `Xvfb` plus a working Docker daemon that the current user can access, but it now bootstraps the rest itself:

- downloads the default TeamSpeak 3 Linux client bundle into `third_party/teamspeak/managed/`
- verifies the archive SHA256 before extracting it
- reuses a system `xdotool` if available, or downloads `xdotool` plus `libxdo3` into the same cache on Debian/Ubuntu-style hosts

As of April 18, 2026, the default client bootstrap target is TeamSpeak 3 Linux `3.6.2` with SHA256 `59f110438971a23f904a700e7dd0a811cf99d4e6b975ba3aa45962d43b006422`, matching the official TeamSpeak downloads page.

Useful overrides:

- `TS3_MANAGED_DIR` to move the download cache
- `TS3_CLIENT_DIR` to reuse an already extracted TeamSpeak client tree
- `TS3_CLIENT_VERSION`, `TS3_CLIENT_URL`, `TS3_CLIENT_SHA256` to pin a different TeamSpeak client artifact
- `TS3_XDOTOOL` and `TS3_XDOTOOL_LIBRARY_PATH` to force a specific xdotool binary/runtime

The script still runs the client from a disposable copy of the extracted bundle so the cached or user-supplied client tree is not mutated.

For manual verification, bring the TeamSpeak-backed runtime up and leave it running:

```bash
./tests/e2e/run_plugin_server_env.sh ./build/ts ./build/ts3cli_plugin.so
```

Equivalent Make targets:

```bash
make env-up
make env-info
make env-ts ARGS='plugin info'
make env-ts ARGS='status'
make env-ts ARGS='channel list'
make env-down
```

That script prints an `env.sh` path you can source before running `ts` manually. Tear it down with:

```bash
./tests/e2e/stop_plugin_server_env.sh /path/to/env.state
```

## Limitations

- The `built-test` backend is the only backend exercised in CI.
- The default plugin shared library still depends on the TeamSpeak 3 Client Plugin SDK.
- The TeamSpeak-backed E2E test is local-only and still depends on a host `Xvfb` plus a working Docker daemon that the current user can access.
- Audio and voice controls are not exposed yet.

## Developer Notes

- Format with `clang-format -i` using the repository `.clang-format`.
- Keep CLI logic in `src/teamspeak_cli/cli/` and backend/plugin details out of command handlers.
- Prefer extending the socket/backend seam instead of letting TeamSpeak callback details leak into CLI code.

## Documentation

- [Architecture](docs/architecture.md)
- [Plugin integration](docs/sdk-integration.md)
- [Command reference](docs/commands.md)
- [Roadmap](docs/roadmap.md)
