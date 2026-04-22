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

## Review Follow-Ups

- keep expanding targeted regression coverage for socket lifecycle cleanup, installer receipt parsing, output encoding, and CLI override parsing
- decide whether `install-receipt.env` should stay bash `%q`-encoded or move to a simpler shared format that both shell scripts and the CLI can read directly
- generate command-reference documentation from the command router metadata to reduce drift between help text and docs
- keep tightening validation around user-supplied runtime paths such as control socket locations and managed runtime discovery roots

## Later

- TUI mode
- full-duplex realtime media bridge behavior on top of the current half-duplex V1
- push-to-talk and VAD controls
- plugin-side auth and session helpers such as identity management

## Non-Goals

- ServerQuery or WebQuery administration tooling
- reverse-engineering the retail TeamSpeak client protocol
- pretending unsupported TeamSpeak plugin APIs exist
