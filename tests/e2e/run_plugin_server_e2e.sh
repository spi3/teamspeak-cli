#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/e2e/runtime_common.sh
source "${script_dir}/runtime_common.sh"

ts_bin="${1:?usage: run_plugin_server_e2e.sh <ts-binary> <plugin-shared-library>}"
plugin_so="${2:?usage: run_plugin_server_e2e.sh <ts-binary> <plugin-shared-library>}"

docker_image="${TS3_DOCKER_IMAGE:-teamspeak:latest}"
server_port="${TS3_SERVER_PORT:-$((19000 + ($$ % 1000)))}"
query_port="${TS3_QUERY_PORT:-$((21000 + ($$ % 1000)))}"

if [[ ! -x "${ts_bin}" ]]; then
  echo "ts binary not found or not executable: ${ts_bin}" >&2
  exit 1
fi

if [[ ! -f "${plugin_so}" ]]; then
  echo "plugin shared library not found: ${plugin_so}" >&2
  exit 1
fi

ts3_runtime_require_command Xvfb "Xvfb is required for the TeamSpeak-backed E2E test"
ts3_runtime_require_docker_access "the TeamSpeak-backed E2E test"
client_source_dir="$(ts3_runtime_resolve_client_source_dir)"
ts3_runtime_resolve_client_runtime_library_path "${client_source_dir}"
ts3_runtime_resolve_xdotool

tmp_dir="$(mktemp -d)"
client_runtime_dir="${tmp_dir}/client"
home_dir="${tmp_dir}/home"
config_path="${tmp_dir}/config.ini"
socket_path="${tmp_dir}/ts3cli.sock"
client_log="${tmp_dir}/client.log"
xvfb_log="${tmp_dir}/xvfb.log"
container_name="ts3cli-e2e-${RANDOM}-${RANDOM}"
volume_name="ts3cli-e2e-${RANDOM}-${RANDOM}"

status=0
client_pid=""
xvfb_pid=""

run_xdotool() {
  local -a cmd_prefix=()
  if [[ -n "${display:-}" ]]; then
    cmd_prefix=(env "DISPLAY=${display}")
  fi
  if [[ -n "${xdotool_library_path}" ]]; then
    "${cmd_prefix[@]}" LD_LIBRARY_PATH="${xdotool_library_path}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" "${xdotool_bin}" "$@"
    return
  fi
  "${cmd_prefix[@]}" "${xdotool_bin}" "$@"
}

find_window_id() {
  local title="$1"
  run_xdotool search --name "${title}" 2>/dev/null | head -n 1 || true
}

dump_client_logs() {
  if [[ -f "${client_log}" ]]; then
    echo "--- client stdout/stderr ---" >&2
    tail -n 200 "${client_log}" >&2 || true
  fi
  if [[ -d "${home_dir}/.ts3client/logs" ]]; then
    while IFS= read -r log_file; do
      echo "--- ${log_file} ---" >&2
      tail -n 80 "${log_file}" >&2 || true
    done < <(find "${home_dir}/.ts3client/logs" -type f | sort | tail -n 3)
  fi
}

dump_diagnostics() {
  echo "--- TeamSpeak-backed E2E diagnostics ---" >&2
  echo "temp dir: ${tmp_dir}" >&2
  echo "display: ${display:-unset}" >&2
  echo "socket: ${socket_path}" >&2
  echo "server port: ${server_port}" >&2
  echo "query port: ${query_port}" >&2
  echo "--- docker logs ---" >&2
  docker logs "${container_name}" 2>&1 | tail -n 200 >&2 || true
  echo "--- xdotool visible windows ---" >&2
  run_xdotool search --onlyvisible --name "." 2>/dev/null | while read -r wid; do
    run_xdotool getwindowname "${wid}" 2>/dev/null || true
  done >&2 || true
  if [[ -f "${config_path}" ]]; then
    echo "--- plugin info ---" >&2
    TS_CONTROL_SOCKET_PATH="${socket_path}" "${ts_bin}" --debug --json --config "${config_path}" plugin info >&2 || true
    echo "--- status ---" >&2
    TS_CONTROL_SOCKET_PATH="${socket_path}" "${ts_bin}" --debug --json --config "${config_path}" status >&2 || true
  fi
  dump_client_logs
  if [[ -f "${xvfb_log}" ]]; then
    echo "--- xvfb log ---" >&2
    tail -n 100 "${xvfb_log}" >&2 || true
  fi
}

