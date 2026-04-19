# Roadmap

## Near Term

- stabilize the TeamSpeak-backed Docker and `Xvfb` harness against first-run dialogs and other host-sensitive client behavior
- improve troubleshooting and failure visibility for the TeamSpeak-backed runtime path
- improve selector behavior for channel and client lookup
- tighten plugin-side error reporting for asynchronous TeamSpeak operations

## Next

- persistent background session mode
- interactive shell on top of the same backend and session seam
- richer JSON views for scripting
- more callback coverage for channel, client, and text-message events

## Later

- TUI mode
- voice capture and playback controls
- push-to-talk and VAD controls
- plugin-side auth and session helpers such as identity management

## Non-Goals

- ServerQuery or WebQuery administration tooling
- reverse-engineering the retail TeamSpeak client protocol
- pretending unsupported TeamSpeak plugin APIs exist
