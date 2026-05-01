#!/usr/bin/env bash
set -euo pipefail

script_source_path="${BASH_SOURCE[0]:-}"
script_dir=""
repo_root=""
local_support_available=0
runtime_common_path=""
uninstall_source_path=""
client_wrapper_source_path=""
support_cache_dir=""
support_source_repo="${INSTALL_SCRIPT_REPO:-spi3/teamspeak-cli}"
support_source_ref="${INSTALL_SCRIPT_REF:-main}"
support_base_url="${INSTALL_SCRIPT_BASE_URL:-https://raw.githubusercontent.com/${support_source_repo}/${support_source_ref}}"

if [[ -n "${script_source_path}" ]]; then
  script_dir="$(cd "$(dirname "${script_source_path}")" 2>/dev/null && pwd || true)"
  if [[ -n "${script_dir}" ]]; then
    repo_root="$(cd "${script_dir}/.." 2>/dev/null && pwd || true)"
    if [[ -n "${repo_root}" && -f "${repo_root}/tests/e2e/runtime_common.sh" && -f "${repo_root}/scripts/uninstall.sh" && -f "${repo_root}/scripts/write-client-wrapper.sh" ]]; then
      local_support_available=1
      runtime_common_path="${repo_root}/tests/e2e/runtime_common.sh"
      uninstall_source_path="${repo_root}/scripts/uninstall.sh"
      client_wrapper_source_path="${repo_root}/scripts/write-client-wrapper.sh"
    fi
  fi
fi

if [[ "${local_support_available}" -ne 1 ]]; then
  repo_root="${support_source_repo}@${support_source_ref}"
fi

have_command() {
  command -v "$1" >/dev/null 2>&1
}

die() {
  printf '%s\n' "$*" >&2
  exit 1
}

log() {
  printf '[install-release] %s\n' "$*"
}

cleanup_support_cache() {
  if [[ -n "${support_cache_dir}" && -d "${support_cache_dir}" ]]; then
    rm -rf "${support_cache_dir}"
  fi
}

trap cleanup_support_cache EXIT

write_install_marker() {
  local dir_path="$1"
  local marker_kind="$2"

  mkdir -p "${dir_path}"
  {
    printf 'kind=%q\n' "${marker_kind}"
    printf 'repo_root=%q\n' "${repo_root}"
  } >"${dir_path}/.teamspeak-cli-install-owner"
}

extract_github_repo() {
  local remote_url="$1"

  remote_url="${remote_url%.git}"

  case "${remote_url}" in
    git@github.com:*)
      printf '%s\n' "${remote_url#git@github.com:}"
      ;;
    https://github.com/*)
      printf '%s\n' "${remote_url#https://github.com/}"
      ;;
    ssh://git@github.com/*)
      printf '%s\n' "${remote_url#ssh://git@github.com/}"
      ;;
    *)
      return 1
      ;;
  esac
}

detect_default_release_repo() {
  local remote_url
  local detected_repo

  if [[ "${local_support_available}" -eq 1 ]] && have_command git; then
    remote_url="$(git -C "${repo_root}" remote get-url origin 2>/dev/null || true)"
    if [[ -n "${remote_url}" ]]; then
      detected_repo="$(extract_github_repo "${remote_url}" || true)"
      if [[ -n "${detected_repo}" ]]; then
        printf '%s\n' "${detected_repo}"
        return 0
      fi
    fi
  fi

  printf '%s\n' "spi3/teamspeak-cli"
}

download_url_to_path() {
  local url="$1"
  local destination="$2"
  local token="${GH_TOKEN:-${GITHUB_TOKEN:-}}"

  mkdir -p "$(dirname "${destination}")"

  if have_command curl; then
    if [[ -n "${token}" ]]; then
      curl \
        --fail \
        --location \
        --silent \
        --show-error \
        -H "Authorization: Bearer ${token}" \
        --output "${destination}" \
        "${url}"
      return 0
    fi

    curl \
      --fail \
      --location \
      --silent \
      --show-error \
      --output "${destination}" \
      "${url}"
    return 0
  fi

  if have_command wget; then
    if [[ -n "${token}" ]]; then
      wget \
        -qO "${destination}" \
        --header="Authorization: Bearer ${token}" \
        "${url}"
      return 0
    fi

    wget \
      -qO "${destination}" \
      "${url}"
    return 0
  fi

  die "curl or wget is required to fetch ${url}"
}

