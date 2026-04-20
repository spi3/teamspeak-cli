# Security Policy

## Supported Versions

Security fixes are handled on a best-effort basis for the current development branch and the latest published release line.

| Version | Supported |
| --- | --- |
| `main` | Yes |
| Latest release | Yes |
| Older releases | No |

## Reporting a Vulnerability

Please do not open a public issue or pull request with exploit details.

Use this order:

1. Use GitHub's private vulnerability reporting flow for this repository if that option is available in the repository UI.
2. If private reporting is not available, open a minimal public issue requesting a private reporting path, but do not include reproduction steps, payloads, or other sensitive details in that issue.

Include as much of the following as you can:

- affected version, branch, or commit
- affected platform and environment
- clear reproduction steps
- expected impact
- any proof-of-concept, logs, or screenshots that help confirm the issue
- whether the issue appears to involve this project, the bundled scripts, or upstream TeamSpeak components

## Scope

This policy covers vulnerabilities in this repository's code and release artifacts, including:

- the `ts` CLI
- the local socket bridge and backend glue
- installer, packaging, and wrapper scripts
- the TeamSpeak plugin code shipped from this repository

Issues in the upstream TeamSpeak client, TeamSpeak SDK, Docker base images, audio stack, or other third-party components may need to be reported to those upstream vendors as well.

## Disclosure Expectations

- keep vulnerability details private until a fix or mitigation is available
- avoid publishing exploit details in issues, pull requests, or discussions before coordinated disclosure
- after a fix lands, release notes or commit history may describe the issue at a high level

Response and remediation are best effort and may depend on whether the issue can be reproduced in the supported development paths.
