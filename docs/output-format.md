# CLI Output Format

This document describes the current output contract for `ts`.

## Streams

Successful command results are written to `stdout`.

Failures are written to `stderr`. Scripts should treat a non-zero exit status as failure and read the failure text from `stderr`.

## Formats

`ts` supports these output formats:

- `--output table`: human-oriented text. This is the default.
- `--json`: shorthand for `--output json`.
- `--output json`: one JSON value on `stdout`.
- `--output yaml`: one YAML value on `stdout`. YAML output is experimental.

For `json` and experimental `yaml`, successful commands emit exactly one top-level value. The value may be an object or an array depending on the command.

`--field <path>` is available only with `--json` or `--output json`. It runs the command normally, then extracts one scalar value from the JSON result using simple dot-separated object keys. Strings are written as raw text, numbers and booleans are written as their JSON literals, and `null` is written as `null`. Selecting a missing field, an invalid path, or an object/array returns a non-zero error.

Table output is for people reading a terminal. It may change labels, spacing, columns, order, wrapping, and explanatory text between releases. Do not parse table output in scripts.

Table controls apply only when output is `table`:

- `--no-headers` removes the table header row while keeping the same data rows.
- `--wide` adds extra columns for supported commands without changing their default table columns.

JSON, experimental YAML, and `--field` output are unaffected by table controls. `--wide` currently adds `Subscribed` to `ts channel list`, `Unique Identity` to `ts client list` and `ts channel clients`, and event type/summary columns to `ts message inbox`. `ts events hook list --wide` is accepted as a no-op because the default hook table already exposes all stored hook fields.

Progress-producing commands stream progress only for human/table output. With `--json` or `--output yaml`, those commands suppress progress and print one structured result at the end.

Current progress-producing commands include:

- `ts connect`
- `ts disconnect`
- `ts update`
- `ts client start`
- `ts client stop`

## Script Contract

Scripts may rely on:

- exit status: zero means success and non-zero means failure
- successful command results appearing on `stdout`
- failures appearing on `stderr`
- `--json` and `--output json` producing one complete JSON value per command invocation
- `--field <path>` producing one raw scalar line from JSON output
- JSON top-level type for the documented commands below, unless release notes document a compatibility change
- JSON field names that are documented or covered by tests

Scripts must not rely on:

- table output spacing, labels, row order, columns, or wording
- progress text wording or timing
- parsing progress text as data
- `json` or `yaml` output containing multiple streamed values for one command invocation
- undocumented debug fields, especially fields that appear only with `--debug`
- YAML as the stable automation contract

Prefer JSON for automation. YAML currently uses a custom renderer and should remain a human-adjacent convenience format until parser-backed tests and a real serializer exist.

Examples:

```bash
ts --json status --field phase
ts --json plugin info --field media_diagnostics.transmit_path_ready
```

## JSON Compatibility

Documented JSON fields are stable unless the field table marks them as best-effort or debug-only.

- Stable fields keep their name, top-level type, JSON type, and nullability across compatible releases.
- Best-effort fields keep their name and type, but the value may be empty, unavailable, backend-specific, or timing-dependent.
- Debug-only fields are omitted unless `--debug` is used. Scripts should not depend on them for normal operation.

Additional fields may be added to documented objects in future releases. Scripts should ignore unknown fields.

Timestamps are UTC strings in the form `YYYY-MM-DDTHH:MM:SSZ`.

## Shared JSON Shapes

### Connection State

Used by `ts status`, and nested under `state` in `ts connect` and `ts disconnect`.

| Field | Type | Nullable | Stability | Notes |
| --- | --- | --- | --- | --- |
| `backend` | string | no | stable | Backend name, such as `mock` or `plugin`. |
| `connection` | string | no | stable | Backend connection handle encoded as a string. |
| `identity` | string | no | best-effort | TeamSpeak identity string; may be empty if unavailable. |
| `mode` | string | no | stable | Session mode, such as `one-shot` or `plugin-control`. |
| `nickname` | string | no | best-effort | Current or configured nickname; may be empty if unavailable. |
| `phase` | string | no | stable | `disconnected`, `connecting`, or `connected`. |
| `port` | number | no | stable | TeamSpeak server port. |
| `profile` | string | no | stable | Active profile name. |
| `server` | string | no | stable | Server host/address; may be empty when disconnected. |

### Channel

