#!/usr/bin/env bash

ts3_runtime_script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ts3_runtime_repo_root="$(cd "${ts3_runtime_script_dir}/../.." && pwd)"
ts3_runtime_managed_dir_default="${TS3_MANAGED_DIR:-${ts3_runtime_repo_root}/third_party/teamspeak/managed}"

ts3_runtime_default_client_version="3.6.2"
ts3_runtime_default_client_sha256="59f110438971a23f904a700e7dd0a811cf99d4e6b975ba3aa45962d43b006422"
ts3_runtime_default_client_url="https://files.teamspeak-services.com/releases/client/${ts3_runtime_default_client_version}/TeamSpeak3-Client-linux_amd64-${ts3_runtime_default_client_version}.run"
ts3_runtime_default_plugin_sdk_repo="https://github.com/TeamSpeak-Systems/ts3client-pluginsdk"
ts3_runtime_default_plugin_sdk_ref="master"

ts3_runtime_log() {
  printf '[ts3-runtime] %s\n' "$*" >&2
}

ts3_runtime_die() {
  printf '%s\n' "$*" >&2
  exit 1
}

ts3_runtime_require_command() {
  local name="$1"
  local message="$2"
  if ! command -v "${name}" >/dev/null 2>&1; then
    ts3_runtime_die "${message}"
  fi
}

ts3_runtime_require_docker_access() {
  local context="$1"
  local docker_error

  ts3_runtime_require_command docker "docker is required for ${context}"

  if docker info >/dev/null 2>&1; then
    return 0
  fi

  docker_error="$(docker info 2>&1 || true)"

  if [[ "${docker_error}" == *"permission denied"*"/var/run/docker.sock"* ]]; then
    ts3_runtime_die \
      "docker is installed but the current user cannot access /var/run/docker.sock for ${context}. Start a Docker daemon the user can access, or add the user to the docker group."
  fi

  if [[ "${docker_error}" == *"Cannot connect to the Docker daemon"* ]] || \
     [[ "${docker_error}" == *"Is the docker daemon running"* ]]; then
    ts3_runtime_die "docker is installed but the daemon is not reachable for ${context}. Start Docker and retry."
  fi

  ts3_runtime_die "docker preflight failed for ${context}: ${docker_error}"
}

ts3_runtime_download_file() {
  local url="$1"
  local destination="$2"
  local partial_file="${destination}.part"

  mkdir -p "$(dirname "${destination}")"

  if command -v curl >/dev/null 2>&1; then
    ts3_runtime_log "downloading ${url}"
    if ! curl \
      --fail \
      --location \
      --continue-at - \
      --retry 5 \
      --retry-delay 2 \
      --retry-all-errors \
      --connect-timeout 30 \
      --speed-limit 10240 \
      --speed-time 30 \
      --output "${partial_file}" \
      "${url}"; then
      return 1
    fi
  elif command -v wget >/dev/null 2>&1; then
    ts3_runtime_log "downloading ${url}"
    if ! wget \
      --continue \
      --tries=5 \
      --timeout=30 \
      --read-timeout=30 \
      -O "${partial_file}" \
      "${url}"; then
      return 1
    fi
  else
    ts3_runtime_die "curl or wget is required to download ${url}"
  fi

  mv "${partial_file}" "${destination}"
}

ts3_runtime_sha256_file() {
  local file_path="$1"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "${file_path}" | awk '{print $1}'
    return 0
  fi
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "${file_path}" | awk '{print $1}'
    return 0
  fi
  ts3_runtime_die "sha256sum or shasum is required to verify downloaded files"
}

ts3_runtime_verify_file_sha256() {
  local expected="$1"
  local file_path="$2"
  local actual

  if [[ -z "${expected}" ]]; then
    return 0
  fi

  actual="$(ts3_runtime_sha256_file "${file_path}")"
  [[ "${actual}" == "${expected}" ]]
}

