#!/usr/bin/env bash
set -euo pipefail

state_file="${1:?usage: stop_real_plugin_server_env.sh <state-file>}"

if [[ ! -f "${state_file}" ]]; then
  echo "state file not found: ${state_file}" >&2
  exit 1
fi

# shellcheck disable=SC1090
source "${state_file}"

if [[ -n "${client_pid:-}" ]]; then
  kill "${client_pid}" 2>/dev/null || true
  wait "${client_pid}" 2>/dev/null || true
fi

if [[ -n "${xvfb_pid:-}" ]]; then
  kill "${xvfb_pid}" 2>/dev/null || true
  wait "${xvfb_pid}" 2>/dev/null || true
fi

if [[ -n "${container_name:-}" ]]; then
  docker rm -f "${container_name}" >/dev/null 2>&1 || true
fi

if [[ -n "${volume_name:-}" ]]; then
  docker volume rm -f "${volume_name}" >/dev/null 2>&1 || true
fi

if [[ "${state_dir_owned:-0}" == "1" && -n "${state_dir:-}" && -d "${state_dir}" ]]; then
  rm -rf "${state_dir}"
elif [[ -n "${client_runtime_dir:-}" || -n "${home_dir:-}" || -n "${env_file:-}" ]]; then
  rm -rf \
    "${client_runtime_dir:-}" \
    "${home_dir:-}" \
    "${config_path:-}" \
    "${socket_path:-}" \
    "${client_log:-}" \
    "${xvfb_log:-}" \
    "${env_file:-}" \
    "${state_file}"
fi

echo "real TeamSpeak runtime stopped"
