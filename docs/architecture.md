# Architecture

## Goal

`ts` is a CLI companion for a TeamSpeak 3 client plugin. The CLI should stay scripting-friendly and terminal-first, while the plugin does the actual TeamSpeak client integration against regular TeamSpeak servers.

## Layers

- `src/teamspeak_cli/cli/`
  command parsing, help, completions, and dispatch
- `src/teamspeak_cli/config/`
  config discovery, profile loading, persistence, and overrides
- `src/teamspeak_cli/domain/`
  IDs, result/error types, session models, and event models
- `src/teamspeak_cli/events/`
  thread-safe event queue
- `src/teamspeak_cli/output/`
  table/detail rendering plus stable JSON/YAML output
- `src/teamspeak_cli/sdk/`
  backend abstraction plus concrete `fake`, `plugin` socket, and real plugin-host backends
- `src/teamspeak_cli/bridge/`
  local control-socket protocol and server implementation
- `src/teamspeak_cli/plugin/`
  TeamSpeak 3 plugin entry points
- `src/teamspeak_cli/session/`
  orchestration layer used by commands

## Runtime Shape

There are two main runtime modes.

### Development and CI

`ts` talks to the `fake` backend directly or over the socket bridge through `ts_fake_plugin_host`.

This keeps the project open-source, deterministic, and testable without TeamSpeak installed.

### Real Integration

1. The TeamSpeak 3 client loads `ts3cli_plugin`.
2. The plugin starts a local control socket.
3. `ts` connects to that socket through the `plugin` backend.
4. The plugin host backend uses the official TeamSpeak 3 plugin callbacks and exported client functions to inspect and control the live TeamSpeak client session.

## Backend Boundary

The central seam is `sdk::Backend`.

That interface exposes the operations the CLI cares about:

- connect and disconnect
- inspect plugin info and connection state
- inspect server, channel, and client state
- join channels
- send text messages
- read translated events

The CLI and session layers depend on that interface, not on TeamSpeak callback types.

## Bridge Protocol

The CLI-side `plugin` backend does not call TeamSpeak APIs directly. It speaks a simple line protocol over a local socket:

- requests are one command per connection
- replies are explicit success or error envelopes
- domain objects are serialized into small, typed record lines

That protocol is used by:

- the fake plugin-host process in tests
- the real TeamSpeak client plugin at runtime

## Event Translation

The real plugin backend translates TeamSpeak callbacks into stable domain events and pushes them into `events::EventQueue`.

That keeps callback-thread behavior and TeamSpeak-specific details inside the plugin/backend layer.

## Session Model

The CLI currently favors one-shot commands:

1. load profile
2. create backend
3. initialize backend
4. run one command
5. shut down backend object

For the `plugin` backend, this does not tear down the real TeamSpeak client session. It only tears down the CLI-side socket client object.

That keeps the command behavior predictable while leaving room for a future long-running shell or TUI.

## Testing Strategy

The repository now has three useful levels of coverage:

- unit tests for config, rendering, and session behavior
- direct bridge tests using `SocketBridgeServer` with the fake backend
- full CLI end-to-end tests where the built `ts` binary talks to `ts_fake_plugin_host`

The missing piece is a fully automated TeamSpeak-client-plus-regular-server integration run, which remains a manual local validation path for now.
