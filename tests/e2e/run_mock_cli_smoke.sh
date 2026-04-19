#!/usr/bin/env bash
set -euo pipefail

ts_bin="${1:?usage: run_mock_cli_smoke.sh <ts-binary>}"

tmp_dir="$(mktemp -d)"
trap 'rm -rf "${tmp_dir}"' EXIT

config_path="${tmp_dir}/config.ini"

"${ts_bin}" config init --config "${config_path}" >/dev/null
"${ts_bin}" profile use mock-local --config "${config_path}" >/dev/null

version_output="$("${ts_bin}" version)"
printf '%s\n' "${version_output}" | grep -q '^ts '

profile_output="$("${ts_bin}" profile list --config "${config_path}")"
printf '%s\n' "${profile_output}" | grep -q 'mock-local'

status_json="$("${ts_bin}" --json status --config "${config_path}")"
printf '%s\n' "${status_json}" | grep -q '"backend":"mock"'
printf '%s\n' "${status_json}" | grep -q '"phase":"connected"'

channel_json="$("${ts_bin}" --json channel list --config "${config_path}")"
printf '%s\n' "${channel_json}" | grep -q '"name":"Lobby"'
printf '%s\n' "${channel_json}" | grep -q '"name":"Engineering"'

client_json="$("${ts_bin}" --json client list --config "${config_path}")"
printf '%s\n' "${client_json}" | grep -q '"nickname":"terminal"'

events_json="$("${ts_bin}" --json events watch --count 2 --timeout-ms 1500 --config "${config_path}")"
printf '%s\n' "${events_json}" | grep -q '"type":"'
