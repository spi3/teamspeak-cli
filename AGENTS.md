# Repository Guidelines

## Project Structure & Module Organization

This is a C++20 CMake project with a CLI, a TeamSpeak plugin, shell tooling, and both offline and TeamSpeak-backed test paths. Keep code and docs in the established layout:

- `src/teamspeak_cli/`: application, bridge, backend, and plugin code
- `tests/`: C++ test binaries and end-to-end shell checks
- `tests/e2e/`: TeamSpeak-backed and built-test harness scripts
- `tests/fixtures/`: small test inputs
- `docs/`: user and developer documentation
- `scripts/`: install and uninstall entry points

Avoid adding new top-level scripts or loose source files when an existing directory already fits.

## Build, Test, and Development Commands

Prefer the top-level `Makefile` unless you specifically need raw CMake control:

- `make help` to list the supported workflows
- `make build-built-test` to build the offline development path
- `make test-built-test` to run the offline suite
- `make build` to build the TeamSpeak-backed tree and bootstrap managed dependencies
- `make test` to run the default automated suite without the Docker and `Xvfb` E2E case
- `make test-e2e` to run the TeamSpeak-backed local integration harness
- `./scripts/install.sh` for a user-level install
- `./scripts/uninstall.sh` or `ts-uninstall` to remove a user-level install

If you use raw CMake, remember that plugin builds are opt-in with `-DTS_ENABLE_TS3_PLUGIN=ON`.

## Coding Style & Naming Conventions

Use 4-space indentation and keep files ASCII unless a non-ASCII character is required. Follow these naming defaults:

- directories, files, functions, methods, variables: `snake_case`
- classes, structs, enums: `PascalCase`
- constants: `UPPER_SNAKE_CASE`

Use the repository `.clang-format` for C++ formatting. Keep TeamSpeak-specific details behind backend, bridge, and plugin boundaries instead of mixing them into command parsing or rendering code.

## Testing Guidelines

Add or update tests with every feature or bug fix. The main test surfaces are:

- focused C++ tests in `tests/*_test.cpp`
- built-test CLI and bridge end-to-end checks
- local-only TeamSpeak-backed shell harnesses in `tests/e2e/`

Prefer the built-test path for most new coverage because it is deterministic and exercised in automation. Use the TeamSpeak-backed harness when the change genuinely depends on the real client plugin runtime.

## Commit & Pull Request Guidelines

Use concise, imperative commit messages such as `Add socket bridge error hint` or `Document TeamSpeak-backed build path`. Keep pull requests focused and include:

- a short summary of behavior changes
- test evidence or a note explaining why tests were not run
- linked issues or task references when applicable

Update this guide whenever the build surface, runtime assumptions, or directory structure change.
