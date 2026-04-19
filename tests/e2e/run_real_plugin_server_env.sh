#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/e2e/real_plugin_runtime_common.sh
source "${script_dir}/real_plugin_runtime_common.sh"

ts_bin="${1:?usage: run_real_plugin_server_env.sh <ts-binary> <plugin-shared-library>}"
plugin_so="${2:?usage: run_real_plugin_server_env.sh <ts-binary> <plugin-shared-library>}"

docker_image="${TS3_REAL_E2E_DOCKER_IMAGE:-teamspeak:latest}"
server_port="${TS3_REAL_E2E_SERVER_PORT:-$((19000 + ($$ % 1000)))}"
query_port="${TS3_REAL_E2E_QUERY_PORT:-$((21000 + ($$ % 1000)))}"
autoconnect="${TS3_REAL_ENV_AUTOCONNECT:-1}"
nickname="${TS3_REAL_ENV_NICKNAME:-cli-manual}"
state_dir="${TS3_REAL_ENV_DIR:-}"
state_dir_owned=0

if [[ ! -x "${ts_bin}" ]]; then
  echo "ts binary not found or not executable: ${ts_bin}" >&2
  exit 1
fi

if [[ ! -f "${plugin_so}" ]]; then
  echo "plugin shared library not found: ${plugin_so}" >&2
  exit 1
fi

ts3_real_e2e_require_command Xvfb "Xvfb is required for the real TeamSpeak runtime environment"
ts3_real_e2e_require_command docker "docker is required for the real TeamSpeak runtime environment"
client_source_dir="$(ts3_real_e2e_resolve_client_source_dir)"
ts3_real_e2e_resolve_xdotool

if [[ -z "${state_dir}" ]]; then
  state_dir="$(mktemp -d)"
  state_dir_owned=1
else
  if [[ -e "${state_dir}" && -n "$(find "${state_dir}" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null)" ]]; then
    echo "TS3_REAL_ENV_DIR must point at an empty directory: ${state_dir}" >&2
    exit 1
  fi
  mkdir -p "${state_dir}"
fi

state_file="${state_dir}/env.state"
env_file="${state_dir}/env.sh"
client_runtime_dir="${state_dir}/client"
home_dir="${state_dir}/home"
config_path="${state_dir}/config.ini"
socket_path="${state_dir}/ts3cli.sock"
client_log="${state_dir}/client.log"
xvfb_log="${state_dir}/xvfb.log"
container_name="ts3cli-manual-${RANDOM}-${RANDOM}"
volume_name="ts3cli-manual-${RANDOM}-${RANDOM}"

status=0
client_pid=""
xvfb_pid=""
display=""
completed=0

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

client_running() {
  [[ -n "${client_pid}" ]] && kill -0 "${client_pid}" 2>/dev/null
}

cleanup_on_error() {
  status=$?
  if [[ ${completed} -eq 1 ]]; then
    exit 0
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
  if [[ "${state_dir_owned}" -eq 1 ]]; then
    rm -rf "${state_dir}"
  fi
  exit "${status}"
}
trap cleanup_on_error EXIT

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
    if printf '%s\n' "${logs}" | grep -Eq 'listening on 0\.0\.0\.0:9987|listening for query on 0\.0\.0\.0:10011'; then
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

start_client() {
  rm -f "${socket_path}"
  client_pid="$(
    cd "${client_runtime_dir}"
    export HOME="${home_dir}"
    export DISPLAY="${display}"
    export TS_CONTROL_SOCKET_PATH="${socket_path}"
    nohup ./ts3client_runscript.sh -nosingleinstance >"${client_log}" 2>&1 &
    echo $!
  )"
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

ensure_client_ready() {
  for _ in $(seq 1 4); do
    if [[ -S "${socket_path}" ]] && client_running; then
      return 0
    fi
    if client_running; then
      kill "${client_pid}" 2>/dev/null || true
      wait "${client_pid}" 2>/dev/null || true
    fi
    start_client
    if wait_for_socket; then
      dismiss_onboarding_dialogs
      if [[ -S "${socket_path}" ]]; then
        return 0
      fi
    fi
    sleep 1
  done
  echo "failed to keep the TeamSpeak client/plugin runtime alive" >&2
  return 1
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
    if ts3_real_e2e_json_expect_fragment "${last_status}" '"phase":"connected"' "connection phase not connected" 2>/dev/null; then
      return 0
    fi
    sleep 1
  done
  return 1
}

