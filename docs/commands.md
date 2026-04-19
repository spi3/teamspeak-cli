# Commands

## Global Options

- `--output table|json|yaml`
- `--json`
- `--profile <name>`
- `--server <host[:port]>`
- `--nickname <name>`
- `--identity <string>`
- `--config <path>`
- `--verbose`
- `--debug`

## Core

- `ts version`
- `ts plugin info`
- `ts status`
- `ts connect`
- `ts disconnect`

`ts sdk info` is still accepted as a compatibility alias for `ts plugin info`.

## Config

- `ts config init`
- `ts config view`
- `ts profile list`
- `ts profile use <name>`

## Server, Channel, and Client

- `ts server info`
- `ts channel list`
- `ts channel get <id-or-name>`
- `ts channel join <id-or-name>`
- `ts client list`
- `ts client get <id-or-name>`

## Messaging and Events

- `ts message send --target <client|channel> --id <id-or-name> --text "<message>"`
- `ts events watch [--count N] [--timeout-ms N]`

## Shell Integration

- `ts completion bash`
- `ts completion zsh`
- `ts completion fish`
- `ts completion powershell`

## Behavioral Notes

- Running a grouped command like `ts channel`, `ts config`, or `ts message` without a nested subcommand now prints contextual help for that command group.
- `ts <command> --help` prints command-specific usage and available subcommands.
- `status`, `server info`, `channel list`, and `client list` inspect the current backend session state. They do not auto-connect around each command anymore.
- `connect` asks the backend to open a connection, waits up to 15 seconds for it to finish, and streams human-readable progress as TeamSpeak reports it.
- `disconnect` asks the backend to close the current connection, waits up to 10 seconds for it to finish, and streams human-readable progress as TeamSpeak reports it.
- `client start` and `client stop` stream human-readable progress while they inspect, launch, and stop the local TeamSpeak client process.
- `connect --json` and `connect --output yaml` still return one structured result at the end instead of streaming prose.
- `disconnect --json`, `client start --json`, and `client stop --json` also return one structured result at the end instead of streaming prose.
- `disconnect` asks the backend to close the current connection.
- the default profile is `plugin-local`
- the `built-test` profile is the offline fallback
- the plugin backend uses a local control socket and expects the TeamSpeak client plugin to already be running
