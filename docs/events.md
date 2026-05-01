# Event Catalog

## Event Surfaces

`teamspeak-cli` has two adjacent event surfaces:

- `ts events watch` reads domain events from the selected backend through the normal control path.
- `ts daemon start` polls the same domain event stream, executes matching hooks, and only journals `message.received` events for `ts message inbox`.
- The media bridge is a separate socket protocol for voice activity, audio chunks, and playback control. See [media-bridge.md](media-bridge.md).

This catalog covers the domain events returned by `ts events watch` and delivered to daemon hooks.

`ts events watch --json` returns a JSON array after the command exits. `ts events watch --output ndjson` returns the same event shape as newline-delimited JSON, one event object per line. The current watch API collects the requested batch first, then writes and flushes each returned NDJSON line.

## Event Envelope

JSON output renders every domain event as:

```json
{
  "type": "message.received",
  "summary": "received TeamSpeak text message",
  "timestamp": "2026-04-24 00:00:00",
  "fields": {
    "text": "hello"
  }
}
```

NDJSON output renders each event as the same object on its own line, without a surrounding array.

All `fields` values are strings. Field sets can differ by backend because the mock backend is a deterministic development harness while the plugin backend reflects TeamSpeak callback data.

## Availability Summary

| Event type | Plugin backend | Mock backend | Daemon hookable | Inbox journaled | `--message-kind` |
| --- | --- | --- | --- | --- | --- |
| `connection.requested` | yes | yes | yes | no | no |
| `connection.connecting` | yes | yes | yes | no | no |
| `connection.connected` | yes | yes | yes | no | no |
| `connection.disconnected` | yes | yes | yes | no | no |
| `connection.error` | yes | no | yes | no | no |
| `connection.status` | yes | no | yes | no | no |
| `message.received` | yes | yes | yes | yes | yes |
| `client.talking` | yes | yes | yes | no | no |
| `client.moved` | yes | no | yes | no | no |
| `server.error` | yes | no | yes | no | no |
| `media.playback.error` | yes | no | yes | no | no |
| `channel.joined` | no | yes | yes | no | no |
| `channel.updated` | no | yes | yes | no | no |
| `client.self_muted` | no | yes | yes | no | no |
| `client.self_away` | no | yes | yes | no | no |
| `message.sent` | no | yes | yes | no | no |
| `heartbeat` | no | yes | yes | no | no |

`Daemon hookable` means the daemon can match the event with `ts events hook add --type ...` or `--type '*'`. It does not mean the event is persisted. The current inbox journal is intentionally message-only.

`--message-kind` is useful for `message.received` hooks. The daemon derives `message_kind` before hook matching and inbox journaling when possible:

| Source payload | Derived `message_kind` |
| --- | --- |
| `target_kind` | same value |
| `channel_id` | `channel` |
| plugin `target_mode=1` | `client` |
| plugin `target_mode=2` | `channel` |
| plugin `target_mode=3` | `server` |

## Plugin-Backed Events

These events come from the TeamSpeak client plugin host backend.

| Event type | Meaning | Fields |
| --- | --- | --- |
| `connection.requested` | The CLI asked TeamSpeak to start a connection. | `server`, `port` |
| `connection.connecting` | TeamSpeak reported a connecting status. | `handler` |
| `connection.connected` | TeamSpeak reported the connection as establishing, established, or connected. | `handler` |
| `connection.disconnected` | TeamSpeak reported the connection as closed. | `handler` |
| `connection.error` | TeamSpeak reported an error number during a connection status callback. | `handler`, `error` |
| `connection.status` | TeamSpeak reported another connection status value. | `handler`, `status` |
| `message.received` | TeamSpeak delivered a text message callback. | `handler`, `target_mode`, `to_id`, `from_id`, `from_name`, `from_unique_identifier`, `text` |
| `client.talking` | TeamSpeak reported a client talk status change. | `handler`, `client_id`, `status` |
| `client.moved` | TeamSpeak reported a client channel move. | `handler`, `client_id`, `old_channel_id`, `new_channel_id`, `message` |
| `server.error` | TeamSpeak reported a server error callback. | `handler`, `error`, `return_code`, `extra_message` |
| `media.playback.error` | The plugin failed to submit media bridge playback audio into the TeamSpeak custom capture device. | `handler`, `error` |

The plugin also publishes media socket frames such as `speaker.start`, `audio.chunk`, `speaker.stop`, `playback.started`, `playback.stopped`, and `playback.cleared`. Those frames are not domain events and are not returned by `ts events watch`.

## Mock-Backed Events

These events come from the deterministic mock backend used by `mock-local`, tests, and local development.

| Event type | Meaning | Fields |
| --- | --- | --- |
| `connection.requested` | The mock backend accepted a connection request. | `server`, `port` |
| `connection.connecting` | The mock backend simulated connection startup. | `server`, `port` |
| `connection.connected` | The mock backend simulated a connected session. | `server`, `port` |
| `connection.disconnected` | The mock backend disconnected the simulated session. | none |
| `channel.joined` | The local mock user joined a mock channel. | `channel_id`, `channel_name` |
| `channel.updated` | The mock background event loop reported channel activity. | `channel_id`, `client_count` |
| `client.talking` | The mock background event loop reported a talking client. | `client_id`, `nickname` |
| `client.self_muted` | The mock local user mute state changed. | `muted` |
| `client.self_away` | The mock local user away state changed. | `away`, `message` |
| `message.sent` | The mock backend accepted an outgoing message command. | `target_kind`, `target`, `text` |
| `message.received` | The mock background event loop reported an incoming channel message. | `from`, `channel_id`, `text` |
| `heartbeat` | The mock background event loop emitted a keepalive-style test event. | `backend` |

Mock-only events are part of the development and test surface. They should not be treated as current plugin-backed TeamSpeak callback coverage.

## Hook And Inbox Behavior

Daemon hooks are generic. A hook with `--type client.talking` can match `client.talking`, and a hook with `--type '*'` can match any event the selected backend emits.

The daemon only appends `message.received` events to the message inbox. `ts message inbox` therefore does not show connection, client, channel, heartbeat, server error, or media playback error events even though hooks can still match them.

Hook commands receive the event JSON on `stdin`. The daemon also sets:

- `TS_HOOK_ID`
- `TS_EVENT_TYPE`
- `TS_EVENT_SUMMARY`
- `TS_EVENT_TIMESTAMP`
- `TS_MESSAGE_KIND`, when available
- `TS_MESSAGE_FROM`, when available
- `TS_MESSAGE_TEXT`, when available
