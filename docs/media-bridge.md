# Media Bridge

## Overview

The TeamSpeak plugin now exposes a dedicated local media socket alongside the existing control socket.

The media bridge is the V1 half-duplex path for:

- inbound per-speaker TeamSpeak voice chunks
- speaker start and stop events
- assistant playback injection back into TeamSpeak
- immediate playback clear and drain-stop semantics
- lightweight queue and drop diagnostics

V1 intentionally supports one active consumer connection at a time.

## Socket Discovery

The media socket path resolves in this order:

- `TS_MEDIA_SOCKET_PATH`, if set
- otherwise a path derived from the control socket by replacing `.sock` with `-media.sock`
- if the control socket does not end in `.sock`, `.media` is appended instead

`ts plugin info` reports both the control socket path and the media socket path for the active plugin backend.

## Framing

Each frame starts with one tab-separated header line terminated by `\n`.

- field `0` is always `tsmedia1`
- field `1` is the frame type
- string payload fields are hex-encoded with the same rules as the control bridge
- `audio.chunk` and `playback.chunk` carry raw PCM bytes immediately after the header line
- there is no delimiter after the binary payload; the next frame begins immediately after the payload bytes

All timestamps are milliseconds since the Unix epoch.

## Accepted Playback Format

V1 playback injection accepts exactly:

- format: `pcm_s16le`
- sample rate: `48000`
- channels: `1`

`audio.chunk` ingress frames use the callback-native channel count from the TeamSpeak client callback. The current mock and TeamSpeak-backed V1 path both emit mono ingress chunks.

## Server Frames

### `hello`

Fields:

- `0`: `tsmedia1`
- `1`: `hello`
- `2`: timestamp
- `3`: media format description, hex-encoded
- `4`: playback sample rate
- `5`: playback channel count
- `6`: socket path, hex-encoded
- `7`: max consumer count

### `status`

Fields:

- `0`: `tsmedia1`
- `1`: `status`
- `2`: timestamp
- `3`: consumer connected (`1` or `0`)
- `4`: playback active (`1` or `0`)
- `5`: queued playback samples
- `6`: active speaker count
- `7`: dropped ingress audio chunks
- `8`: dropped playback chunks
- `9`: last error message, hex-encoded

### `speaker.start`

### `speaker.stop`

Fields:

- `0`: `tsmedia1`
- `1`: frame type
- `2`: timestamp
- `3`: TeamSpeak handler id
- `4`: TeamSpeak client id
- `5`: speaker unique identity, hex-encoded
- `6`: speaker nickname, hex-encoded
- `7`: channel id or `0`

These frames are the segment boundaries for utterance detection in V1.

### `audio.chunk`

Fields:

- `0`: `tsmedia1`
- `1`: `audio.chunk`
- `2`: timestamp
- `3`: TeamSpeak handler id
- `4`: TeamSpeak client id
- `5`: speaker unique identity, hex-encoded
- `6`: speaker nickname, hex-encoded
- `7`: channel id or `0`
- `8`: sample rate
- `9`: channel count
- `10`: frame count per channel
- `11`: payload byte length

Payload:

- signed 16-bit little-endian PCM
- interleaved by channel when `channel count > 1`

### `playback.started`

Sent after a valid `playback.start`.

### `playback.stopped`

Fields:

- `0`: `tsmedia1`
- `1`: `playback.stopped`
- `2`: timestamp
- `3`: stop reason, hex-encoded

The current stop reason is `drained` once queued playback has been fully consumed.

### `playback.cleared`

Sent after `playback.clear` drops the queued playback immediately.

### `error`

Fields:

- `0`: `tsmedia1`
- `1`: `error`
- `2`: timestamp
- `3`: error code, hex-encoded
- `4`: error message, hex-encoded

## Client Frames

### `status.request`

Header only:

```text
tsmedia1    status.request
```

### `playback.start`

Fields:

- `0`: `tsmedia1`
- `1`: `playback.start`
- `2`: playback format, hex-encoded
- `3`: playback sample rate
- `4`: playback channel count

### `playback.chunk`

Fields:

- `0`: `tsmedia1`
- `1`: `playback.chunk`
- `2`: frame count
- `3`: payload byte length

Payload:

- raw `pcm_s16le` mono samples matching the active playback session format

### `playback.stop`

Marks the current playback session for drain-stop. The queue continues to play until empty, then the server emits
`playback.stopped`.

### `playback.clear`

Drops all queued playback immediately and emits `playback.cleared`.

## V1 Semantics

- The bridge is half-duplex. Playback injection is designed to override outgoing capture audio while active.
- `playback.start` must be sent before `playback.chunk`.
- `playback.stop` drains queued samples before stopping.
- `playback.clear` is the interruption path. It empties the queue immediately.
- The mock backend exercises the same socket contract so playback queueing and interruption can be tested without a live TeamSpeak client.
