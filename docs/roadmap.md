# Roadmap

## Near Term

- finish build verification of the optional `ts3cli_plugin` target against the TeamSpeak 3 Client Plugin SDK
- validate real plugin runtime behavior inside a local TeamSpeak client install
- improve selector behavior for channel and client lookup
- tighten plugin-side error reporting for asynchronous TeamSpeak operations

## Next

- persistent background session mode
- interactive shell on top of the same backend/session seam
- richer JSON views for scripting
- more callback coverage for channel, client, and text-message events

## Later

- TUI mode
- voice capture/playback controls
- push-to-talk and VAD controls
- plugin-side auth/session helpers such as identity management

## Non-Goals

- ServerQuery or WebQuery administration tooling
- reverse-engineering the retail TeamSpeak client protocol
- pretending unsupported TeamSpeak plugin APIs exist
