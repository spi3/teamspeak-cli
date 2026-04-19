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
- `fake` backend: fully local development backend used in tests and CI
- `plugin` backend: a local control-socket backend that talks to a TeamSpeak client plugin

The fake backend keeps the project buildable and runnable without TeamSpeak installed. The real plugin shared library is optional and built only when you point CMake at a local TeamSpeak 3 Client Plugin SDK checkout.

## Build

If you do not want to remember the CMake and test commands, use the top-level `Makefile`:

```bash
make help
```

To prefetch the real runtime assets without launching the environment:

```bash
make deps-real
```

That target caches the TeamSpeak 3 client bundle, the TeamSpeak 3 Client Plugin SDK, and `xdotool`, then writes default paths into `.cache/ts3-real-e2e/deps.mk` and `.cache/ts3-real-e2e/deps.env`.

### CLI and tests

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Equivalent Make targets:

```bash
make build
make test
```

### TeamSpeak 3 client plugin

The TeamSpeak 3 Client Plugin SDK is not bundled here. Build the shared library with a local SDK checkout:

```bash
cmake -S . -B build-plugin -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DTS_ENABLE_TS3_PLUGIN=ON \
  -DTS3_PLUGIN_SDK_DIR=/path/to/ts3client-pluginsdk

cmake --build build-plugin --target ts3cli_plugin
```

If auto-discovery is not enough, pass:

- `-DTS3_PLUGIN_SDK_INCLUDE_DIR=/path/to/ts3client-pluginsdk/include`

Equivalent Make target:

```bash
make deps-real
make build-plugin
```

## Real Runtime Prerequisites

To use the real `plugin` backend against a regular TeamSpeak server, you need:

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

- `fake-default`
- `plugin-local`

Switch profiles with:

```bash
./build/ts profile use plugin-local
```

The `plugin` backend uses a local control socket path. By default it resolves from:

- `TS_CONTROL_SOCKET_PATH`, if set
- otherwise an OS-appropriate local runtime path such as `/tmp/ts3cli-<uid>.sock`

## Example Commands

Initialize config and inspect profiles:

```bash
./build/ts config init
./build/ts profile list
```

Talk to the fake backend:

```bash
./build/ts status
./build/ts channel list --json
./build/ts events watch --count 5
```

Talk to the real plugin backend after the TeamSpeak client plugin is running:

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

## Sample Output

```text
$ ts plugin info
Backend      fake
Transport    in-process
Plugin       fake-plugin-host
Version      development
Available    yes
SocketPath   /tmp/ts3cli-1000.sock
Note         fake plugin host for local development and CI
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
- end-to-end CLI execution against the fake backend
- end-to-end CLI execution against a fake plugin host over the same control socket used by the real plugin path

Run everything with:

```bash
ctest --test-dir build --output-on-failure
```

The repository does not yet automate a full TeamSpeak-client-plus-regular-server runtime test in CI. That path requires a local TeamSpeak client installation and plugin loading, which is documented as a manual integration step for now.

An optional live E2E harness is available for local runs. It starts:

- a real TeamSpeak 3 server in Docker
- a real TeamSpeak 3 client under `Xvfb`
- the `ts3cli_plugin` shared library inside that client

and then drives the `ts` CLI against that live session.

Example:

```bash
cmake -S . -B build-real -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DTS_ENABLE_TS3_PLUGIN=ON \
  -DTS_ENABLE_REAL_TS3_E2E=ON \
  -DTS3_PLUGIN_SDK_DIR=/path/to/ts3client-pluginsdk

cmake --build build-real

ctest --test-dir build-real --output-on-failure -R ts_real_plugin_server_e2e_test
```

Equivalent Make target:

```bash
make test-real
```

`make build-real`, `make test-real`, and `make env-up` automatically verify the managed TeamSpeak runtime dependencies and bootstrap them into `.cache/ts3-real-e2e` when they are missing.

If you want to pre-download the TeamSpeak client bundle, the TeamSpeak 3 Client Plugin SDK, and `xdotool` first:

```bash
make deps-real
```

After that, the generated defaults are enough for:

```bash
make build-real
make test-real
```

The live harness is intentionally opt-in. It still depends on a working Docker daemon plus `Xvfb`, but it now bootstraps the rest itself:

- downloads the default TeamSpeak 3 Linux client bundle into `.cache/ts3-real-e2e/`
- verifies the archive SHA256 before extracting it
- reuses a system `xdotool` if available, or downloads `xdotool` plus `libxdo3` into the same cache on Debian/Ubuntu-style hosts

As of April 18, 2026, the default client bootstrap target is TeamSpeak 3 Linux `3.6.2` with SHA256 `59f110438971a23f904a700e7dd0a811cf99d4e6b975ba3aa45962d43b006422`, matching the official TeamSpeak downloads page.

Useful overrides:

- `TS3_REAL_E2E_CACHE_DIR` to move the download cache
- `TS3_REAL_E2E_CLIENT_DIR` to reuse an already extracted TeamSpeak client tree
- `TS3_REAL_E2E_CLIENT_VERSION`, `TS3_REAL_E2E_CLIENT_URL`, `TS3_REAL_E2E_CLIENT_SHA256` to pin a different TeamSpeak client artifact
- `TS3_REAL_E2E_XDOTOOL` and `TS3_REAL_E2E_XDOTOOL_LIBRARY_PATH` to force a specific xdotool binary/runtime

The script still runs the client from a disposable copy of the extracted bundle so the cached or user-supplied client tree is not mutated.

For manual verification, bring the real runtime up and leave it running:

```bash
./tests/e2e/run_real_plugin_server_env.sh ./build-real/ts ./build-real/ts3cli_plugin.so
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
./tests/e2e/stop_real_plugin_server_env.sh /path/to/env.state
```

## Limitations

- The fake backend is the only backend exercised in CI.
- The real plugin shared library is build-gated behind a local TeamSpeak 3 Client Plugin SDK checkout.
- The live TeamSpeak E2E test is local-only and still depends on a host `Xvfb` plus a working Docker daemon.
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
