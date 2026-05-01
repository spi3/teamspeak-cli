# Plugin Integration

## Scope

This project targets the TeamSpeak 3 Client Plugin SDK.

The live runtime model is:

- the official TeamSpeak 3 client loads `ts3cli_plugin`
- the plugin exposes a local control socket plus a dedicated local media socket
- `ts` talks to the plugin over the control socket
- local media consumers talk to the plugin over the media socket
- the plugin talks to regular TeamSpeak servers through the official TeamSpeak client APIs

This project does not target ServerQuery, WebQuery, or a standalone `ClientLib` application.

## Upstream Sources

Useful upstream references:

- TeamSpeak downloads: <https://teamspeak.com/en/downloads>
- TeamSpeak 3 Client Plugin SDK: <https://github.com/TeamSpeak-Systems/ts3client-pluginsdk>

The repo does not commit TeamSpeak proprietary client or SDK artifacts. Those are supplied locally or bootstrapped into the managed cache.

## Build Inputs

Relevant CMake flags:

- `TS_ENABLE_TS3_PLUGIN=ON` to build `ts3cli_plugin`
- `TS_ENABLE_TS3_E2E=ON` to enable the TeamSpeak-backed Docker and `Xvfb` test
- `TS3_PLUGIN_SDK_DIR=/path/to/ts3client-pluginsdk`

Optional overrides:

- `TS3_PLUGIN_SDK_INCLUDE_DIR=/path/to/ts3client-pluginsdk/include`
- `TS3_MANAGED_DIR=/path/to/cache`
- `TS3_CLIENT_DIR=/path/to/extracted/TeamSpeak3-Client-linux_amd64`
- `TS3_CLIENT_VERSION=<version>`
- `TS3_CLIENT_URL=<download-url>`
- `TS3_CLIENT_SHA256=<expected-sha256>`
- `TS3_XDOTOOL=/path/to/xdotool`
- `TS3_XDOTOOL_LIBRARY_PATH=/path/to/xdotool/runtime/libs`
- `TS3_XVFB=/path/to/Xvfb`
- `TS3_XVFB_LIBRARY_PATH=/path/to/xvfb/runtime/libs`
- `TS3_XVFB_XKB_DIR=/path/to/xkb/data`
- `TS3_XVFB_BINARY_DIR=/path/to/xvfb/companion/bin`

Example plugin-enabled configure:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_MAKE_PROGRAM=ninja \
  -DTS_ENABLE_TS3_PLUGIN=ON \
  -DTS_ENABLE_TS3_E2E=ON \
  -DTS3_MANAGED_DIR=third_party/teamspeak/managed

cmake --build build
```

## What The Plugin Does

The plugin target:

- receives the TeamSpeak client function table through `ts3plugin_setFunctionPointers`
- starts the local bridge server in `ts3plugin_init`
- exposes status, channel, client, join, self mute, away, message, and event operations to the CLI
- translates selected TeamSpeak callbacks into domain events for `ts events watch`
- exposes a dedicated media bridge for speaker events, ingress voice chunks, and half-duplex playback injection
  through a plugin-managed custom capture device

See [events.md](events.md) for the domain event catalog and backend availability.

## Control And Media Sockets

The plugin and CLI must agree on the control socket path, and media consumers must discover the companion media socket path.

Resolution order on the CLI side is:

- `control_socket_path=` in the selected profile, if non-empty
- `TS_CONTROL_SOCKET_PATH`, if set
- otherwise the runtime default socket path

The starter `plugin-local` profile intentionally leaves `control_socket_path=` blank so tests, harnesses, and local environments can override it cleanly.

The media socket path resolves in this order:

- `TS_MEDIA_SOCKET_PATH`, if set
- otherwise a path derived from the resolved control socket by replacing `.sock` with `-media.sock`
- if the control socket does not end in `.sock`, `.media` is appended instead

`ts plugin info` reports both socket paths, the accepted playback media format, and media diagnostics such as effective
capture/playback bindings, queue counters, dropped chunks, and transmit-path attachment state. Use
`ts playback status` for the same diagnostics without the rest of the plugin metadata.

See [media-bridge.md](media-bridge.md) for the dedicated media frame contract.

## Local Runtime Checklist

1. Build `ts3cli_plugin`.
2. Install it into the TeamSpeak client plugin directory.
3. Start the TeamSpeak client.
4. Enable the plugin in the TeamSpeak client UI.
5. Run `ts plugin info`.
6. Run `ts status`.
7. Inspect an already connected tab or run `ts connect`.

## Managed Runtime Bootstrap

`make deps` prepares the managed runtime inputs used by the repo's TeamSpeak-backed workflow.

```bash
make deps
```

That command:

- resolves the TeamSpeak client
- resolves the TeamSpeak 3 Client Plugin SDK
- resolves `xdotool`
- resolves `Xvfb` for headless operation
- ensures PulseAudio-compatible audio control is available for media routing
- writes reusable defaults to `third_party/teamspeak/managed/deps.mk`
- writes shell exports to `third_party/teamspeak/managed/deps.env`

The bootstrap is intentionally pinned in `tests/e2e/runtime_common.sh`. It does not silently follow whatever upstream artifact happens to be newest.

## TeamSpeak-backed E2E Harness

`make test-e2e` runs `tests/e2e/run_plugin_server_e2e.sh`, which attempts to stand up:

- a Dockerized TeamSpeak 3 server
- a TeamSpeak 3 client under `Xvfb`
- `ts3cli_plugin` inside that client
- the `ts` CLI against that local plugin bridge

This harness is useful, but it is still best-effort local automation. It is more fragile than the mock path because it depends on:

- Docker access
- `Xvfb`, resolved from the managed cache when it is not already available on `PATH`
- `pactl` and a reachable PulseAudio-compatible server for automatic media routing
- TeamSpeak first-run dialogs and onboarding screens
- upstream client UI behavior
- host-specific runtime quirks such as audio and display behavior

If the harness fails, inspect the temp directory it prints. That directory contains the client log, display state, socket path, and other artifacts needed to debug the failure.

## Manual Runtime Environment

Use `make env-up` when you want the same TeamSpeak-backed runtime left running for manual checks instead of a self-cleaning test.

Useful commands:

```bash
make env-up
make env-info
make env-ts ARGS='plugin info'
make env-ts ARGS='status'
make env-ts ARGS='channel list'
make env-down
```

The environment helper prints:

- a state file path
- an `env.sh` file to source
- the config path
- the control socket path
- the chosen server port

The media socket path is derived from that control socket path unless `TS_MEDIA_SOCKET_PATH` overrides it.

## Current Limitations

- the TeamSpeak-backed build now runs in CI, but the Docker and `Xvfb` harness is still local-only
- the TeamSpeak-backed harness is still host-sensitive
- both local transports are Unix-domain-socket focused
- the media bridge is V1 half-duplex and single-consumer
- playback injection currently accepts `pcm_s16le @ 48000 Hz mono`
- playback injection temporarily replaces the active TeamSpeak capture device and then restores the previous device

## Development Without TeamSpeak

For day-to-day work, prefer the mock path:

- `make build-mock`
- `make test-mock`

That path exercises the same CLI, output, config, control socket, and media bridge contract without depending on
proprietary TeamSpeak runtime components.
