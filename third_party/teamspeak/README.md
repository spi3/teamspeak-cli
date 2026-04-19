This directory is the repo-local home for TeamSpeak-managed dependencies used by the default plugin-backed build and runtime.

`managed/` is created on demand by `make deps`, `make build`, `make test-e2e`, or `make env-up`. It is intentionally ignored by git because it contains TeamSpeak-owned SDK headers, client bundles, helper tools, and generated environment files.