ts3_runtime_ensure_downloaded_file() {
  local url="$1"
  local destination="$2"
  local expected_sha="$3"

  if [[ -f "${destination}" ]]; then
    if ts3_runtime_verify_file_sha256 "${expected_sha}" "${destination}"; then
      return 0
    fi
    ts3_runtime_log "cached file checksum mismatch for ${destination}; re-downloading"
    rm -f "${destination}"
  fi

  ts3_runtime_download_file "${url}" "${destination}" || {
    ts3_runtime_die "failed to download ${url}"
  }

  if ! ts3_runtime_verify_file_sha256 "${expected_sha}" "${destination}"; then
    rm -f "${destination}"
    ts3_runtime_die "checksum mismatch for ${destination}"
  fi
}

ts3_runtime_validate_client_dir() {
  local candidate_dir="$1"
  [[ -x "${candidate_dir}/ts3client_runscript.sh" ]]
}

ts3_runtime_validate_plugin_sdk_dir() {
  local candidate_dir="$1"
  [[ -f "${candidate_dir}/include/plugin_definitions.h" ]]
}

ts3_runtime_client_ld_library_path() {
  local client_dir="$1"
  local runtime_library_path="${2:-}"

  if [[ -n "${runtime_library_path}" ]]; then
    printf '%s:%s\n' "${client_dir}" "${runtime_library_path}"
    return 0
  fi
  printf '%s\n' "${client_dir}"
}

ts3_runtime_missing_client_shared_libraries() {
  local client_dir="$1"
  local runtime_library_path="${2:-}"
  local launch_library_path
  local binary_path

  launch_library_path="$(ts3_runtime_client_ld_library_path "${client_dir}" "${runtime_library_path}")"

  for binary_path in \
    "${client_dir}/ts3client_linux_amd64" \
    "${client_dir}/QtWebEngineProcess" \
    "${client_dir}/platforms/libqxcb.so" \
    "${client_dir}/xcbglintegrations/libqxcb-egl-integration.so" \
    "${client_dir}/xcbglintegrations/libqxcb-glx-integration.so"; do
    [[ -f "${binary_path}" ]] || continue
    env LD_LIBRARY_PATH="${launch_library_path}" ldd "${binary_path}" 2>/dev/null
  done | awk '/=> not found/ {print $1}' | sort -u
}

ts3_runtime_client_runtime_libraries_ready() {
  local client_dir="$1"
  local runtime_library_path="${2:-}"

  [[ -n "${runtime_library_path}" ]] || return 1
  [[ -d "${runtime_library_path}" ]] || return 1
  [[ ! -n "$(ts3_runtime_missing_client_shared_libraries "${client_dir}" "${runtime_library_path}")" ]]
}

ts3_runtime_client_package_for_soname() {
  local soname="$1"

  case "${soname}" in
    libevent-2.1.so.7)
      printf '%s\n' "libevent-2.1-7"
      ;;
    libxslt.so.1)
      printf '%s\n' "libxslt1.1"
      ;;
    libxcb-icccm.so.4)
      printf '%s\n' "libxcb-icccm4"
      ;;
    libxcb-image.so.0)
      printf '%s\n' "libxcb-image0"
      ;;
    libxcb-keysyms.so.1)
      printf '%s\n' "libxcb-keysyms1"
      ;;
    libxcb-render-util.so.0)
      printf '%s\n' "libxcb-render-util0"
      ;;
    libxcb-util.so.1)
      printf '%s\n' "libxcb-util1"
      ;;
    libxcb-xinerama.so.0)
      printf '%s\n' "libxcb-xinerama0"
      ;;
    libxcb-xkb.so.1)
      printf '%s\n' "libxcb-xkb1"
      ;;
    libxkbcommon-x11.so.0)
      printf '%s\n' "libxkbcommon-x11-0"
      ;;
    *)
      return 1
      ;;
  esac
}

