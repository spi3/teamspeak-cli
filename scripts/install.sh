#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

have_command() {
  command -v "$1" >/dev/null 2>&1
}

die() {
  printf '%s\n' "$*" >&2
  exit 1
}

log() {
  printf '[install] %s\n' "$*"
}

write_install_marker() {
  local dir_path="$1"
  local marker_kind="$2"

  mkdir -p "${dir_path}"
  {
    printf 'kind=%q\n' "${marker_kind}"
    printf 'repo_root=%q\n' "${repo_root}"
  } >"${dir_path}/.teamspeak-cli-install-owner"
}

home_dir="${HOME:-}"
[[ -n "${home_dir}" ]] || die "HOME must be set for a user-level install"

xdg_data_home="${XDG_DATA_HOME:-${home_dir}/.local/share}"
xdg_cache_home="${XDG_CACHE_HOME:-${home_dir}/.cache}"
xdg_config_home="${XDG_CONFIG_HOME:-${home_dir}/.config}"

prefix="${PREFIX:-${home_dir}/.local}"
client_install_dir="${TS3_INSTALL_DIR:-${xdg_data_home}/teamspeak-cli/teamspeak3-client}"
build_dir="${BUILD_DIR:-${repo_root}/build-install}"
managed_dir="${TS3_MANAGED_DIR:-${xdg_cache_home}/teamspeak-cli/install}"
config_path="${TS_CONFIG_PATH:-${xdg_config_home}/ts/config.ini}"
build_type="${CMAKE_BUILD_TYPE:-Release}"
skip_config=0

cmake_bin=""
ninja_bin=""
plugin_sdk_dir=""
client_source_dir=""
xvfb_bin=""
xvfb_library_path=""
xvfb_xkb_dir=""
xvfb_binary_dir=""
config_created_by_installer=0

usage() {
  cat <<EOF
Usage: ./scripts/install.sh [options]

Download the TeamSpeak 3 client, build the TeamSpeak CLI and plugin, and
install them for the current user.

Options:
  --prefix DIR        Install ts under DIR/bin and supporting files under DIR/share
                      Default: ${prefix}
  --client-dir DIR    Install the TeamSpeak client bundle and plugin here
                      Default: ${client_install_dir}
  --build-dir DIR     Use this CMake build directory
                      Default: ${build_dir}
  --managed-dir DIR   Cache TeamSpeak downloads and SDK assets here
                      Default: ${managed_dir}
  --config-path FILE  Initialize ts config at FILE
                      Default: ${config_path}
  --build-type TYPE   CMake build type
                      Default: ${build_type}
  --skip-config       Do not create or touch the user's ts config
  -h, --help          Show this help text
EOF
}

resolve_cmake_bin() {
  if [[ -n "${CMAKE:-}" ]]; then
    printf '%s\n' "${CMAKE}"
    return 0
  fi
  if have_command cmake; then
    command -v cmake
    return 0
  fi
  if [[ -x "${home_dir}/.local/venvs/build-tools/bin/cmake" ]]; then
    printf '%s\n' "${home_dir}/.local/venvs/build-tools/bin/cmake"
    return 0
  fi
  return 1
}

resolve_ninja_bin() {
  if [[ -n "${NINJA:-}" ]]; then
    printf '%s\n' "${NINJA}"
    return 0
  fi
  if have_command ninja; then
    command -v ninja
    return 0
  fi
  if [[ -x "${home_dir}/.local/venvs/build-tools/bin/ninja" ]]; then
    printf '%s\n' "${home_dir}/.local/venvs/build-tools/bin/ninja"
    return 0
  fi
  return 1
}

ensure_requirements() {
  local missing=()

  cmake_bin="$(resolve_cmake_bin || true)"
  ninja_bin="$(resolve_ninja_bin || true)"

  [[ -n "${cmake_bin}" ]] || missing+=("cmake")
  if [[ -z "${ninja_bin}" ]] && ! have_command make; then
    missing+=("ninja or make")
  fi
  if ! have_command c++ && ! have_command g++; then
    missing+=("c++ compiler")
  fi
  if ! have_command curl && ! have_command wget; then
    missing+=("curl or wget")
  fi
  have_command tar || missing+=("tar")

  if [[ "${#missing[@]}" -gt 0 ]]; then
    die "missing required tools: ${missing[*]}"
  fi
}