Used by `ts channel list` and nested under `channel` in `ts channel clients`.

| Field | Type | Nullable | Stability | Notes |
| --- | --- | --- | --- | --- |
| `client_count` | number | no | best-effort | Backend-reported client count for the channel. |
| `id` | string | no | stable | Channel ID encoded as a string. |
| `is_default` | boolean | no | stable | Whether the channel is the server default. |
| `name` | string | no | stable | Channel display name. |
| `parent_id` | string or null | yes | stable | Parent channel ID as a string, or `null` for top-level channels. |
| `subscribed` | boolean | no | best-effort | Subscription state when the backend reports it. |

### Client

Used by `ts client list` and nested under `clients` in `ts channel clients`.

| Field | Type | Nullable | Stability | Notes |
| --- | --- | --- | --- | --- |
| `channel_id` | string or null | yes | stable | Current channel ID as a string, or `null` when unavailable. |
| `id` | string | no | stable | Client ID encoded as a string. |
| `nickname` | string | no | stable | Client display name. |
| `self` | boolean | no | stable | Whether the client is the local user. |
| `talking` | boolean | no | best-effort | Current speaking state when available. |
| `unique_identity` | string | no | best-effort | TeamSpeak unique identity; may be empty if unavailable. |

### Event

Used by `ts events watch`, and nested under `lifecycle` in `ts connect` and `ts disconnect`.

| Field | Type | Nullable | Stability | Notes |
| --- | --- | --- | --- | --- |
| `fields` | object | no | stable | Event-specific string key/value fields. Keys vary by event type. |
| `summary` | string | no | best-effort | Human-readable event summary. |
| `timestamp` | string | no | best-effort | UTC timestamp. Scripts should parse the format, not compare exact values. |
| `type` | string | no | stable | Event type, such as `connection.connected` or `client.talking`. |

### Media Diagnostics

Used by `ts playback status` and nested under `media_diagnostics` in `ts plugin info`.

| Field | Type | Nullable | Stability | Notes |
| --- | --- | --- | --- | --- |
| `active_speaker_count` | number | no | best-effort | Count of currently active speakers when available. |
| `capture` | object | no | stable | Audio device binding with `known`, `mode`, `device`, and `is_default`. |
| `captured_voice_edit_attached` | boolean | no | best-effort | Plugin media bridge diagnostic. |
| `consumer_connected` | boolean | no | best-effort | Whether a media consumer is connected. |
| `custom_capture_device_id` | string | no | best-effort | Custom capture device ID, or empty when unavailable. |
| `custom_capture_device_name` | string | no | best-effort | Custom capture device display name, or empty when unavailable. |
| `custom_capture_device_registered` | boolean | no | best-effort | Whether the plugin registered the custom capture device. |
| `custom_capture_path_available` | boolean | no | best-effort | Whether the transmit path can use the custom capture path. |
| `dropped_audio_chunks` | number | no | best-effort | Dropped inbound audio chunks. |
| `dropped_playback_chunks` | number | no | best-effort | Dropped outbound playback chunks. |
| `injected_playback_attached_to_capture` | boolean | no | best-effort | Whether playback injection is attached to capture. |
| `last_error` | string | no | best-effort | Last media bridge error, or empty. |
| `playback` | object | no | stable | Audio device binding with `known`, `mode`, `device`, and `is_default`. |
| `playback_active` | boolean | no | best-effort | Whether playback is active or queued. |
| `pulse_sink` | string | no | best-effort | PulseAudio sink, or empty. |
| `pulse_source` | string | no | best-effort | PulseAudio source, or empty. |
| `pulse_source_is_monitor` | boolean | no | best-effort | Whether the source is a monitor source. |
| `queued_playback_samples` | number | no | best-effort | Queued playback sample count. |
| `transmit_path` | string | no | best-effort | Effective transmit path, or empty. |
| `transmit_path_ready` | boolean | no | best-effort | Whether playback can be transmitted through the selected path. |

Audio device binding fields:

| Field | Type | Nullable | Stability | Notes |
| --- | --- | --- | --- | --- |
| `device` | string | no | best-effort | Device name, or empty when unavailable. |
| `is_default` | boolean | no | best-effort | Whether the device is the backend default. |
| `known` | boolean | no | stable | Whether the binding is known. |
| `mode` | string | no | best-effort | Device mode, or empty when unavailable. |

