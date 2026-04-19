# Repository Guidelines

## Project Structure & Module Organization
This repository is currently minimal and only exposes the root workspace plus a `.codex` marker file. There is no established source or test layout yet. Keep new code organized from the start:

- Place application code in `src/teamspeak_cli/`.
- Place tests in `tests/`.
- Keep small fixtures or sample inputs in `tests/fixtures/`.
- Store project notes or design docs in `docs/` when needed.

Avoid mixing scripts, library code, and test data at the repository root.

## Build, Test, and Development Commands
There is no committed build toolchain in the current snapshot. When adding one, keep the command surface small and document it here and in the primary README. Prefer predictable commands such as:

- `python -m pytest` to run the full test suite
- `ruff check .` for linting
- `ruff format .` for formatting

If the project adopts another language or package manager, update this section immediately rather than leaving stale examples behind.

## Coding Style & Naming Conventions
Use 4-space indentation for Python and keep files ASCII unless a non-ASCII character is required. Follow these naming defaults:

- Modules and packages: `snake_case`
- Classes: `PascalCase`
- Functions, methods, variables: `snake_case`
- Constants: `UPPER_SNAKE_CASE`

Favor small modules, explicit imports, and short docstrings for non-obvious behavior.

## Testing Guidelines
Add tests alongside every new feature or bug fix. Name test files `test_*.py` and test functions `test_<behavior>()`. Keep fixtures local to the test module unless they are shared broadly. Aim for meaningful coverage of CLI behavior, parsing, and error paths, not just happy-path execution.

## Commit & Pull Request Guidelines
Git history is not available in this workspace snapshot, so use concise, imperative commit messages such as `Add CLI config loader` or `Fix argument parsing error`. Keep pull requests focused and include:

- A short summary of behavior changes
- Test evidence or a note explaining why tests were not run
- Linked issues or task references when applicable

Update this guide as soon as the repository gains a real toolchain or directory structure.
