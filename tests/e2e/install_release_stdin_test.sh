#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"

die() {
  printf '%s\n' "$*" >&2
  exit 1
}

expect_file() {
  local path="$1"
  local message="$2"
  [[ -e "${path}" ]] || die "${message}"
}

expect_fragment() {
  local text="$1"
  local fragment="$2"
  local message="$3"
  [[ "${text}" == *"${fragment}"* ]] || die "${message}"
}

tmp_dir="$(mktemp -d)"
cleanup() {
  rm -rf "${tmp_dir}"
}
trap cleanup EXIT

home_dir="${tmp_dir}/home"
prefix="${tmp_dir}/prefix"
client_dir="${tmp_dir}/client"
managed_dir="${tmp_dir}/managed"
config_path="${tmp_dir}/config.ini"
fake_client_source="${tmp_dir}/fake-client-source"
fake_xvfb_path="${tmp_dir}/fake-Xvfb"
release_tag="vstdin-test"
release_archive_dir="${tmp_dir}/archive/ts-${release_tag}-linux-x86_64"
release_cache_dir="${managed_dir}/releases/${release_tag}"
release_archive_path="${release_cache_dir}/ts-${release_tag}-linux-x86_64.tar.gz"
release_checksum_path="${release_archive_path}.sha256"

mkdir -p "${home_dir}" "${fake_client_source}" "${release_archive_dir}/bin" "${release_archive_dir}/plugins" "${release_archive_dir}/share/teamspeak-cli" "${release_cache_dir}"

cat >"${fake_client_source}/ts3client_runscript.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

printf 'FAKE_CLIENT_RUNSCRIPT\n'
printf 'ARGS=%s\n' "$*"
EOF
chmod +x "${fake_client_source}/ts3client_runscript.sh"

cat >"${fake_xvfb_path}" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "-help" ]]; then
  printf 'fake Xvfb help\n'
  exit 0
fi

exit 1
EOF
chmod +x "${fake_xvfb_path}"

cat >"${release_archive_dir}/bin/ts" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "config" && "${2:-}" == "init" && "${3:-}" == "--config" && -n "${4:-}" ]]; then
  mkdir -p "$(dirname "${4}")"
  printf '[profiles]\nactive=plugin-local\n' >"${4}"
  exit 0
fi

printf 'fake ts invoked: %s\n' "$*" >&2
exit 1
EOF
chmod +x "${release_archive_dir}/bin/ts"

printf 'fake plugin\n' >"${release_archive_dir}/plugins/ts3cli_plugin.so"
tar -C "${tmp_dir}/archive" -czf "${release_archive_path}" "ts-${release_tag}-linux-x86_64"

if command -v sha256sum >/dev/null 2>&1; then
  (
    cd "${release_cache_dir}"
    sha256sum "$(basename "${release_archive_path}")" >"$(basename "${release_checksum_path}")"
  )
elif command -v shasum >/dev/null 2>&1; then
  (
    cd "${release_cache_dir}"
    shasum -a 256 "$(basename "${release_archive_path}")" >"$(basename "${release_checksum_path}")"
  )
else
  die "sha256sum or shasum is required for the stdin install regression test"
fi

install_log="${tmp_dir}/install.log"
cat "${repo_root}/scripts/install-release.sh" | env \
  HOME="${home_dir}" \
	  INSTALL_SCRIPT_BASE_URL="file://${repo_root}" \
	  TS3_CLIENT_DIR="${fake_client_source}" \
	  TS3_XVFB="${fake_xvfb_path}" \
	  bash -s -- \
    --release-tag "${release_tag}" \
    --prefix "${prefix}" \
    --client-dir "${client_dir}" \
    --managed-dir "${managed_dir}" \
    --config-path "${config_path}" >"${install_log}" 2>&1

expect_file "${prefix}/bin/ts3client" "stdin install should create the wrapper launcher"
expect_file "${prefix}/bin/ts-uninstall" "stdin install should install the uninstaller"
expect_file "${client_dir}/plugins/ts3cli_plugin.so" "stdin install should stage the plugin library"
expect_file "${config_path}" "stdin install should initialize the config"
expect_fragment "$(cat "${prefix}/share/teamspeak-cli/install-receipt.env")" "xvfb_bin_path=${fake_xvfb_path}" \
  "stdin install should record the resolved Xvfb dependency"

wrapper_output="$("${prefix}/bin/ts3client" --demo 2>&1)"
expect_fragment "${wrapper_output}" "FAKE_CLIENT_RUNSCRIPT" "installed wrapper should launch the staged client runscript"
expect_fragment "${wrapper_output}" "ARGS=--demo" "installed wrapper should preserve wrapper arguments"
