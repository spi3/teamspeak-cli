#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

log() {
  printf '[uninstall] %s\n' "$*"
}

die() {
  printf '%s\n' "$*" >&2
  exit 1
}

home_dir="${HOME:-}"
[[ -n "${home_dir}" ]] || die "HOME must be set for a user-level uninstall"

xdg_data_home="${XDG_DATA_HOME:-${home_dir}/.local/share}"
xdg_cache_home="${XDG_CACHE_HOME:-${home_dir}/.cache}"
xdg_config_home="${XDG_CONFIG_HOME:-${home_dir}/.config}"

prefix_override=""
receipt_path="${TS_INSTALL_RECEIPT_PATH:-}"
keep_config=0

usage() {
  cat <<EOF
Usage: ./scripts/uninstall.sh [options]

Remove the files previously installed by ./scripts/install.sh or
./scripts/install-release.sh.

Options:
  --prefix DIR      Resolve the install receipt under DIR/share/teamspeak-cli
  --receipt FILE    Read uninstall metadata from FILE
  --keep-config     Preserve the ts config even if the installer created it
  -h, --help        Show this help text
EOF
}

remove_file_if_present() {
  local file_path="$1"

  if [[ -L "${file_path}" || -f "${file_path}" ]]; then
    rm -f "${file_path}"
    log "removed ${file_path}"
  fi
}

remove_owned_dir_if_present() {
  local dir_path="$1"

  if [[ ! -d "${dir_path}" ]]; then
    return 0
  fi

  if [[ -f "${dir_path}/.teamspeak-cli-install-owner" ]]; then
    rm -rf "${dir_path}"
    log "removed ${dir_path}"
    return 0
  fi

  return 1
}

maybe_rmdir() {
  local dir_path="$1"
  rmdir "${dir_path}" 2>/dev/null || true
}

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --prefix)
      [[ "$#" -ge 2 ]] || die "--prefix requires a value"
      prefix_override="$2"
      shift 2
      ;;
    --receipt)
      [[ "$#" -ge 2 ]] || die "--receipt requires a value"
      receipt_path="$2"
      shift 2
      ;;
    --keep-config)
      keep_config=1
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

if [[ -z "${receipt_path}" ]]; then
  if [[ -n "${prefix_override}" ]]; then
    receipt_path="${prefix_override%/}/share/teamspeak-cli/install-receipt.env"
  else
    script_prefix="$(cd "${script_dir}/.." && pwd)"
    script_receipt="${script_prefix}/share/teamspeak-cli/install-receipt.env"
    default_receipt="${xdg_data_home}/teamspeak-cli/install-receipt.env"

    if [[ -f "${script_receipt}" ]]; then
      receipt_path="${script_receipt}"
    else
      receipt_path="${default_receipt}"
    fi
  fi
fi

if [[ -f "${receipt_path}" ]]; then
  # shellcheck source=/dev/null
  source "${receipt_path}"
  log "loaded install receipt from ${receipt_path}"
fi

prefix="${prefix:-${prefix_override:-${home_dir}/.local}}"
prefix="${prefix%/}"
share_dir="${share_dir:-${prefix}/share/teamspeak-cli}"
client_install_dir="${client_install_dir:-${xdg_data_home}/teamspeak-cli/teamspeak3-client}"
managed_dir="${managed_dir:-${xdg_cache_home}/teamspeak-cli/install}"
build_dir="${build_dir:-}"
config_path="${config_path:-${xdg_config_home}/ts/config.ini}"
config_created_by_installer="${config_created_by_installer:-0}"
ts_bin_path="${ts_bin_path:-${prefix}/bin/ts}"
ts3client_wrapper_path="${ts3client_wrapper_path:-${prefix}/bin/ts3client}"
client_alias_path="${client_alias_path:-${prefix}/bin/teamspeak3-client}"
uninstall_bin_path="${uninstall_bin_path:-${prefix}/bin/ts-uninstall}"

share_dir_removed=0
if remove_owned_dir_if_present "${share_dir}"; then
  share_dir_removed=1
else
  if [[ -d "${share_dir}" ]]; then
    remove_file_if_present "${share_dir}/install-receipt.env"
    remove_file_if_present "${share_dir}/.teamspeak-cli-install-owner"
    rm -rf "${share_dir}/docs" "${share_dir}/examples"
    remove_file_if_present "${share_dir}/README.md"
    remove_file_if_present "${share_dir}/LICENSE"
    maybe_rmdir "${share_dir}"
  fi
fi

if [[ "${client_install_dir}" != "${share_dir}" && "${client_install_dir}" != "${share_dir}/"* ]]; then
  remove_owned_dir_if_present "${client_install_dir}" || true
elif [[ "${share_dir_removed}" -eq 0 && -d "${client_install_dir}" ]]; then
  remove_owned_dir_if_present "${client_install_dir}" || true
fi

remove_owned_dir_if_present "${managed_dir}" || true
if [[ -n "${build_dir}" ]]; then
  remove_owned_dir_if_present "${build_dir}" || true
fi

remove_file_if_present "${ts_bin_path}"
remove_file_if_present "${ts3client_wrapper_path}"
remove_file_if_present "${client_alias_path}"
remove_file_if_present "${uninstall_bin_path}"

if [[ "${keep_config}" -eq 0 && "${config_created_by_installer}" == "1" ]]; then
  remove_file_if_present "${config_path}"
  maybe_rmdir "$(dirname "${config_path}")"
fi

maybe_rmdir "${prefix}/bin"
maybe_rmdir "${prefix}/share"
maybe_rmdir "$(dirname "${managed_dir}")"
maybe_rmdir "${xdg_data_home}/teamspeak-cli"

cat <<EOF

Uninstall complete.

Removed:
  ${ts_bin_path}
  ${ts3client_wrapper_path}
  ${client_alias_path}
  ${uninstall_bin_path}
  ${share_dir}
  ${client_install_dir}
  ${managed_dir}
$(if [[ -n "${build_dir}" ]]; then printf '  %s\n' "${build_dir}"; fi)$(if [[ "${keep_config}" -eq 0 && "${config_created_by_installer}" == "1" ]]; then printf '  %s\n' "${config_path}"; fi)
EOF
