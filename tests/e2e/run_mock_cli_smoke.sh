#!/usr/bin/env bash
set -euo pipefail

ts_bin="${1:?usage: run_mock_cli_smoke.sh <ts-binary>}"

tmp_dir="$(mktemp -d)"
trap 'rm -rf "${tmp_dir}"' EXIT

config_path="${tmp_dir}/config.ini"
last_stdout=""
last_stderr=""

run_ts_capture() {
  local name="${1:?missing capture name}"
  shift
  last_stdout="${tmp_dir}/${name}.stdout"
  last_stderr="${tmp_dir}/${name}.stderr"
  "${ts_bin}" "$@" >"${last_stdout}" 2>"${last_stderr}"
}

run_ts_capture_allow_error() {
  local name="${1:?missing capture name}"
  shift
  last_stdout="${tmp_dir}/${name}.stdout"
  last_stderr="${tmp_dir}/${name}.stderr"
  "${ts_bin}" "$@" >"${last_stdout}" 2>"${last_stderr}"
}

assert_contains() {
  local path="${1:?missing path}"
  local pattern="${2:?missing pattern}"
  grep -q -- "${pattern}" "${path}"
}

assert_not_contains() {
  local path="${1:?missing path}"
  local pattern="${2:?missing pattern}"
  if grep -q -- "${pattern}" "${path}"; then
    printf 'unexpected pattern in %s: %s\n' "${path}" "${pattern}" >&2
    return 1
  fi
}

assert_empty() {
  local path="${1:?missing path}"
  if [[ -s "${path}" ]]; then
    printf 'expected %s to be empty, got:\n' "${path}" >&2
    sed -n '1,40p' "${path}" >&2
    return 1
  fi
}

"${ts_bin}" config init --config "${config_path}" >/dev/null
"${ts_bin}" profile use mock-local --config "${config_path}" >/dev/null

version_output="$("${ts_bin}" version)"
printf '%s\n' "${version_output}" | grep -q '^ts '

profile_output="$("${ts_bin}" profile list --config "${config_path}")"
printf '%s\n' "${profile_output}" | grep -q 'mock-local'

status_json="$("${ts_bin}" --json status --config "${config_path}")"
printf '%s\n' "${status_json}" | grep -q '"backend":"mock"'
printf '%s\n' "${status_json}" | grep -q '"phase":"connected"'

status_phase="$("${ts_bin}" --json status --field phase --config "${config_path}")"
[[ "${status_phase}" == "connected" ]]
status_phase_table_options="$("${ts_bin}" --json status --field phase --wide --no-headers --config "${config_path}")"
[[ "${status_phase_table_options}" == "connected" ]]

plugin_transmit_ready="$("${ts_bin}" --json plugin info --field media_diagnostics.transmit_path_ready --config "${config_path}")"
[[ "${plugin_transmit_ready}" == "true" || "${plugin_transmit_ready}" == "false" ]]

if run_ts_capture_allow_error missing_field --json status --field missing --config "${config_path}"; then
  printf 'expected missing field extraction to fail\n' >&2
  exit 1
fi
assert_empty "${last_stdout}"
assert_contains "${last_stderr}" '"code":"field_not_found"'

channel_json="$("${ts_bin}" --json channel list --config "${config_path}")"
printf '%s\n' "${channel_json}" | grep -q '"name":"Lobby"'
printf '%s\n' "${channel_json}" | grep -q '"name":"Engineering"'
channel_json_wide="$("${ts_bin}" --json channel list --wide --no-headers --config "${config_path}")"
[[ "${channel_json_wide}" == "${channel_json}" ]]

run_ts_capture channel_no_headers channel list --no-headers --config "${config_path}"
assert_not_contains "${last_stdout}" '^ID'
assert_contains "${last_stdout}" 'Lobby'

run_ts_capture channel_wide channel list --wide --config "${config_path}"
assert_contains "${last_stdout}" 'Subscribed'

client_json="$("${ts_bin}" --json client list --config "${config_path}")"
printf '%s\n' "${client_json}" | grep -q '"nickname":"terminal"'
run_ts_capture client_wide client list --wide --config "${config_path}"
assert_contains "${last_stdout}" 'Unique Identity'

events_json="$("${ts_bin}" --json events watch --count 2 --timeout-ms 1500 --config "${config_path}")"
printf '%s\n' "${events_json}" | grep -q '"type":"'
events_ndjson="$("${ts_bin}" events watch --output ndjson --count 2 --timeout-ms 1500 --config "${config_path}")"
[[ "$(printf '%s\n' "${events_ndjson}" | grep -c '^{"fields":')" == "2" ]]
printf '%s\n' "${events_ndjson}" | grep -q '"type":"'
printf '%s\n' "${events_ndjson}" | grep -qv '^\['

run_ts_capture connect_table --config "${config_path}" --server voice.example.com:9987 --nickname smoke-tester connect
assert_contains "${last_stdout}" 'Connected to voice.example.com:9987 as smoke-tester.'
assert_not_contains "${last_stdout}" 'Connecting to voice.example.com:9987 as smoke-tester'
assert_not_contains "${last_stdout}" 'TeamSpeak accepted the request'
assert_contains "${last_stderr}" 'Connecting to voice.example.com:9987 as smoke-tester'
assert_contains "${last_stderr}" 'TeamSpeak accepted the request'