install_client_wrapper() {
  "${script_dir}/write-client-wrapper.sh" "${ts3client_wrapper_path}" "${client_install_dir}"
  ln -sfn "ts3client" "${client_alias_path}"
}

install_uninstaller() {
  install -d "${prefix}/bin"
  install -m 0755 "${script_dir}/uninstall.sh" "${uninstall_bin_path}"
}

configure_and_build() {
  local generator
  local cmake_args=()

  if [[ -n "${CMAKE_GENERATOR:-}" ]]; then
    generator="${CMAKE_GENERATOR}"
  elif [[ -n "${ninja_bin}" ]]; then
    generator="Ninja"
  else
    generator="Unix Makefiles"
  fi

  cmake_args=(
    -S "${repo_root}"
    -B "${build_dir}"
    -G "${generator}"
    -DCMAKE_BUILD_TYPE="${build_type}"
    -DBUILD_TESTING=OFF
    -DTS_ENABLE_SANITIZERS=OFF
    -DTS_ENABLE_TS3_PLUGIN=ON
    -DTS_ENABLE_TS3_E2E=OFF
    -DTS3_PLUGIN_SDK_DIR="${plugin_sdk_dir}"
  )

  if [[ "${generator}" == "Ninja" ]]; then
    cmake_args+=("-DCMAKE_MAKE_PROGRAM=${ninja_bin}")
  fi

  log "configuring ${build_type} build in ${build_dir}"
  "${cmake_bin}" "${cmake_args[@]}"
  log "building ts and ts3cli_plugin"
  "${cmake_bin}" --build "${build_dir}" --parallel
}

stage_client_install() {
  local staging_dir="${client_install_dir}.tmp.$$"
  local runtime_lib_root="${managed_dir}/client-runtime-libs/root"

  mkdir -p "$(dirname "${client_install_dir}")"
  rm -rf "${staging_dir}"
  mkdir -p "${staging_dir}"
  cp -a "${client_source_dir}/." "${staging_dir}/"

  if [[ -d "${runtime_lib_root}" ]]; then
    mkdir -p "${staging_dir}/runtime-libs"
    cp -a "${runtime_lib_root}/." "${staging_dir}/runtime-libs/"
  fi

  install -d "${staging_dir}/plugins"
  install -m 0755 "${build_dir}/ts3cli_plugin.so" "${staging_dir}/plugins/ts3cli_plugin.so"
  write_install_marker "${staging_dir}" "client-install-dir"

  rm -rf "${client_install_dir}"
  mv "${staging_dir}" "${client_install_dir}"
}

write_receipt() {
  local receipt_tmp

  receipt_tmp="$(mktemp)"
  {
    printf 'receipt_version=%q\n' "1"
    printf 'prefix=%q\n' "${prefix}"
    printf 'share_dir=%q\n' "${share_dir}"
    printf 'client_install_dir=%q\n' "${client_install_dir}"
    printf 'managed_dir=%q\n' "${managed_dir}"
    printf 'build_dir=%q\n' "${build_dir}"
    printf 'config_path=%q\n' "${config_path}"
    printf 'config_created_by_installer=%q\n' "${config_created_by_installer}"
    printf 'ts_bin_path=%q\n' "${ts_bin_path}"
    printf 'ts3client_wrapper_path=%q\n' "${ts3client_wrapper_path}"
    printf 'client_alias_path=%q\n' "${client_alias_path}"
    printf 'uninstall_bin_path=%q\n' "${uninstall_bin_path}"
    printf 'receipt_path=%q\n' "${receipt_path}"
    printf 'xvfb_bin_path=%q\n' "${xvfb_bin}"
    printf 'xvfb_library_path=%q\n' "${xvfb_library_path}"
    printf 'xvfb_xkb_dir=%q\n' "${xvfb_xkb_dir}"
    printf 'xvfb_binary_dir=%q\n' "${xvfb_binary_dir}"
  } >"${receipt_tmp}"

  install -d "${share_dir}"
  mv "${receipt_tmp}" "${receipt_path}"
}

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --prefix)
      [[ "$#" -ge 2 ]] || die "--prefix requires a value"
      prefix="$2"
      shift 2
      ;;
    --client-dir)
      [[ "$#" -ge 2 ]] || die "--client-dir requires a value"
      client_install_dir="$2"
      shift 2
      ;;
    --build-dir)
      [[ "$#" -ge 2 ]] || die "--build-dir requires a value"
      build_dir="$2"
      shift 2
      ;;
    --managed-dir)
      [[ "$#" -ge 2 ]] || die "--managed-dir requires a value"
      managed_dir="$2"
      shift 2
      ;;
    --config-path)
      [[ "$#" -ge 2 ]] || die "--config-path requires a value"
      config_path="$2"
      shift 2
      ;;
    --build-type)
      [[ "$#" -ge 2 ]] || die "--build-type requires a value"
      build_type="$2"
      shift 2
      ;;
    --skip-config)
      skip_config=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown argument: $1"
      ;;
  esac
