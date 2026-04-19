# Architecture

## Goal

`ts` should feel like a normal Unix CLI while delegating all live TeamSpeak work to a backend. In the real integration path, that backend is a TeamSpeak 3 client plugin reached over a local socket. In development and CI, that backend is a mock implementation.

## Main Targets

- `ts`: the CLI executable
- `ts3cli_plugin`: the TeamSpeak 3 client plugin shared library
- `ts_built_test_plugin_host`: a local mock plugin host used in tests

## Layer Map

- `src/teamspeak_cli/cli/`: argument parsing, help, completions, command dispatch, and client-process helpers
- `src/teamspeak_cli/config/`: config discovery, parsing, persistence, profile selection, and per-command overrides
- `src/teamspeak_cli/domain/`: shared models, IDs, result types, and event shapes
- `src/teamspeak_cli/events/`: thread-safe event queue
- `src/teamspeak_cli/output/`: table, detail, JSON, and YAML rendering
- `src/teamspeak_cli/sdk/`: backend interface plus `mock`, socket-backed `plugin`, and plugin-host implementations
- `src/teamspeak_cli/bridge/`: local socket protocol and bridge server
- `src/teamspeak_cli/plugin/`: TeamSpeak 3 plugin exports and callback entry points
- `src/teamspeak_cli/session/`: orchestration layer used by command handlers

## Runtime Modes

### Built-test mode

`ts` talks either directly to the mock backend or to that same mock backend through the socket bridge and `ts_built_test_plugin_host`.

This is the stable development path because it is:

- open source
- deterministic
- fast
- exercised in CI

### TeamSpeak-backed mode

1. The official TeamSpeak 3 client loads `ts3cli_plugin`.
2. The plugin starts a local control socket.
3. `ts` connects to that socket through the `plugin` backend.
4. The plugin host backend translates CLI requests into TeamSpeak 3 client API calls.

## Backend Boundary

The key seam is `sdk::Backend`.

That interface exposes the operations the CLI cares about:

- connect and disconnect
- inspect plugin info and connection state
- inspect server, channel, and client state
- join channels
- send text messages
- read translated events

The CLI and session layers depend on this interface, not on TeamSpeak callback types.

## Bridge Protocol

The CLI-side plugin backend never calls TeamSpeak APIs directly. It uses a small line-based local socket protocol:

- one request per connection
- explicit success and error envelopes
- serialized domain objects rather than TeamSpeak-native structs

The same protocol is used by:

- the built-test plugin host in automated tests
- the real TeamSpeak client plugin at runtime

## Event Translation

The TeamSpeak side translates client callbacks into stable domain events and pushes them into `events::EventQueue`.

That keeps callback-thread behavior and TeamSpeak-specific details behind the backend boundary.

## Command Lifecycle

The CLI currently favors one-shot commands:

1. load config and resolve a profile
2. create one backend object
3. initialize it
4. run one command
5. shut down the CLI-side backend object

For the real plugin backend, this does not stop the TeamSpeak client session. It only tears down the short-lived socket client that served the command.

## Testing Shape

The repo has three practical layers of coverage:

- unit tests for config, output, session, and event behavior
- bridge tests using the mock backend over the same socket protocol used by the live plugin path
- full CLI end-to-end tests where the built binary talks to `ts_built_test_plugin_host`

There is also a TeamSpeak-backed local harness, but it should still be treated as host-sensitive integration tooling rather than the primary stable test surface.
