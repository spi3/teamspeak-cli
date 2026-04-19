#!/usr/bin/env bash
set -euo pipefail

ts_bin="${1:?usage: run_mock_bridge_e2e.sh <ts-binary> <mock-bridge-host-binary>}"
host_bin="${2:?usage: run_mock_bridge_e2e.sh <ts-binary> <mock-bridge-host-binary>}"

tmp_dir="$(mktemp -d)"
cleanup() {
  if [[ -n "${host_pid:-}" ]]; then
    kill "${host_pid}" 2>/dev/null || true
    wait "${host_pid}" 2>/dev/null || true
  fi
  rm -rf "${tmp_dir}"
}
trap cleanup EXIT

socket_path="${tmp_dir}/plugin.sock"
config_path="${tmp_dir}/config.ini"

export TS_CONTROL_SOCKET_PATH="${socket_path}"

"${host_bin}" "${socket_path}" >"${tmp_dir}/host.log" 2>&1 &
host_pid=$!

for _ in $(seq 1 50); do
  if [[ -S "${socket_path}" ]]; then
    break
  fi
  sleep 0.1
done

if [[ ! -S "${socket_path}" ]]; then
  echo "mock bridge host did not create control socket" >&2
  exit 1
fi

"${ts_bin}" config init --config "${config_path}" >/dev/null
"${ts_bin}" profile use plugin-local --config "${config_path}" >/dev/null

plugin_info_json="$("${ts_bin}" --json plugin info --config "${config_path}")"
printf '%s\n' "${plugin_info_json}" | grep -q '"plugin_available":true'

status_json="$("${ts_bin}" --json status --config "${config_path}")"
printf '%s\n' "${status_json}" | grep -q '"phase":"connected"'

channel_json="$("${ts_bin}" --json channel list --config "${config_path}")"
printf '%s\n' "${channel_json}" | grep -q '"name":"Lobby"'

client_json="$("${ts_bin}" --json client list --config "${config_path}")"
printf '%s\n' "${client_json}" | grep -q '"nickname":"terminal"'

events_json="$("${ts_bin}" --json events watch --count 2 --timeout-ms 500 --config "${config_path}")"
printf '%s\n' "${events_json}" | grep -q '"type":"'