write_state_var() {
  printf '%s=%q\n' "$1" "$2" >>"${state_file}"
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

xvfb_pid="$(nohup Xvfb "${display}" -screen 0 1280x1024x24 -ac >"${xvfb_log}" 2>&1 & echo $!)"
sleep 1

ensure_client_ready

TS_CONTROL_SOCKET_PATH="${socket_path}" "${ts_bin}" config init --config "${config_path}" >/dev/null
TS_CONTROL_SOCKET_PATH="${socket_path}" "${ts_bin}" profile use plugin-local --config "${config_path}" >/dev/null

plugin_info_json="$(TS_CONTROL_SOCKET_PATH="${socket_path}" "${ts_bin}" --json --config "${config_path}" plugin info)"
ts3_real_e2e_json_expect_fragment "${plugin_info_json}" '"plugin_available":true' "plugin backend was not available"
ts3_real_e2e_json_expect_fragment "${plugin_info_json}" '"backend":"plugin"' "plugin backend did not report plugin mode"

if [[ "${autoconnect}" != "0" ]]; then
  connect_ok=0
  for _ in $(seq 1 8); do
    ensure_client_ready
    dismiss_onboarding_dialogs
    if TS_CONTROL_SOCKET_PATH="${socket_path}" \
        "${ts_bin}" \
        --json \
        --config "${config_path}" \
        --server "127.0.0.1:${server_port}" \
        --nickname "${nickname}" \
        connect >/dev/null 2>"${state_dir}/connect.err"; then
      if wait_for_connected_status 20; then
        connect_ok=1
        break
      fi
      sleep 1
      continue
    fi
    if grep -qi "currently not possible" "${state_dir}/connect.err" 2>/dev/null; then
      sleep 1
      continue
    fi
    cat "${state_dir}/connect.err" >&2 || true
    exit 1
  done

  if [[ "${connect_ok}" -ne 1 ]]; then
    echo "failed to connect the TeamSpeak client to the local test server" >&2
    cat "${state_dir}/connect.err" >&2 || true
    exit 1
  fi
fi

: >"${state_file}"
write_state_var "state_dir" "${state_dir}"
write_state_var "state_dir_owned" "${state_dir_owned}"
write_state_var "env_file" "${env_file}"
write_state_var "client_runtime_dir" "${client_runtime_dir}"
write_state_var "home_dir" "${home_dir}"
write_state_var "config_path" "${config_path}"
write_state_var "socket_path" "${socket_path}"
write_state_var "client_log" "${client_log}"
write_state_var "xvfb_log" "${xvfb_log}"
write_state_var "client_pid" "${client_pid}"
write_state_var "xvfb_pid" "${xvfb_pid}"
write_state_var "container_name" "${container_name}"
write_state_var "volume_name" "${volume_name}"
write_state_var "display" "${display}"
write_state_var "server_host" "127.0.0.1"
write_state_var "server_port" "${server_port}"
write_state_var "query_port" "${query_port}"
write_state_var "nickname" "${nickname}"
write_state_var "ts_bin" "${ts_bin}"
write_state_var "autoconnect" "${autoconnect}"
write_state_var "client_source_dir" "${client_source_dir}"
write_state_var "xdotool_bin" "${xdotool_bin}"
write_state_var "xdotool_library_path" "${xdotool_library_path}"

cat >"${env_file}" <<EOF
export TS_CONTROL_SOCKET_PATH='${socket_path}'
export TS_REAL_ENV_CONFIG='${config_path}'
export TS_REAL_ENV_STATE='${state_file}'
export TS_REAL_ENV_DISPLAY='${display}'
export TS_REAL_ENV_SERVER='127.0.0.1:${server_port}'
export TS_REAL_ENV_NICKNAME='${nickname}'
export TS_REAL_ENV_TS_BIN='${ts_bin}'
EOF

completed=1
trap - EXIT

cat <<EOF
Real TeamSpeak runtime is up.

State file: ${state_file}
Env file:   ${env_file}
Config:     ${config_path}
Socket:     ${socket_path}
Server:     127.0.0.1:${server_port}
Display:    ${display}
Nickname:   ${nickname}
Client:     ${client_source_dir}
xdotool:    ${xdotool_bin}

Use it like:
  source '${env_file}'
  "\${TS_REAL_ENV_TS_BIN}" --config "\${TS_REAL_ENV_CONFIG}" plugin info
  "\${TS_REAL_ENV_TS_BIN}" --config "\${TS_REAL_ENV_CONFIG}" status
  "\${TS_REAL_ENV_TS_BIN}" --config "\${TS_REAL_ENV_CONFIG}" channel list
  "\${TS_REAL_ENV_TS_BIN}" --config "\${TS_REAL_ENV_CONFIG}" client list
  "\${TS_REAL_ENV_TS_BIN}" --config "\${TS_REAL_ENV_CONFIG}" --server "\${TS_REAL_ENV_SERVER}" --nickname "\${TS_REAL_ENV_NICKNAME}" connect
  "\${TS_REAL_ENV_TS_BIN}" --config "\${TS_REAL_ENV_CONFIG}" disconnect

Stop it with:
  ${script_dir}/stop_real_plugin_server_env.sh '${state_file}'
EOF