### Error Object

For JSON output, failures are written to `stderr` as one object:

| Field | Type | Nullable | Stability | Notes |
| --- | --- | --- | --- | --- |
| `category` | string | no | stable | Error category, such as `cli`, `config`, `bridge`, or `media`. |
| `code` | string | no | stable | Machine-readable error code. |
| `message` | string | no | stable | Human-readable error message. |
| `hints` | array of strings | omitted when empty | stable | Structured next steps when available. |
| `details` | object | omitted unless `--debug` | debug-only | String key/value diagnostic details. Hint internals are not duplicated here. |

Example:

```json
{"category":"bridge","code":"socket_connect_failed","hints":["Run `ts client start` to launch the local TeamSpeak client.","Run `ts plugin info` to verify the ts3cli plugin bridge is available."],"message":"Unable to read TeamSpeak status because the TeamSpeak client is not running or the ts3cli plugin is unavailable."}
```

With `--debug`, non-hint details are added:

```json
{"category":"bridge","code":"socket_connect_failed","details":{"socket_path":"/tmp/ts3cli.sock"},"hints":["Run `ts client start` to launch the local TeamSpeak client.","Run `ts plugin info` to verify the ts3cli plugin bridge is available."],"message":"Unable to read TeamSpeak status because the TeamSpeak client is not running or the ts3cli plugin is unavailable."}
```

## Command JSON Schemas

### `ts status`