download_text() {
  local url="$1"
  local token="${GH_TOKEN:-${GITHUB_TOKEN:-}}"

  if have_command curl; then
    if [[ -n "${token}" ]]; then
      curl \
        --fail \
        --location \
        --silent \
        --show-error \
        -H "Accept: application/vnd.github+json" \
        -H "Authorization: Bearer ${token}" \
        "${url}"
      return 0
    fi

    curl \
      --fail \
      --location \
      --silent \
      --show-error \
      -H "Accept: application/vnd.github+json" \
      "${url}"
    return 0
  fi

  if have_command wget; then
    if [[ -n "${token}" ]]; then
      wget \
        -qO- \
        --header="Accept: application/vnd.github+json" \
        --header="Authorization: Bearer ${token}" \
        "${url}"
      return 0
    fi

    wget \
      -qO- \
      --header="Accept: application/vnd.github+json" \
      "${url}"
    return 0
  fi

  die "curl or wget is required to fetch ${url}"
}

extract_json_string() {
  local json_payload="$1"
  local field_name="$2"

  printf '%s' "${json_payload}" | tr -d '\n' | sed -nE "s/.*\"${field_name}\"[[:space:]]*:[[:space:]]*\"([^\"]+)\".*/\\1/p" | head -n 1
}

set_support_base_url() {
  support_base_url="${INSTALL_SCRIPT_BASE_URL:-https://raw.githubusercontent.com/${support_source_repo}/${support_source_ref}}"
}

resolve_support_files() {
  if [[ "${local_support_available}" -eq 1 ]]; then
    return 0
  fi

  support_cache_dir="$(mktemp -d)"
  runtime_common_path="${support_cache_dir}/runtime_common.sh"
  uninstall_source_path="${support_cache_dir}/uninstall.sh"
  client_wrapper_source_path="${support_cache_dir}/write-client-wrapper.sh"

  log "downloading runtime support files from ${support_base_url}"
  download_url_to_path "${support_base_url}/tests/e2e/runtime_common.sh" "${runtime_common_path}" || {
    die "failed to download runtime_common.sh from ${support_base_url}"
  }
  download_url_to_path "${support_base_url}/scripts/uninstall.sh" "${uninstall_source_path}" || {
    die "failed to download uninstall.sh from ${support_base_url}"
  }
  download_url_to_path "${support_base_url}/scripts/write-client-wrapper.sh" "${client_wrapper_source_path}" || {
    die "failed to download write-client-wrapper.sh from ${support_base_url}"
  }
  chmod 0755 "${client_wrapper_source_path}"
}

home_dir="${HOME:-}"
[[ -n "${home_dir}" ]] || die "HOME must be set for a user-level install"

xdg_data_home="${XDG_DATA_HOME:-${home_dir}/.local/share}"
xdg_cache_home="${XDG_CACHE_HOME:-${home_dir}/.cache}"
xdg_config_home="${XDG_CONFIG_HOME:-${home_dir}/.config}"

default_release_repo="$(detect_default_release_repo)"

prefix="${PREFIX:-${home_dir}/.local}"
client_install_dir="${TS3_INSTALL_DIR:-${xdg_data_home}/teamspeak-cli/teamspeak3-client}"
managed_dir="${TS3_MANAGED_DIR:-${xdg_cache_home}/teamspeak-cli/install}"
config_path="${TS_CONFIG_PATH:-${xdg_config_home}/ts/config.ini}"
release_repo="${TS_RELEASE_REPO:-${default_release_repo}}"
release_tag="${TS_RELEASE_TAG:-}"
platform="linux-x86_64"
skip_config=0

client_source_dir=""
xvfb_bin=""
xvfb_library_path=""
xvfb_xkb_dir=""
xvfb_binary_dir=""
config_created_by_installer=0
build_dir=""
release_archive_path=""
release_checksum_path=""
release_root=""

