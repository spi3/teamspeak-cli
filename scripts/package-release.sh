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
  printf '[package-release] %s\n' "$*"
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
  if [[ -x "${HOME:-}/.local/venvs/build-tools/bin/cmake" ]]; then
    printf '%s\n' "${HOME}/.local/venvs/build-tools/bin/cmake"
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
  if [[ -x "${HOME:-}/.local/venvs/build-tools/bin/ninja" ]]; then
    printf '%s\n' "${HOME}/.local/venvs/build-tools/bin/ninja"
    return 0
  fi
  return 1
}

write_sha256_file() {
  local archive_path="$1"
  local checksum_path="$2"

  if have_command sha256sum; then
    (
      cd "$(dirname "${archive_path}")"
      sha256sum "$(basename "${archive_path}")" >"$(basename "${checksum_path}")"
    )
    return 0
  fi

  if have_command shasum; then
    (
      cd "$(dirname "${archive_path}")"
      shasum -a 256 "$(basename "${archive_path}")" >"$(basename "${checksum_path}")"
    )
    return 0
  fi

  die "missing required checksum tool: sha256sum or shasum"
}

cmake_bin="$(resolve_cmake_bin || true)"
ninja_bin="$(resolve_ninja_bin || true)"
build_dir="${BUILD_DIR:-${repo_root}/build-release}"
dist_dir="${DIST_DIR:-${repo_root}/dist}"
staging_parent="${dist_dir}/.staging"
build_type="${CMAKE_BUILD_TYPE:-Release}"
package_version="${1:-${PACKAGE_VERSION:-${GITHUB_REF_NAME:-}}}"
platform="${PLATFORM:-linux-x86_64}"

[[ -n "${cmake_bin}" ]] || die "cmake is required"

if [[ -z "${package_version}" ]]; then
  package_version="$(git -C "${repo_root}" describe --tags --always --dirty 2>/dev/null || true)"
fi
[[ -n "${package_version}" ]] || die "set PACKAGE_VERSION, GITHUB_REF_NAME, or pass a version argument"

if [[ -n "${CMAKE_GENERATOR:-}" ]]; then
  generator="${CMAKE_GENERATOR}"
elif [[ -n "${ninja_bin}" ]]; then
  generator="Ninja"
else
  generator="Unix Makefiles"
fi

archive_root="ts-${package_version}-${platform}"
staging_dir="${staging_parent}/${archive_root}"
archive_path="${dist_dir}/${archive_root}.tar.gz"
checksum_path="${archive_path}.sha256"

mkdir -p "${dist_dir}" "${staging_parent}"
rm -rf "${staging_dir}"
rm -f "${archive_path}" "${checksum_path}"

cmake_args=(
  -S "${repo_root}"
  -B "${build_dir}"
  -G "${generator}"
  -DCMAKE_BUILD_TYPE="${build_type}"
  -DBUILD_TESTING=OFF
  -DTS_ENABLE_SANITIZERS=OFF
  -DTS_ENABLE_TS3_PLUGIN=OFF
  -DTS_ENABLE_TS3_E2E=OFF
)

if [[ "${generator}" == "Ninja" ]]; then
  [[ -n "${ninja_bin}" ]] || die "generator Ninja selected but ninja was not found"
  cmake_args+=("-DCMAKE_MAKE_PROGRAM=${ninja_bin}")
fi

log "configuring ${build_type} build in ${build_dir}"
"${cmake_bin}" "${cmake_args[@]}"

log "building ts"
"${cmake_bin}" --build "${build_dir}" --parallel --target ts

log "installing release tree into ${staging_dir}"
"${cmake_bin}" --install "${build_dir}" --prefix "${staging_dir}"

if have_command strip; then
  strip "${staging_dir}/bin/ts" || true
fi

log "creating ${archive_path}"
tar -C "${staging_parent}" -czf "${archive_path}" "${archive_root}"

log "writing ${checksum_path}"
write_sha256_file "${archive_path}" "${checksum_path}"

log "created $(basename "${archive_path}")"
log "created $(basename "${checksum_path}")"