done

[[ "$(uname -s)" == "Linux" ]] || die "this installer currently supports Linux only"
[[ "$(uname -m)" == "x86_64" ]] || die "this installer currently supports x86_64 only"

prefix="${prefix%/}"
share_dir="${prefix}/share/teamspeak-cli"
ts_bin_path="${prefix}/bin/ts"
ts3client_wrapper_path="${prefix}/bin/ts3client"
client_alias_path="${prefix}/bin/teamspeak3-client"
uninstall_bin_path="${prefix}/bin/ts-uninstall"
receipt_path="${share_dir}/install-receipt.env"

ensure_requirements

export TS3_MANAGED_DIR="${managed_dir}"
# shellcheck source=tests/e2e/runtime_common.sh
source "${repo_root}/tests/e2e/runtime_common.sh"

log "bootstrapping TeamSpeak client bundle and plugin SDK into ${managed_dir}"
plugin_sdk_dir="$(ts3_runtime_resolve_plugin_sdk_dir)"
client_source_dir="$(ts3_runtime_resolve_client_source_dir)"
ts3_runtime_resolve_client_runtime_library_path "${client_source_dir}"
ts3_runtime_resolve_xdotool >/dev/null
ts3_runtime_resolve_xvfb >/dev/null
ts3_runtime_prepare_pulseaudio_runtime
write_install_marker "${managed_dir}" "managed-dir"

configure_and_build
write_install_marker "${build_dir}" "build-dir"

[[ -x "${build_dir}/ts" ]] || die "built ts binary was not found in ${build_dir}"
[[ -f "${build_dir}/ts3cli_plugin.so" ]] || die "built ts3cli_plugin.so was not found in ${build_dir}"

log "installing ts into ${prefix}"
"${cmake_bin}" --install "${build_dir}" --prefix "${prefix}"
write_install_marker "${share_dir}" "share-dir"

log "installing TeamSpeak client bundle into ${client_install_dir}"
stage_client_install
install_client_wrapper
install_uninstaller

if [[ "${skip_config}" -eq 0 ]]; then
  if [[ ! -e "${config_path}" ]]; then
    config_created_by_installer=1
  fi
  log "initializing ts config at ${config_path}"
  "${ts_bin_path}" config init --config "${config_path}" >/dev/null
fi

write_receipt

cat <<EOF

Install complete.

ts binary:           ${ts_bin_path}
TeamSpeak launcher:  ${ts3client_wrapper_path}
Uninstaller:         ${uninstall_bin_path}
Client bundle:       ${client_install_dir}
Plugin library:      ${client_install_dir}/plugins/ts3cli_plugin.so
Xvfb:                ${xvfb_bin}
Config path:         ${config_path}

If ${prefix}/bin is not already on your PATH, add:
  export PATH="${prefix}/bin:\$PATH"

Next steps:
  1. Start the client with: ${ts3client_wrapper_path}
  2. Enable the ts3cli plugin in TeamSpeak if it is not already enabled
  3. Verify the bridge with: ${ts_bin_path} plugin info
EOF
