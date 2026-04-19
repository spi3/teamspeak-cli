#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/e2e/runtime_common.sh
source "${script_dir}/runtime_common.sh"

plugin_sdk_dir="$(ts3_runtime_resolve_plugin_sdk_dir)"
client_source_dir="$(ts3_runtime_resolve_client_source_dir)"
ts3_runtime_resolve_client_runtime_library_path "${client_source_dir}"
ts3_runtime_resolve_xdotool
ts3_runtime_write_default_env_files "${plugin_sdk_dir}" "${client_source_dir}" "${client_runtime_library_path}" "${xdotool_bin}" "${xdotool_library_path}"

deps_mk_path="${TS3_DEPS_MK:-${ts3_runtime_managed_dir_default}/deps.mk}"
deps_env_path="${TS3_DEPS_ENV:-${ts3_runtime_managed_dir_default}/deps.env}"

cat <<EOF
TeamSpeak-managed dependencies are ready.

Managed dir:  ${ts3_runtime_managed_dir_default}
Plugin SDK:   ${plugin_sdk_dir}
ClientDir:    ${client_source_dir}
Client libs:  ${client_runtime_library_path:-system}
xdotool:      ${xdotool_bin}
xdotool libs: ${xdotool_library_path:-system}
deps.mk:      ${deps_mk_path}
deps.env:     ${deps_env_path}

If you want to reuse the downloaded runtime paths explicitly:
  TS3_PLUGIN_SDK_DIR='${plugin_sdk_dir}'
  TS3_CLIENT_DIR='${client_source_dir}'
  TS3_CLIENT_LIBRARY_PATH='${client_runtime_library_path}'
  TS3_XDOTOOL='${xdotool_bin}'
  TS3_XDOTOOL_LIBRARY_PATH='${xdotool_library_path}'
EOF
