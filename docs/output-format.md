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
- `--output yaml`: one YAML value on `stdout`.

For `json` and `yaml`, successful commands emit exactly one top-level value. The value may be an object or an array depending on the command.

Table output is for people reading a terminal. It may change labels, spacing, columns, order, wrapping, and explanatory text between releases. Do not parse table output in scripts.

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
- `--output yaml` producing one complete YAML value per command invocation
- JSON top-level type for the documented commands below, unless release notes document a compatibility change
- JSON field names that are documented or covered by tests

Scripts must not rely on:

- table output spacing, labels, row order, columns, or wording
- progress text wording or timing
- parsing progress text as data
- `json` or `yaml` output containing multiple streamed values for one command invocation
- undocumented debug fields, especially fields that appear only with `--debug`
- YAML as the primary stable scripting format when JSON can be used instead

Prefer JSON for automation.

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