usage() {
  cat <<EOF
Usage: ./scripts/install-release.sh [options]

Download the latest published teamspeak-cli release archive from GitHub, install
the CLI, install the TeamSpeak 3 client bundle, and install the bundled plugin
into that client for the current user.

Options:
  --repo OWNER/REPO    Download release assets from this GitHub repository
                       Default: ${release_repo}
  --release-tag TAG    Install a specific release tag instead of the latest release
  --prefix DIR         Install ts under DIR/bin and supporting files under DIR/share
                       Default: ${prefix}
  --client-dir DIR     Install the TeamSpeak client bundle and plugin here
                       Default: ${client_install_dir}
  --managed-dir DIR    Cache release downloads and TeamSpeak runtime assets here
                       Default: ${managed_dir}
  --config-path FILE   Initialize ts config at FILE
                       Default: ${config_path}
  --skip-config        Do not create or touch the user's ts config
  -h, --help           Show this help text
EOF
}

ensure_requirements() {
  local missing=()

  if ! have_command curl && ! have_command wget; then
    missing+=("curl or wget")
  fi
  have_command tar || missing+=("tar")
  if ! have_command sha256sum && ! have_command shasum; then
    missing+=("sha256sum or shasum")
  fi

  if [[ "${#missing[@]}" -gt 0 ]]; then
    die "missing required tools: ${missing[*]}"
  fi
}

resolve_release_tag() {
  local metadata

  if [[ -n "${release_tag}" ]]; then
    return 0
  fi

  log "resolving latest release tag from ${release_repo}"
  metadata="$(download_text "https://api.github.com/repos/${release_repo}/releases/latest")" || {
    die "failed to query the latest release for ${release_repo}"
  }

  release_tag="$(extract_json_string "${metadata}" "tag_name")"
  [[ -n "${release_tag}" ]] || die "failed to parse the latest release tag for ${release_repo}"
}

install_client_wrapper() {
  "${client_wrapper_source_path}" "${ts3client_wrapper_path}" "${client_install_dir}"
  ln -sfn "ts3client" "${client_alias_path}"
}

install_uninstaller() {
  install -d "${prefix}/bin"
  install -m 0755 "${uninstall_source_path}" "${uninstall_bin_path}"
}

stage_client_install() {
  local plugin_source_path="$1"
  local staging_dir="${client_install_dir}.tmp.$$"
  local runtime_lib_root="${managed_dir}/client-runtime-libs/root"

  [[ -f "${plugin_source_path}" ]] || die "release plugin library was not found at ${plugin_source_path}"

  mkdir -p "$(dirname "${client_install_dir}")"
  rm -rf "${staging_dir}"
  mkdir -p "${staging_dir}"
  cp -a "${client_source_dir}/." "${staging_dir}/"

  if [[ -d "${runtime_lib_root}" ]]; then
    mkdir -p "${staging_dir}/runtime-libs"
    cp -a "${runtime_lib_root}/." "${staging_dir}/runtime-libs/"
  fi

  install -d "${staging_dir}/plugins"
  install -m 0755 "${plugin_source_path}" "${staging_dir}/plugins/ts3cli_plugin.so"
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
    printf 'release_repo=%q\n' "${release_repo}"
    printf 'release_tag=%q\n' "${release_tag}"
    printf 'release_archive_path=%q\n' "${release_archive_path}"
    printf 'xvfb_bin_path=%q\n' "${xvfb_bin}"
    printf 'xvfb_library_path=%q\n' "${xvfb_library_path}"
    printf 'xvfb_xkb_dir=%q\n' "${xvfb_xkb_dir}"
    printf 'xvfb_binary_dir=%q\n' "${xvfb_binary_dir}"
  } >"${receipt_tmp}"

  install -d "${share_dir}"
  mv "${receipt_tmp}" "${receipt_path}"
}

download_release_archive() {
  local release_cache_dir="${managed_dir}/releases/${release_tag}"
  local archive_name="ts-${release_tag}-${platform}.tar.gz"
  local checksum_name="${archive_name}.sha256"
  local archive_url="https://github.com/${release_repo}/releases/download/${release_tag}/${archive_name}"
  local checksum_url="https://github.com/${release_repo}/releases/download/${release_tag}/${checksum_name}"
  local expected_sha256

  mkdir -p "${release_cache_dir}"
  release_archive_path="${release_cache_dir}/${archive_name}"
  release_checksum_path="${release_cache_dir}/${checksum_name}"

  log "downloading ${checksum_name}"
  ts3_runtime_ensure_downloaded_file "${checksum_url}" "${release_checksum_path}" ""

  expected_sha256="$(awk 'NR == 1 {print $1}' "${release_checksum_path}")"
  [[ -n "${expected_sha256}" ]] || die "failed to read the checksum from ${release_checksum_path}"

  log "downloading ${archive_name}"
  ts3_runtime_ensure_downloaded_file "${archive_url}" "${release_archive_path}" "${expected_sha256}"
}