cleanup() {
  status=$?
  if [[ ${status} -ne 0 ]]; then
    dump_diagnostics
  fi
  if [[ -n "${client_pid}" ]]; then
    kill "${client_pid}" 2>/dev/null || true
    wait "${client_pid}" 2>/dev/null || true
  fi
  if [[ -n "${xvfb_pid}" ]]; then
    kill "${xvfb_pid}" 2>/dev/null || true
    wait "${xvfb_pid}" 2>/dev/null || true
  fi
  docker rm -f "${container_name}" >/dev/null 2>&1 || true
  docker volume rm -f "${volume_name}" >/dev/null 2>&1 || true
  if [[ ${status} -eq 0 ]]; then
    rm -rf "${tmp_dir}"
  else
    echo "TeamSpeak-backed E2E artifacts preserved at ${tmp_dir}" >&2
  fi
  exit "${status}"
}
trap cleanup EXIT

wait_for_socket() {
  for _ in $(seq 1 300); do
    if [[ -S "${socket_path}" ]]; then
      return 0
    fi
    sleep 0.2
  done
  echo "TeamSpeak plugin control socket did not appear at ${socket_path}" >&2
  return 1
}

wait_for_server() {
  for _ in $(seq 1 120); do
    logs="$(docker logs "${container_name}" 2>&1 || true)"
    if printf '%s\n' "${logs}" | grep -Eq 'listening on 0\.0\.0\.0:9987|ServerQuery listening on 0\.0\.0\.0:10011|listening for query on 0\.0\.0\.0:10011'; then
      return 0
    fi
    sleep 1
  done
  echo "TeamSpeak server container did not become ready" >&2
  return 1
}

choose_display() {
  for candidate in $(seq 130 170); do
    if [[ ! -e "/tmp/.X11-unix/X${candidate}" ]]; then
      printf ':%s' "${candidate}"
      return 0
    fi
  done
  return 1
}

client_running() {
  [[ -n "${client_pid}" ]] && kill -0 "${client_pid}" 2>/dev/null
}

start_client() {
  rm -f "${socket_path}"
  (
    cd "${client_runtime_dir}"
    export HOME="${home_dir}"
    export DISPLAY="${display}"
    export TS_CONTROL_SOCKET_PATH="${socket_path}"
    if [[ -n "${client_runtime_library_path}" ]]; then
      export LD_LIBRARY_PATH="${client_runtime_library_path}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
    fi
    exec ./ts3client_runscript.sh -nosingleinstance
  ) >"${client_log}" 2>&1 &
  client_pid=$!
}

ensure_client_ready() {
  if [[ -S "${socket_path}" ]] && client_running; then
    return 0
  fi
  if client_running; then
    kill "${client_pid}" 2>/dev/null || true
    wait "${client_pid}" 2>/dev/null || true
  fi
  start_client
  wait_for_socket
  dismiss_onboarding_dialogs
}

accept_license_window() {
  local wid="$1"
  local geometry
  geometry="$(run_xdotool getwindowgeometry --shell "${wid}")"
  eval "${geometry}"
  local click_x=573
  local click_y=676
  if [[ "${WIDTH}" -ne 740 || "${HEIGHT}" -ne 700 ]]; then
    click_x=$((WIDTH * 77 / 100))
    click_y=$((HEIGHT * 97 / 100))
  fi
  run_xdotool key --window "${wid}" End || true
  sleep 0.3
  run_xdotool mousemove --window "${wid}" "${click_x}" "${click_y}" click 1
}

dismiss_onboarding_dialogs() {
  local progress
  local wid
  for _ in $(seq 1 50); do
    progress=0

    wid="$(find_window_id 'License agreement')"
    if [[ -n "${wid}" ]]; then
      accept_license_window "${wid}"
      progress=1
      sleep 0.7
    fi

    for title in \
      'Introducing the next generation of TeamSpeak' \
      'myTeamSpeak Account' \
      'Warning' \
      'Identities' \
      'Choose Your Nickname'; do
      wid="$(find_window_id "${title}")"
      if [[ -n "${wid}" ]]; then
        run_xdotool key --window "${wid}" Escape || true
        progress=1
        sleep 0.4
      fi
    done

    if [[ "${progress}" -eq 0 ]]; then
      break
    fi
  done
}

