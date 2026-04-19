#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/e2e/real_plugin_runtime_common.sh
source "${script_dir}/real_plugin_runtime_common.sh"

plugin_sdk_dir="$(ts3_real_e2e_resolve_plugin_sdk_dir)"
client_source_dir="$(ts3_real_e2e_resolve_client_source_dir)"
ts3_real_e2e_resolve_xdotool
ts3_real_e2e_write_default_env_files "${plugin_sdk_dir}" "${client_source_dir}" "${xdotool_bin}" "${xdotool_library_path}"

deps_mk_path="${TS3_REAL_E2E_DEPS_MK:-${ts3_real_e2e_cache_dir_default}/deps.mk}"
deps_env_path="${TS3_REAL_E2E_DEPS_ENV:-${ts3_real_e2e_cache_dir_default}/deps.env}"

cat <<EOF
Real TeamSpeak runtime dependencies are ready.

Cache:        ${ts3_real_e2e_cache_dir_default}
Plugin SDK:   ${plugin_sdk_dir}
ClientDir:    ${client_source_dir}
xdotool:      ${xdotool_bin}
xdotool libs: ${xdotool_library_path:-system}
deps.mk:      ${deps_mk_path}
deps.env:     ${deps_env_path}

If you want to reuse the downloaded runtime paths explicitly:
  TS3_PLUGIN_SDK_DIR='${plugin_sdk_dir}'
  TS3_REAL_E2E_CLIENT_DIR='${client_source_dir}'
  TS3_REAL_E2E_XDOTOOL='${xdotool_bin}'
  TS3_REAL_E2E_XDOTOOL_LIBRARY_PATH='${xdotool_library_path}'
EOF
