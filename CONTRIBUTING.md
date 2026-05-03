# Contributing

Thanks for contributing to `teamspeak-cli`.

This project is a C++20 CLI plus a TeamSpeak 3 client plugin bridge, with both a fully local mock path and a TeamSpeak-backed integration path. Most changes should be developed and tested through the mock path first, then exercised against the TeamSpeak-backed path only when the change depends on the real client runtime.

## Before You Start

- read [README.md](README.md) for the supported workflows and runtime assumptions
- keep changes scoped; avoid mixing feature work, refactors, and unrelated cleanup in one pull request
- update docs when behavior, flags, install flow, or runtime expectations change
- add or update tests with every feature or bug fix

## Project Layout

- `src/teamspeak_cli/`: CLI, config, backend, bridge, session, and plugin code
- `tests/`: focused C++ tests and end-to-end shell checks
- `tests/e2e/`: TeamSpeak-backed and mock harness scripts
- `tests/fixtures/`: small test inputs
- `docs/`: user and developer documentation
- `scripts/`: install, uninstall, packaging, and helper entry points

Avoid adding new top-level scripts or loose source files when an existing directory already fits.

## Development Workflow

Prefer the top-level `Makefile` unless you specifically need raw CMake control.

Common commands:

- `make help`: list the supported top-level workflows
- `make build-mock`: build the offline development tree
- `make test-mock`: run the offline suite
- `make build`: build the TeamSpeak-backed tree and bootstrap managed dependencies
- `make test`: run the default automated suite without the Docker/`Xvfb` E2E case
- `make test-e2e`: run the TeamSpeak-backed local integration harness

If you use raw CMake, remember that plugin builds are opt-in with `-DTS_ENABLE_TS3_PLUGIN=ON`.

## Branch Naming

Create branches from the latest main branch and keep each branch focused on one change.
Use lowercase, hyphen-separated names in this format:

```text
<type>/<short-description>
```

Standard branch types:

- `feature/`: new user-facing behavior or capabilities
- `fix/`: bug fixes
- `docs/`: documentation-only changes
- `test/`: test-only changes
- `refactor/`: internal code changes without intended behavior changes
- `chore/`: maintenance, dependency, or tooling updates

Examples:

- `feature/socket-bridge-retry`
- `fix/config-path-resolution`
- `docs/install-troubleshooting`

Branch process:

- start from the current main branch before beginning work
- keep unrelated fixes, cleanup, and experiments on separate branches
- rebase or merge main before opening a pull request when your branch is stale
- delete merged branches after the pull request lands

## Testing Expectations

Choose the narrowest test surface that proves the change.

- For config, output, command routing, socket protocol, and session changes, prefer the mock path.
- For TeamSpeak plugin loading, runtime launch behavior, or client-plugin bridge behavior, use the TeamSpeak-backed harness.
- When changing installer or shell logic, validate the affected scripts directly and keep shell syntax clean.

Main test surfaces:

- focused C++ tests in `tests/*_test.cpp`
- mock CLI and bridge end-to-end checks
- local-only TeamSpeak-backed shell harnesses in `tests/e2e/`

If you cannot run the TeamSpeak-backed path locally, say so in the pull request and include the mock-path evidence you did run.

## Coding Style

- use 4-space indentation
- keep files ASCII unless a non-ASCII character is required
- format C++ with the repository `.clang-format`
- use `snake_case` for directories, files, functions, methods, and variables
- use `PascalCase` for classes, structs, and enums
- use `UPPER_SNAKE_CASE` for constants

Keep TeamSpeak-specific details behind backend, bridge, and plugin boundaries instead of mixing them into command parsing or rendering code.

## Design Guidelines

- keep CLI parsing and command dispatch in `src/teamspeak_cli/cli/`
- prefer extending the backend seam over leaking TeamSpeak callback details into the CLI
- keep domain models and renderers TeamSpeak-agnostic where possible
- prefer small, targeted patches over broad rewrites

## Pull Requests

Use concise, imperative commit messages such as `Add socket bridge error hint` or `Document TeamSpeak-backed build path`.

Pull requests should include:

- a short summary of behavior changes
- test evidence, or a note explaining why tests were not run
- linked issues or task references when applicable

When the build surface, runtime assumptions, or directory structure change, update [README.md](README.md) and the relevant docs under [docs/](docs/).