Top-level type: object. Shape: [Connection State](#connection-state).

### `ts plugin info`

Top-level type: object.

| Field | Type | Nullable | Stability | Notes |
| --- | --- | --- | --- | --- |
| `backend` | string | no | stable | Backend name. |
| `media_diagnostics` | object | no | stable | See [Media Diagnostics](#media-diagnostics). |
| `media_format` | string | no | best-effort | Accepted media format, or empty when unavailable. |
| `media_socket_path` | string | no | best-effort | Media socket path, or empty when unavailable. |
| `media_transport` | string | no | best-effort | Media transport, or empty when unavailable. |
| `note` | string | no | best-effort | Backend-specific note, or empty. |
| `plugin_available` | boolean | no | stable | Whether the plugin bridge is available. |
| `plugin_name` | string | no | best-effort | Plugin name, or empty when unavailable. |
| `plugin_version` | string | no | best-effort | Plugin version, or empty when unavailable. |
| `socket_path` | string | no | stable | Control socket path used by the backend. |
| `transport` | string | no | stable | Control transport name. |

### `ts channel list`

Top-level type: array. Each item is a [Channel](#channel).

### `ts channel clients`

Without an argument, top-level type is an array of channel-client groups. With `id-or-name`, top-level type is one channel-client group object.

| Field | Type | Nullable | Stability | Notes |
| --- | --- | --- | --- | --- |
| `channel` | object | no | stable | See [Channel](#channel). |
| `clients` | array | no | stable | Array of [Client](#client) objects. Empty channels use `[]`. |

### `ts client list`

Top-level type: array. Each item is a [Client](#client).

### `ts connect`

Top-level type: object.

| Field | Type | Nullable | Stability | Notes |
| --- | --- | --- | --- | --- |
| `connected` | boolean | no | stable | Whether the connection reached `connected`. |
| `lifecycle` | array | no | stable | Array of [Event](#event) objects collected while waiting. |
| `result` | string | no | stable | `connected`, `timeout`, or `failed`. |
| `state` | object | no | stable | Final [Connection State](#connection-state). |
| `timed_out` | boolean | no | stable | Whether waiting timed out. |
| `timeout_ms` | number | no | stable | Wait timeout in milliseconds. |

### `ts disconnect`

Top-level type: object.

| Field | Type | Nullable | Stability | Notes |
| --- | --- | --- | --- | --- |
| `disconnected` | boolean | no | stable | Whether the connection reached `disconnected`. |
| `lifecycle` | array | no | stable | Array of [Event](#event) objects collected while waiting. |
| `result` | string | no | stable | `disconnected`, `timeout`, or `failed`. |
| `state` | object | no | stable | Final [Connection State](#connection-state). |
| `timed_out` | boolean | no | stable | Whether waiting timed out. |
| `timeout_ms` | number | no | stable | Wait timeout in milliseconds. |

### `ts playback status`

Top-level type: object. Shape: [Media Diagnostics](#media-diagnostics).

### `ts events watch`

Top-level type: array. Each item is an [Event](#event). The array is emitted after the command exits. If no events arrive before the timeout, the result is `[]`.

## JSON Examples

The examples below use the `mock-local` profile. Real plugin-backed output uses the same command-level shape, but values vary by TeamSpeak server, client state, and plugin runtime.

### `ts status`

`ts status --json` emits an object:

```json
{"backend":"mock","connection":"42","identity":"mock-local-identity","mode":"one-shot","nickname":"terminal","phase":"connected","port":9987,"profile":"mock-local","server":"127.0.0.1"}
```

### `ts channel list`

`ts channel list --json` emits an array of channel objects:

```json
[{"client_count":1,"id":"1","is_default":true,"name":"Lobby","parent_id":null,"subscribed":true},{"client_count":2,"id":"2","is_default":false,"name":"Engineering","parent_id":null,"subscribed":true},{"client_count":1,"id":"3","is_default":false,"name":"Operations","parent_id":null,"subscribed":true},{"client_count":0,"id":"4","is_default":false,"name":"Breakout","parent_id":"2","subscribed":true}]
```

### `ts client list`

`ts client list --json` emits an array of client objects:

```json
[{"channel_id":"1","id":"1","nickname":"terminal","self":true,"talking":false,"unique_identity":"mock-local-identity"},{"channel_id":"2","id":"2","nickname":"alice","self":false,"talking":false,"unique_identity":"sdk-alice"},{"channel_id":"2","id":"3","nickname":"bob","self":false,"talking":true,"unique_identity":"sdk-bob"},{"channel_id":"3","id":"4","nickname":"ops-bot","self":false,"talking":false,"unique_identity":"sdk-ops-bot"}]
```

### `ts plugin info`

`ts plugin info --json` emits an object:

```json
{"backend":"mock","media_diagnostics":{"active_speaker_count":0,"capture":{"device":"mock-capture","is_default":true,"known":true,"mode":"mock"},"captured_voice_edit_attached":false,"consumer_connected":false,"custom_capture_device_id":"mock-capture-loop","custom_capture_device_name":"Mock Media Bridge","custom_capture_device_registered":true,"custom_capture_path_available":true,"dropped_audio_chunks":0,"dropped_playback_chunks":0,"injected_playback_attached_to_capture":false,"last_error":"","playback":{"device":"mock-playback","is_default":true,"known":true,"mode":"mock"},"playback_active":false,"pulse_sink":"","pulse_source":"","pulse_source_is_monitor":false,"queued_playback_samples":0,"transmit_path":"mock-capture-loop","transmit_path_ready":true},"media_format":"pcm_s16le @48000 Hz mono","media_socket_path":"","media_transport":"","note":"mock bridge host for local development and CI","plugin_available":true,"plugin_name":"mock-plugin-host","plugin_version":"development","socket_path":"/run/user/1000/ts3cli.sock","transport":"in-process"}
```

### `ts connect`

`ts connect --json` emits one final object. Human/table mode may print progress first; JSON mode does not.

```json
{"connected":true,"lifecycle":[{"fields":{"port":"9987","server":"127.0.0.1"},"summary":"requested new mock TeamSpeak connection","timestamp":"2026-05-01T03:13:25Z","type":"connection.requested"},{"fields":{"port":"9987","server":"127.0.0.1"},"summary":"connection is starting","timestamp":"2026-05-01T03:13:25Z","type":"connection.connecting"},{"fields":{"port":"9987","server":"127.0.0.1"},"summary":"connected to mock TeamSpeak server","timestamp":"2026-05-01T03:13:25Z","type":"connection.connected"}],"result":"connected","state":{"backend":"mock","connection":"42","identity":"mock-local-identity","mode":"one-shot","nickname":"terminal","phase":"connected","port":9987,"profile":"mock-local","server":"127.0.0.1"},"timed_out":false,"timeout_ms":15000}
```

### `ts events watch`

`ts events watch --json` emits an array of event objects after the command exits:

```json
[{"fields":{"channel_id":"2","client_count":"2"},"summary":"Engineering activity increased","timestamp":"2026-05-01T03:13:25Z","type":"channel.updated"},{"fields":{"client_id":"3","nickname":"bob"},"summary":"bob started talking","timestamp":"2026-05-01T03:13:25Z","type":"client.talking"}]
```
