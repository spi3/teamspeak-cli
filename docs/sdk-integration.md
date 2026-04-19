# Plugin Integration

## Scope

This project targets the TeamSpeak 3 Client Plugin SDK.

The supported real runtime model is:

- TeamSpeak 3 client plugin loaded into the official client
- local control socket exposed by that plugin
- `ts` talking to the plugin over that socket
- the plugin talking to regular TeamSpeak servers through the official TeamSpeak client APIs

This is not ServerQuery, WebQuery, or a standalone `ClientLib` app.

## Build Inputs

The repository does not ship TeamSpeak proprietary components. You can either supply the TeamSpeak 3 Client Plugin SDK locally or let `make deps-real` fetch the default SDK into the local cache.

The CMake inputs are:

- `TS_ENABLE_TS3_PLUGIN=ON`
- `TS_ENABLE_REAL_TS3_E2E=ON` for the live client-plus-server E2E harness
- `TS3_PLUGIN_SDK_DIR=/path/to/ts3client-pluginsdk`

Optional live-E2E overrides:

- `TS3_PLUGIN_SDK_INCLUDE_DIR=/path/to/ts3client-pluginsdk/include`
- `TS3_REAL_E2E_CACHE_DIR=/path/to/cache`
- `TS3_REAL_E2E_CLIENT_DIR=/path/to/extracted/TeamSpeak3-Client-linux_amd64`
- `TS3_REAL_E2E_CLIENT_VERSION=3.6.2`
- `TS3_REAL_E2E_CLIENT_URL=https://.../TeamSpeak3-Client-linux_amd64-3.6.2.run`
- `TS3_REAL_E2E_CLIENT_SHA256=...`
- `TS3_REAL_E2E_XDOTOOL=/path/to/xdotool`
- `TS3_REAL_E2E_XDOTOOL_LIBRARY_PATH=/path/to/xdotool/runtime/libs`

Example:

```bash
cmake -S . -B build-plugin -G Ninja \
  -DTS_ENABLE_TS3_PLUGIN=ON \
  -DTS3_PLUGIN_SDK_DIR=/path/to/ts3client-pluginsdk

cmake --build build-plugin --target ts3cli_plugin
```

## What The Plugin Does

The plugin target:

- receives the TeamSpeak function table through `ts3plugin_setFunctionPointers`
- starts a local bridge server in `ts3plugin_init`
- exposes status, channel, client, join, message, and event operations to the CLI
- translates selected TeamSpeak callbacks into domain events for `ts events watch`

## Control Socket

The plugin and the CLI agree on a local control socket path.

Resolution order:

- `TS_CONTROL_SOCKET_PATH`
- otherwise a runtime-local default path such as `/tmp/ts3cli-<uid>.sock`

The `plugin-local` starter profile leaves the socket configurable so local development and test harnesses can override it cleanly.

## Local Runtime Checklist

1. Build `ts3cli_plugin`.
2. Install it into the TeamSpeak client plugin directory for your platform.
3. Start the TeamSpeak client.
4. Enable the plugin in the client.
5. Run `ts plugin info` and `ts status`.
6. Either inspect an already connected client tab or run `ts connect`.

## Live E2E Harness

`tests/e2e/run_real_plugin_server_e2e.sh` drives the real runtime end to end:

- Dockerized TeamSpeak 3 server
- TeamSpeak 3 client under `Xvfb`
- `ts3cli_plugin` loaded into that client
- `ts` driving the plugin over the local control socket

The real-runtime bootstrap auto-fetches the TeamSpeak client bundle, the TeamSpeak 3 Client Plugin SDK, and `xdotool` when they are not already available locally:

- by default it downloads TeamSpeak 3 Linux `3.6.2`
- by default it clones the official TeamSpeak 3 Client Plugin SDK repository linked from TeamSpeak’s downloads page
- it verifies the archive against the official SHA256 before extraction
- it caches downloaded assets under `.cache/ts3-real-e2e/`
- it reuses a system `xdotool` when possible, otherwise it downloads `xdotool` and `libxdo3` on Debian/Ubuntu-style hosts

As of April 18, 2026, the default TeamSpeak client bootstrap settings match the official TeamSpeak downloads page:

- TeamSpeak 3 Linux `3.6.2`
- SHA256 `59f110438971a23f904a700e7dd0a811cf99d4e6b975ba3aa45962d43b006422`

If you want to fetch those runtime assets ahead of time without starting the client or the Docker server, run:

```bash
make deps-real
```

That writes reusable defaults to:

- `.cache/ts3-real-e2e/deps.mk`
- `.cache/ts3-real-e2e/deps.env`

After `make deps-real`, `make build-real` and `make test-real` can use the cached default paths without extra environment variables.

Even without a separate `make deps-real` step, `make build-real`, `make test-real`, and `make env-up` now verify those managed dependencies and bootstrap them automatically when needed.

The harness validates:

- plugin availability
- client connection establishment
- `status`
- `server info`
- `channel list`
- `channel get`
- `channel join`
- `client list`
- `client get`
- `message send`
- `events watch`
- disconnect

The script copies the TeamSpeak client bundle into a disposable temp directory before launching it, so the cached or user-supplied client tree is left unchanged.

## Manual Runtime Environment

Use `tests/e2e/run_real_plugin_server_env.sh` when you want the same real runtime left running for manual CLI checks instead of a self-cleaning test run.

It starts:

- the Dockerized TeamSpeak server
- `Xvfb`
- the TeamSpeak client with `ts3cli_plugin`

Like the self-cleaning E2E harness, it auto-downloads the TeamSpeak client bundle, the TeamSpeak 3 Client Plugin SDK, and xdotool into the local cache if they are missing.

It then prints:

- a state file for teardown
- an `env.sh` file you can `source`
- the config path
- the control socket path
- the chosen local server port

Stop it with `tests/e2e/stop_real_plugin_server_env.sh <state-file>`.

## Current Limitations

- The plugin build is optional and not exercised in CI.
- The live E2E path is local-only and still expects a working Docker daemon plus `Xvfb` on the host.
- The socket transport is implemented first on Unix-style local sockets.
- Voice/audio controls are intentionally out of scope for the first version.

## Development Without TeamSpeak

Use the fake backend and fake plugin-host E2E path for day-to-day development:

- `ctest --test-dir build --output-on-failure`

That path exercises the same CLI and socket layers as the real plugin runtime without requiring proprietary binaries in the repository or CI.