wait_for_connected_status() {
  local max_checks="${1:-120}"
  local last_status=""
  for _ in $(seq 1 "${max_checks}"); do
    ensure_client_ready
    dismiss_onboarding_dialogs
    if ! last_status="$(TS_CONTROL_SOCKET_PATH="${socket_path}" "${ts_bin}" --json --config "${config_path}" status 2>/dev/null)"; then
      sleep 1
      continue
    fi
    if ts3_runtime_json_expect_fragment "${last_status}" '"phase":"connected"' "connection phase not connected" 2>/dev/null; then
      printf '%s\n' "${last_status}"
      return 0
    fi
    sleep 1
  done
  printf '%s\n' "${last_status}"
  return 1
}

wait_for_disconnected_status() {
  local last_status=""
  for _ in $(seq 1 60); do
    if ! last_status="$(TS_CONTROL_SOCKET_PATH="${socket_path}" "${ts_bin}" --json --config "${config_path}" status 2>/dev/null)"; then
      sleep 1
      continue
    fi
    if ts3_runtime_json_expect_fragment "${last_status}" '"phase":"disconnected"' "connection phase not disconnected" 2>/dev/null; then
      printf '%s\n' "${last_status}"
      return 0
    fi
    sleep 1
  done
  printf '%s\n' "${last_status}"
  return 1
}

cp -R "${client_source_dir}/." "${client_runtime_dir}/"
mkdir -p "${client_runtime_dir}/plugins" "${home_dir}"
cp "${plugin_so}" "${client_runtime_dir}/plugins/ts3cli_plugin.so"

docker volume create "${volume_name}" >/dev/null
docker run \
  --name "${container_name}" \
  --detach \
  --publish "127.0.0.1:${server_port}:9987/udp" \
  --publish "127.0.0.1:${query_port}:10011" \
  --env TS3SERVER_LICENSE=accept \
  --volume "${volume_name}:/var/ts3server/" \
  "${docker_image}" >/dev/null

wait_for_server

display="$(choose_display)"
if [[ -z "${display}" ]]; then
  echo "failed to find a free Xvfb display" >&2
  exit 1
fi

Xvfb "${display}" -screen 0 1280x1024x24 -ac >"${xvfb_log}" 2>&1 &
xvfb_pid=$!
sleep 1

start_client
wait_for_socket
dismiss_onboarding_dialogs

TS_CONTROL_SOCKET_PATH="${socket_path}" "${ts_bin}" config init --config "${config_path}" >/dev/null
TS_CONTROL_SOCKET_PATH="${socket_path}" "${ts_bin}" profile use plugin-local --config "${config_path}" >/dev/null

plugin_info_json="$(TS_CONTROL_SOCKET_PATH="${socket_path}" "${ts_bin}" --json --config "${config_path}" plugin info)"
ts3_runtime_json_expect_fragment "${plugin_info_json}" '"plugin_available":true' "plugin backend was not available"
ts3_runtime_json_expect_fragment "${plugin_info_json}" '"backend":"plugin"' "plugin backend did not report plugin mode"

connect_ok=0
status_json=""
for _ in $(seq 1 8); do
  ensure_client_ready
  dismiss_onboarding_dialogs
  if TS_CONTROL_SOCKET_PATH="${socket_path}" \
      "${ts_bin}" \
      --json \
      --config "${config_path}" \
      --server "127.0.0.1:${server_port}" \
      --nickname "cli-e2e" \
      connect >/dev/null 2>"${tmp_dir}/connect.err"; then
    if status_json="$(wait_for_connected_status 20)"; then
      connect_ok=1
      break
    fi
    sleep 1
    continue
  fi
  if grep -qi "currently not possible" "${tmp_dir}/connect.err"; then
    sleep 1
    continue
  fi
  cat "${tmp_dir}/connect.err" >&2
  exit 1
done

if [[ "${connect_ok}" -ne 1 ]]; then
  echo "failed to request TeamSpeak connection through the plugin backend" >&2
  cat "${tmp_dir}/connect.err" >&2 || true
  exit 1
