#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"

die() {
  printf '%s\n' "$*" >&2
  exit 1
}

expect_fragment() {
  local output="$1"
  local fragment="$2"
  local message="$3"
  if [[ "${output}" != *"${fragment}"* ]]; then
    die "${message}"
  fi
}

expect_not_fragment() {
  local output="$1"
  local fragment="$2"
  local message="$3"
  if [[ "${output}" == *"${fragment}"* ]]; then
    die "${message}"
  fi
}

tmp_dir="$(mktemp -d)"
trap 'rm -rf "${tmp_dir}"' EXIT

client_dir="${tmp_dir}/client"
wrapper_path="${tmp_dir}/ts3client"
fake_bin_dir="${tmp_dir}/bin"
mkdir -p "${client_dir}" "${fake_bin_dir}"

cat >"${client_dir}/ts3client_runscript.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

printf 'RUNSCRIPT_OK\n'
printf 'LD_LIBRARY_PATH=%s\n' "${LD_LIBRARY_PATH:-}"
printf 'ARGS=%s\n' "$*"
EOF
chmod +x "${client_dir}/ts3client_runscript.sh"

cat >"${fake_bin_dir}/ldconfig" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

printf '0 libs found in cache `/etc/ld.so.cache`\n'
if [[ "${LDCONFIG_FIXTURE:-missing}" == "present" ]]; then
  printf '\tlibXi.so.6 (libc6,x86-64) => /usr/lib/libXi.so.6\n'
fi
EOF
chmod +x "${fake_bin_dir}/ldconfig"

"${repo_root}/scripts/write-client-wrapper.sh" "${wrapper_path}" "${client_dir}"

set +e
missing_output="$(
  PATH="${fake_bin_dir}:${PATH}" LDCONFIG_FIXTURE=missing "${wrapper_path}" --demo 2>&1
)"
missing_status=$?
set -e
[[ "${missing_status}" -ne 0 ]] || die "wrapper should fail when libXi.so.6 is unavailable"
expect_fragment "${missing_output}" "[ts3client] required shared library missing: libXi.so.6" \
  "wrapper should explain which shared library is missing"
expect_fragment "${missing_output}" "runtime-libs" \
  "wrapper should explain where to add the missing runtime library"
expect_not_fragment "${missing_output}" "RUNSCRIPT_OK" \
  "wrapper should stop before launching the TeamSpeak runscript"

mkdir -p "${client_dir}/runtime-libs/usr/lib"
: >"${client_dir}/runtime-libs/usr/lib/libXi.so.6"

local_runtime_output="$(
  PATH="${fake_bin_dir}:${PATH}" LDCONFIG_FIXTURE=missing "${wrapper_path}" --demo 2>&1
)"
expect_fragment "${local_runtime_output}" "RUNSCRIPT_OK" \
  "wrapper should launch when libXi.so.6 is bundled under runtime-libs"
expect_fragment "${local_runtime_output}" "ARGS=--demo" \
  "wrapper should preserve client launch arguments when the runtime library is present"
expect_not_fragment "${local_runtime_output}" "required shared library missing" \
  "wrapper should stay quiet once libXi.so.6 is available in runtime-libs"