extract_release_archive() {
  local extract_parent="${managed_dir}/releases/${release_tag}/extract"
  local extract_tmp="${extract_parent}.tmp.$$"
  local release_root_count

  rm -rf "${extract_tmp}"
  mkdir -p "${extract_tmp}"

  log "extracting release archive to ${extract_parent}"
  tar -xzf "${release_archive_path}" -C "${extract_tmp}"

  release_root_count="$(find "${extract_tmp}" -mindepth 1 -maxdepth 1 -type d | wc -l | tr -d ' ')"
  [[ "${release_root_count}" == "1" ]] || die "expected one top-level directory in ${release_archive_path}"

  release_root="$(find "${extract_tmp}" -mindepth 1 -maxdepth 1 -type d -print -quit)"
  [[ -x "${release_root}/bin/ts" ]] || die "release archive is missing bin/ts"
  [[ -d "${release_root}/share/teamspeak-cli" ]] || die "release archive is missing share/teamspeak-cli"
  [[ -f "${release_root}/plugins/ts3cli_plugin.so" ]] || die "release archive is missing plugins/ts3cli_plugin.so"

  rm -rf "${extract_parent}"
  mv "${extract_tmp}" "${extract_parent}"
  release_root="${extract_parent}/$(basename "${release_root}")"
}

install_release_tree() {
  log "installing ts into ${prefix}"
  install -d "${prefix}/bin" "${share_dir}"
  install -m 0755 "${release_root}/bin/ts" "${ts_bin_path}"
  cp -a "${release_root}/share/teamspeak-cli/." "${share_dir}/"
  write_install_marker "${share_dir}" "share-dir"

  log "installing TeamSpeak client bundle into ${client_install_dir}"
  stage_client_install "${release_root}/plugins/ts3cli_plugin.so"
  install_client_wrapper
  install_uninstaller
}

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --repo)
      [[ "$#" -ge 2 ]] || die "--repo requires a value"
      release_repo="$2"
      shift 2
      ;;
    --release-tag)
      [[ "$#" -ge 2 ]] || die "--release-tag requires a value"
      release_tag="$2"
      shift 2
      ;;
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
resolve_release_tag
if [[ "${local_support_available}" -ne 1 && -z "${INSTALL_SCRIPT_BASE_URL:-}" && -z "${INSTALL_SCRIPT_REF:-}" ]]; then
  support_source_ref="${release_tag}"
  set_support_base_url
fi
resolve_support_files
# shellcheck source=tests/e2e/runtime_common.sh
source "${runtime_common_path}"
download_release_archive

log "bootstrapping TeamSpeak client bundle into ${managed_dir}"
client_source_dir="$(ts3_runtime_resolve_client_source_dir)"
ts3_runtime_resolve_client_runtime_library_path "${client_source_dir}"
if declare -F ts3_runtime_resolve_xvfb >/dev/null; then
  ts3_runtime_resolve_xvfb >/dev/null
else
  xvfb_bin="$(command -v Xvfb || true)"
  [[ -n "${xvfb_bin}" ]] || die "Xvfb is required for headless operation, but this release's runtime support cannot bootstrap it automatically"
fi
if declare -F ts3_runtime_prepare_pulseaudio_runtime >/dev/null; then
  ts3_runtime_prepare_pulseaudio_runtime
fi
write_install_marker "${managed_dir}" "managed-dir"

extract_release_archive
install_release_tree

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

Release repo:         ${release_repo}
Release tag:          ${release_tag}
ts binary:            ${ts_bin_path}
TeamSpeak launcher:   ${ts3client_wrapper_path}
Uninstaller:          ${uninstall_bin_path}
Client bundle:        ${client_install_dir}
Plugin library:       ${client_install_dir}/plugins/ts3cli_plugin.so
Xvfb:                 ${xvfb_bin}
Config path:          ${config_path}

If ${prefix}/bin is not already on your PATH, add:
  export PATH="${prefix}/bin:\$PATH"

Next steps:
  1. Start the client with: ${ts3client_wrapper_path}
  2. Enable the ts3cli plugin in TeamSpeak if it is not already enabled
  3. Verify the bridge with: ${ts_bin_path} plugin info
EOF