ts3_runtime_bootstrap_client_runtime_libraries_from_apt() {
  local client_dir="$1"
  local cache_dir="${ts3_runtime_managed_dir_default}/client-runtime-libs"
  local package_dir="${cache_dir}/debs"
  local extract_root="${cache_dir}/root"
  local runtime_library_path=""
  local missing_libraries
  local package_name
  local packages=()
  local package_string=""
  local deb_file
  local pass=0
  local max_passes=6

  ts3_runtime_require_command apt-get "missing TeamSpeak client runtime libraries require apt-get on this host"
  ts3_runtime_require_command dpkg-deb "missing TeamSpeak client runtime libraries require dpkg-deb on this host"

  if [[ -d "${extract_root}" ]]; then
    runtime_library_path="$(find "${extract_root}" -type f -name '*.so*' -printf '%h\n' 2>/dev/null | sort -u | paste -sd ':' -)"
    if ts3_runtime_client_runtime_libraries_ready "${client_dir}" "${runtime_library_path}"; then
      client_runtime_library_path="${runtime_library_path}"
      return 0
    fi
  fi

  mkdir -p "${package_dir}"
  mkdir -p "${extract_root}"

  while (( pass < max_passes )); do
    runtime_library_path="$(find "${extract_root}" -type f -name '*.so*' -printf '%h\n' 2>/dev/null | sort -u | paste -sd ':' -)"
    missing_libraries="$(ts3_runtime_missing_client_shared_libraries "${client_dir}" "${runtime_library_path}")"
    if [[ -z "${missing_libraries}" ]]; then
      client_runtime_library_path="${runtime_library_path}"
      return 0
    fi

    packages=()
    while IFS= read -r soname; do
      [[ -n "${soname}" ]] || continue
      package_name="$(ts3_runtime_client_package_for_soname "${soname}")" || {
        ts3_runtime_die "unsupported TeamSpeak client runtime dependency: ${soname}"
      }
      if [[ " ${package_string} " != *" ${package_name} "* ]]; then
        packages+=("${package_name}")
        package_string+=" ${package_name}"
      fi
    done <<<"${missing_libraries}"

    if [[ "${#packages[@]}" -eq 0 ]]; then
      break
    fi

    ts3_runtime_log "downloading TeamSpeak client runtime packages: ${packages[*]}"
    (
      cd "${package_dir}"
      apt-get download "${packages[@]}" >/dev/null
    ) || ts3_runtime_die "failed to download TeamSpeak client runtime packages via apt-get"

    for deb_file in "${package_dir}"/*.deb; do
      [[ -f "${deb_file}" ]] || continue
      dpkg-deb -x "${deb_file}" "${extract_root}"
    done

    pass=$((pass + 1))
  done

  runtime_library_path="$(find "${extract_root}" -type f -name '*.so*' -printf '%h\n' 2>/dev/null | sort -u | paste -sd ':' -)"
  if ! ts3_runtime_client_runtime_libraries_ready "${client_dir}" "${runtime_library_path}"; then
    ts3_runtime_die "bootstrapped TeamSpeak client runtime libraries are still incomplete"
  fi

  client_runtime_library_path="${runtime_library_path}"
}

ts3_runtime_resolve_client_runtime_library_path() {
  local client_dir="$1"
  local explicit_path="${TS3_CLIENT_LIBRARY_PATH:-}"

  client_runtime_library_path=""

  if [[ -n "${explicit_path}" ]]; then
    if ts3_runtime_client_runtime_libraries_ready "${client_dir}" "${explicit_path}"; then
      client_runtime_library_path="${explicit_path}"
      return 0
    fi
    if [[ "${explicit_path}" != "${ts3_runtime_managed_dir_default}"* ]]; then
      ts3_runtime_die "TeamSpeak client runtime library path is incomplete: ${explicit_path}"
    fi
    ts3_runtime_log "cached TeamSpeak client runtime libraries missing or incomplete at ${explicit_path}; bootstrapping again"
  fi

  if [[ -n "$(ts3_runtime_missing_client_shared_libraries "${client_dir}")" ]]; then
    ts3_runtime_bootstrap_client_runtime_libraries_from_apt "${client_dir}"
    return 0
  fi
}

ts3_runtime_resolve_client_source_dir() {
  local explicit_dir="${TS3_CLIENT_DIR:-}"
  local version="${TS3_CLIENT_VERSION:-${ts3_runtime_default_client_version}}"
  local archive_url="${TS3_CLIENT_URL:-}"
  local archive_sha256="${TS3_CLIENT_SHA256:-}"
  local cache_dir="${ts3_runtime_managed_dir_default}"
  local install_label="${version}"
  local install_dir
  local archive_name
  local installer_path
  local temp_dir

  if [[ -n "${explicit_dir}" ]]; then
    if ts3_runtime_validate_client_dir "${explicit_dir}"; then
      printf '%s\n' "${explicit_dir}"
      return 0
    fi
    if [[ "${explicit_dir}" != "${ts3_runtime_managed_dir_default}"* ]]; then
      ts3_runtime_die "TeamSpeak client runscript not found under ${explicit_dir}"
    fi
    ts3_runtime_log "cached TeamSpeak client bundle missing at ${explicit_dir}; bootstrapping again"
  fi

  if [[ -z "${archive_url}" ]]; then
    archive_url="https://files.teamspeak-services.com/releases/client/${version}/TeamSpeak3-Client-linux_amd64-${version}.run"
  fi

  if [[ -z "${archive_sha256}" && "${archive_url}" == "${ts3_runtime_default_client_url}" ]]; then
    archive_sha256="${ts3_runtime_default_client_sha256}"
  fi

  if [[ -n "${TS3_CLIENT_URL:-}" && -z "${TS3_CLIENT_VERSION:-}" ]]; then
    install_label="$(basename "${archive_url%%\?*}" .run)"
  fi

  install_dir="${cache_dir}/client/${install_label}"
  if ts3_runtime_validate_client_dir "${install_dir}"; then
    printf '%s\n' "${install_dir}"
    return 0
  fi

  archive_name="$(basename "${archive_url%%\?*}")"
  installer_path="${cache_dir}/downloads/${archive_name}"
  ts3_runtime_ensure_downloaded_file "${archive_url}" "${installer_path}" "${archive_sha256}"

  chmod +x "${installer_path}"

  temp_dir="${install_dir}.tmp.$$"
  rm -rf "${temp_dir}"
  mkdir -p "${temp_dir}"
  ts3_runtime_log "extracting TeamSpeak client archive to ${install_dir}"
  if ! "${installer_path}" --noexec --accept --target "${temp_dir}" >/dev/null; then
    rm -rf "${temp_dir}"
    ts3_runtime_die "failed to extract ${installer_path}; try setting TS3_CLIENT_DIR to an existing client bundle"
  fi

  if ! ts3_runtime_validate_client_dir "${temp_dir}"; then
    rm -rf "${temp_dir}"
    ts3_runtime_die "extracted TeamSpeak client bundle is missing ts3client_runscript.sh"
  fi

  rm -rf "${install_dir}"
  mv "${temp_dir}" "${install_dir}"
  printf '%s\n' "${install_dir}"
}

ts3_runtime_resolve_plugin_sdk_dir() {
  local explicit_dir="${TS3_PLUGIN_SDK_DIR:-}"
  local explicit_include_dir="${TS3_PLUGIN_SDK_INCLUDE_DIR:-}"
  local repo_url="${TS3_PLUGIN_SDK_URL:-${ts3_runtime_default_plugin_sdk_repo}}"
  local repo_ref="${TS3_PLUGIN_SDK_REF:-${ts3_runtime_default_plugin_sdk_ref}}"
  local cache_dir="${ts3_runtime_managed_dir_default}"
  local install_dir="${cache_dir}/ts3client-pluginsdk"
  local temp_dir
  local archive_url
  local archive_name
  local archive_path

  if [[ -n "${explicit_dir}" ]]; then
    if ts3_runtime_validate_plugin_sdk_dir "${explicit_dir}"; then
      printf '%s\n' "${explicit_dir}"
      return 0
    fi
    if [[ "${explicit_dir}" != "${ts3_runtime_managed_dir_default}"* ]]; then
      ts3_runtime_die "TeamSpeak plugin SDK headers not found under ${explicit_dir}"
    fi
    ts3_runtime_log "cached TeamSpeak plugin SDK missing at ${explicit_dir}; bootstrapping again"
  fi

  if [[ -n "${explicit_include_dir}" ]]; then
    if [[ -f "${explicit_include_dir}/plugin_definitions.h" ]]; then
      printf '%s\n' "$(cd "${explicit_include_dir}/.." && pwd)"
      return 0
    fi
    if [[ "${explicit_include_dir}" != "${ts3_runtime_managed_dir_default}"* ]]; then
      ts3_runtime_die "TeamSpeak plugin SDK header not found under ${explicit_include_dir}"
    fi
    ts3_runtime_log "cached TeamSpeak plugin SDK include dir missing at ${explicit_include_dir}; bootstrapping again"
  fi

  if ts3_runtime_validate_plugin_sdk_dir "${install_dir}"; then
    printf '%s\n' "${install_dir}"
    return 0
  fi

  temp_dir="${install_dir}.tmp.$$"
  rm -rf "${temp_dir}"

  if command -v git >/dev/null 2>&1; then
    ts3_runtime_log "cloning TeamSpeak plugin SDK from ${repo_url} (${repo_ref})"
    git clone --depth 1 --branch "${repo_ref}" "${repo_url}" "${temp_dir}" >/dev/null 2>&1 || {
      rm -rf "${temp_dir}"
      ts3_runtime_die "failed to clone TeamSpeak plugin SDK from ${repo_url}"
    }
  else
    archive_url="${repo_url%/}"
    archive_url="${archive_url%.git}/archive/refs/heads/${repo_ref}.tar.gz"
    archive_name="$(basename "${archive_url%%\?*}")"
    archive_path="${cache_dir}/downloads/${archive_name}"
    ts3_runtime_require_command tar "tar is required to extract the TeamSpeak plugin SDK archive"
    ts3_runtime_ensure_downloaded_file "${archive_url}" "${archive_path}" ""
    mkdir -p "${temp_dir}"
    ts3_runtime_log "extracting TeamSpeak plugin SDK archive to ${install_dir}"
    tar -xzf "${archive_path}" -C "${temp_dir}" --strip-components=1 || {
      rm -rf "${temp_dir}"
      ts3_runtime_die "failed to extract TeamSpeak plugin SDK archive ${archive_path}"
    }
  fi

  if ! ts3_runtime_validate_plugin_sdk_dir "${temp_dir}"; then
    rm -rf "${temp_dir}"
    ts3_runtime_die "downloaded TeamSpeak plugin SDK is missing include/plugin_definitions.h"
  fi

  rm -rf "${install_dir}"
  mv "${temp_dir}" "${install_dir}"
  printf '%s\n' "${install_dir}"
}

ts3_runtime_guess_xdotool_library_path() {
  local xdotool_path="$1"
  local explicit_path="${TS3_XDOTOOL_LIBRARY_PATH:-}"
  local candidate_root
  local library_file

  if [[ -n "${explicit_path}" ]]; then
    printf '%s\n' "${explicit_path}"
    return 0
  fi

  candidate_root="$(cd "$(dirname "${xdotool_path}")/.." && pwd)"
  library_file="$(find "${candidate_root}" -type f -name 'libxdo.so*' 2>/dev/null | head -n 1 || true)"
  if [[ -n "${library_file}" ]]; then
    dirname "${library_file}"
  fi
}

ts3_runtime_xdotool_works() {
  local xdotool_path="$1"
  local library_path="$2"

  if [[ -z "${library_path}" ]]; then
    "${xdotool_path}" --version >/dev/null 2>&1
    return
  fi

  LD_LIBRARY_PATH="${library_path}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
    "${xdotool_path}" --version >/dev/null 2>&1
}

ts3_runtime_bootstrap_xdotool_from_apt() {
  local cache_dir="${ts3_runtime_managed_dir_default}/xdotool"
  local package_dir="${cache_dir}/debs"
  local extract_root="${cache_dir}/root"
  local xdotool_candidate="${extract_root}/usr/bin/xdotool"

  ts3_runtime_require_command apt-get "xdotool is missing and automatic bootstrap requires apt-get on this host"
  ts3_runtime_require_command dpkg-deb "xdotool is missing and automatic bootstrap requires dpkg-deb on this host"

  if [[ -x "${xdotool_candidate}" ]]; then
    xdotool_bin="${xdotool_candidate}"
    xdotool_library_path="$(ts3_runtime_guess_xdotool_library_path "${xdotool_bin}")"
    if ts3_runtime_xdotool_works "${xdotool_bin}" "${xdotool_library_path}"; then
      return 0
    fi
  fi

  mkdir -p "${package_dir}"
  rm -rf "${extract_root}"
  mkdir -p "${extract_root}"

  ts3_runtime_log "downloading xdotool and libxdo3 Debian packages"
  (
    cd "${package_dir}"
    apt-get download xdotool libxdo3 >/dev/null
  ) || ts3_runtime_die "failed to download xdotool packages via apt-get"

  local deb_file
  for deb_file in "${package_dir}"/xdotool_*.deb "${package_dir}"/libxdo3_*.deb; do
    if [[ ! -f "${deb_file}" ]]; then
      ts3_runtime_die "failed to locate downloaded package ${deb_file}"
    fi
    dpkg-deb -x "${deb_file}" "${extract_root}"
  done

  xdotool_bin="${xdotool_candidate}"
  xdotool_library_path="$(ts3_runtime_guess_xdotool_library_path "${xdotool_bin}")"
  if [[ ! -x "${xdotool_bin}" ]]; then
    ts3_runtime_die "bootstrapped xdotool binary was not found at ${xdotool_bin}"
  fi
  if ! ts3_runtime_xdotool_works "${xdotool_bin}" "${xdotool_library_path}"; then
    ts3_runtime_die "bootstrapped xdotool could not be executed"
  fi
}

ts3_runtime_resolve_xdotool() {
  local explicit_bin="${TS3_XDOTOOL:-}"
  local discovered_bin=""

  xdotool_library_path=""

  if [[ -n "${explicit_bin}" ]]; then
    if [[ -x "${explicit_bin}" ]]; then
      xdotool_bin="${explicit_bin}"
      xdotool_library_path="$(ts3_runtime_guess_xdotool_library_path "${xdotool_bin}")"
      if ts3_runtime_xdotool_works "${xdotool_bin}" "${xdotool_library_path}"; then
        return 0
      fi
    fi
    if [[ "${explicit_bin}" != "${ts3_runtime_managed_dir_default}"* ]]; then
      ts3_runtime_die "xdotool failed to execute: ${explicit_bin}"
    fi
    ts3_runtime_log "cached xdotool missing or unusable at ${explicit_bin}; bootstrapping again"
  fi

  discovered_bin="$(command -v xdotool || true)"
  if [[ -n "${discovered_bin}" ]]; then
    xdotool_bin="${discovered_bin}"
    xdotool_library_path="$(ts3_runtime_guess_xdotool_library_path "${xdotool_bin}")"
    if ts3_runtime_xdotool_works "${xdotool_bin}" "${xdotool_library_path}"; then
      return 0
    fi
  fi

  ts3_runtime_bootstrap_xdotool_from_apt
}

ts3_runtime_json_expect_fragment() {
  local json_payload="$1"
  local fragment="$2"
  local message="$3"

  if [[ "${json_payload}" != *"${fragment}"* ]]; then
    printf '%s\n' "${message}" >&2
    return 1
  fi
}

ts3_runtime_json_expect_not_fragment() {
  local json_payload="$1"
  local fragment="$2"
  local message="$3"

  if [[ "${json_payload}" == *"${fragment}"* ]]; then
    printf '%s\n' "${message}" >&2
    return 1
  fi
}

ts3_runtime_json_expect_nonempty_array() {
  local json_payload="$1"
  local message="$2"

  if [[ "${json_payload}" == "[]" ]]; then
    printf '%s\n' "${message}" >&2
    return 1
  fi
}

ts3_runtime_json_extract_number_field() {
  local json_payload="$1"
  local field_name="$2"

  printf '%s\n' "${json_payload}" | sed -nE "s/.*\"${field_name}\":([0-9]+).*/\\1/p" | head -n 1
}