run_ts_capture disconnect_table --config "${config_path}" disconnect
assert_contains "${last_stdout}" 'Disconnected from 127.0.0.1:9987.'
assert_not_contains "${last_stdout}" 'Requesting disconnect from the current TeamSpeak server.'
assert_contains "${last_stderr}" 'Requesting disconnect from the current TeamSpeak server.'
assert_contains "${last_stderr}" 'server connection is closed'

run_ts_capture connect_json --json --config "${config_path}" --server voice.example.com:9987 --nickname smoke-tester connect
assert_contains "${last_stdout}" '"result":"connected"'
assert_not_contains "${last_stdout}" 'Connecting to voice.example.com:9987 as smoke-tester'
assert_empty "${last_stderr}"

run_ts_capture disconnect_yaml --output yaml --config "${config_path}" disconnect
assert_contains "${last_stdout}" 'result: disconnected'
assert_not_contains "${last_stdout}" 'Requesting disconnect from the current TeamSpeak server.'
assert_empty "${last_stderr}"

update_receipt_path="${tmp_dir}/update-receipt.env"
fake_installer_path="${tmp_dir}/fake-install-release.sh"
cat >"${update_receipt_path}" <<EOF
receipt_version=1
prefix=${tmp_dir}/update-prefix
client_install_dir=${tmp_dir}/update-client
managed_dir=${tmp_dir}/update-managed
config_path=${tmp_dir}/update-config.ini
config_created_by_installer=0
release_repo=example/fork
release_tag=v0.0.1
EOF
cat >"${fake_installer_path}" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
repo=""
release_tag="vfake-latest"
prefix=""
client_dir=""
managed_dir=""
config_path=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo) repo="$2"; shift 2 ;;
    --release-tag) release_tag="$2"; shift 2 ;;
    --prefix) prefix="$2"; shift 2 ;;
    --client-dir) client_dir="$2"; shift 2 ;;
    --managed-dir) managed_dir="$2"; shift 2 ;;
    --config-path) config_path="$2"; shift 2 ;;
    *) shift ;;
  esac
done
receipt="${TS_UPDATE_RECEIPT_PATH:?}"
cat >"${receipt}" <<RECEIPT
receipt_version=1
prefix=${prefix}
client_install_dir=${client_dir}
managed_dir=${managed_dir}
config_path=${config_path}
release_repo=${repo}
release_tag=${release_tag}
RECEIPT
EOF
chmod +x "${fake_installer_path}"

TS_UPDATE_RECEIPT_PATH="${update_receipt_path}" \
  TS_UPDATE_INSTALLER_PATH="${fake_installer_path}" \
  run_ts_capture update_table update --release-tag v9.8.7
assert_contains "${last_stdout}" 'Updated teamspeak-cli to v9.8.7 from spi3/teamspeak-cli.'
assert_not_contains "${last_stdout}" 'Updating teamspeak-cli from spi3/teamspeak-cli.'
assert_contains "${last_stderr}" 'Updating teamspeak-cli from spi3/teamspeak-cli.'
assert_contains "${last_stderr}" 'Running the release installer.'

client_launcher_path="${tmp_dir}/mock-client.sh"
client_socket_path="${tmp_dir}/client-socket.txt"
cat >"${client_launcher_path}" <<EOF
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "\${TS_CONTROL_SOCKET_PATH:-}" >"${client_socket_path}"
trap 'exit 0' TERM INT
while true; do sleep 1; done
EOF
chmod +x "${client_launcher_path}"

XDG_STATE_HOME="${tmp_dir}/state" \
  HOME="${tmp_dir}/home" \
  TS_CLIENT_LAUNCHER="${client_launcher_path}" \
  TS_CLIENT_HEADLESS=0 \
  TS_CLIENT_SYSTEMD_RUN=0 \
  run_ts_capture client_start_table --config "${config_path}" client start
assert_contains "${last_stdout}" 'Started the local TeamSpeak client as PID'
assert_not_contains "${last_stdout}" 'Checking whether a local TeamSpeak client is already running.'
assert_contains "${last_stderr}" 'Checking whether a local TeamSpeak client is already running.'
assert_contains "${last_stderr}" 'The TeamSpeak client started as PID'

XDG_STATE_HOME="${tmp_dir}/state" \
  HOME="${tmp_dir}/home" \
  TS_CLIENT_LAUNCHER="${client_launcher_path}" \
  TS_CLIENT_HEADLESS=0 \
  TS_CLIENT_SYSTEMD_RUN=0 \
  run_ts_capture client_stop_table client stop
assert_contains "${last_stdout}" 'Stopped the local TeamSpeak client process rooted at PID'
assert_not_contains "${last_stdout}" 'Looking for a running local TeamSpeak client process.'
assert_contains "${last_stderr}" 'Looking for a running local TeamSpeak client process.'
assert_contains "${last_stderr}" 'Sending SIGTERM to the TeamSpeak client process group rooted at PID'