fi
ts3_runtime_json_expect_fragment "${status_json}" '"phase":"connected"' "status never became connected"
ts3_runtime_json_expect_fragment "${status_json}" '"backend":"plugin"' "status backend mismatch"
ts3_runtime_json_expect_fragment "${status_json}" '"nickname":"cli-e2e"' "status nickname mismatch"
ts3_runtime_json_expect_fragment "${status_json}" '"server":"127.0.0.1"' "status host mismatch"
ts3_runtime_json_expect_fragment "${status_json}" "\"port\":${server_port}" "status port mismatch"

server_info_json="$(TS_CONTROL_SOCKET_PATH="${socket_path}" "${ts_bin}" --json --config "${config_path}" server info)"
ts3_runtime_json_expect_fragment "${server_info_json}" '"backend":"plugin"' "server info backend mismatch"
ts3_runtime_json_expect_fragment "${server_info_json}" '"host":"127.0.0.1"' "server info host mismatch"
ts3_runtime_json_expect_fragment "${server_info_json}" "\"port\":${server_port}" "server info port mismatch"
ts3_runtime_json_expect_number_ge "${server_info_json}" "channel_count" 1 "server info channel count mismatch"
ts3_runtime_json_expect_number_ge "${server_info_json}" "client_count" 1 "server info client count mismatch"
ts3_runtime_json_expect_not_fragment "${server_info_json}" '"current_channel":null' "server info did not report a current channel"

channels_json="$(TS_CONTROL_SOCKET_PATH="${socket_path}" "${ts_bin}" --json --config "${config_path}" channel list)"
ts3_runtime_json_expect_nonempty_array "${channels_json}" "expected at least one channel"
default_channel_id="$(ts3_runtime_json_extract_first_array_object_id_matching "${channels_json}" '"is_default":true' || true)"
if [[ -z "${default_channel_id}" ]]; then
  echo "expected a default channel" >&2
  exit 1
fi

channel_json="$(TS_CONTROL_SOCKET_PATH="${socket_path}" "${ts_bin}" --json --config "${config_path}" channel get "${default_channel_id}")"
ts3_runtime_json_expect_fragment "${channel_json}" "\"id\":\"${default_channel_id}\"" "channel get returned unexpected id"

join_json="$(TS_CONTROL_SOCKET_PATH="${socket_path}" "${ts_bin}" --json --config "${config_path}" channel join "${default_channel_id}")"
ts3_runtime_json_expect_fragment "${join_json}" "\"current_channel\":\"${default_channel_id}\"" "channel join did not leave the client in the requested channel"

clients_json="$(TS_CONTROL_SOCKET_PATH="${socket_path}" "${ts_bin}" --json --config "${config_path}" client list)"
ts3_runtime_json_expect_nonempty_array "${clients_json}" "expected at least one client"
self_client_id="$(ts3_runtime_json_extract_first_array_object_id_matching "${clients_json}" '"nickname":"cli-e2e"' '"self":true' || true)"
if [[ -z "${self_client_id}" ]]; then
  echo "self client was not visible in client list" >&2
  exit 1
fi

self_client_json="$(TS_CONTROL_SOCKET_PATH="${socket_path}" "${ts_bin}" --json --config "${config_path}" client get "cli-e2e")"
ts3_runtime_json_expect_fragment "${self_client_json}" "\"id\":\"${self_client_id}\"" "client get returned the wrong client"
ts3_runtime_json_expect_fragment "${self_client_json}" '"self":true' "client get did not report self=true"

TS_CONTROL_SOCKET_PATH="${socket_path}" \
  "${ts_bin}" \
  --json \
  --config "${config_path}" \
  message send --target channel --id "${default_channel_id}" --text "cli-e2e ping" >/dev/null

events_json="$(TS_CONTROL_SOCKET_PATH="${socket_path}" "${ts_bin}" --json --config "${config_path}" events watch --count 8 --timeout-ms 1500)"
ts3_runtime_json_expect_nonempty_array "${events_json}" "expected at least one event from the live session"
ts3_runtime_json_expect_fragment "${events_json}" '"type":"connection.' "expected connection events from the live session"

TS_CONTROL_SOCKET_PATH="${socket_path}" "${ts_bin}" --json --config "${config_path}" disconnect >/dev/null
disconnected_json="$(wait_for_disconnected_status)"
ts3_runtime_json_expect_fragment "${disconnected_json}" '"phase":"disconnected"' "status never became disconnected"