ts3_runtime_json_expect_number_ge() {
  local json_payload="$1"
  local field_name="$2"
  local minimum="$3"
  local message="$4"
  local extracted

  extracted="$(ts3_runtime_json_extract_number_field "${json_payload}" "${field_name}")"
  if [[ -z "${extracted}" ]]; then
    printf '%s\n' "${message}" >&2
    return 1
  fi

  if (( extracted < minimum )); then
    printf '%s\n' "${message}" >&2
    return 1
  fi
}

ts3_runtime_json_extract_first_array_object_id_matching() {
  local json_payload="$1"
  shift

  local object_line
  local fragment
  local matches_all
  local extracted_id

  while IFS= read -r object_line; do
    [[ -n "${object_line}" ]] || continue
    matches_all=1
    for fragment in "$@"; do
      if [[ "${object_line}" != *"${fragment}"* ]]; then
        matches_all=0
        break
      fi
    done
    if [[ "${matches_all}" -ne 1 ]]; then
      continue
    fi
    extracted_id="$(printf '%s\n' "${object_line}" | sed -nE 's/.*"id":"([^"]+)".*/\1/p' | head -n 1)"
    if [[ -n "${extracted_id}" ]]; then
      printf '%s\n' "${extracted_id}"
      return 0
    fi
  done < <(printf '%s\n' "${json_payload}" | tr '{' '\n')

  return 1
}

ts3_runtime_write_default_env_files() {
  local plugin_sdk_dir="$1"
  local client_source_dir="$2"
  local client_library_path="$3"
  local xdotool_path="$4"
  local xdotool_lib_path="$5"
  local deps_mk_path="${TS3_DEPS_MK:-${ts3_runtime_managed_dir_default}/deps.mk}"
  local deps_env_path="${TS3_DEPS_ENV:-${ts3_runtime_managed_dir_default}/deps.env}"

  mkdir -p "$(dirname "${deps_mk_path}")"
  mkdir -p "$(dirname "${deps_env_path}")"

  cat >"${deps_mk_path}" <<EOF
TS3_MANAGED_DIR := ${ts3_runtime_managed_dir_default}
TS3_PLUGIN_SDK_DIR := ${plugin_sdk_dir}
TS3_CLIENT_DIR := ${client_source_dir}
TS3_CLIENT_LIBRARY_PATH := ${client_library_path}
TS3_XDOTOOL := ${xdotool_path}
TS3_XDOTOOL_LIBRARY_PATH := ${xdotool_lib_path}
EOF

  cat >"${deps_env_path}" <<EOF
export TS3_MANAGED_DIR='${ts3_runtime_managed_dir_default}'
export TS3_PLUGIN_SDK_DIR='${plugin_sdk_dir}'
export TS3_CLIENT_DIR='${client_source_dir}'
export TS3_CLIENT_LIBRARY_PATH='${client_library_path}'
export TS3_XDOTOOL='${xdotool_path}'
export TS3_XDOTOOL_LIBRARY_PATH='${xdotool_lib_path}'
EOF
}
