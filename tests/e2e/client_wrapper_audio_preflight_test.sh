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
printf 'CLIENT_DIR=%s\n' "${TS3_CLIENT_DIR:-}"
printf 'LD_LIBRARY_PATH=%s\n' "${LD_LIBRARY_PATH:-}"
printf 'ARGS=%s\n' "$*"
EOF
chmod +x "${client_dir}/ts3client_runscript.sh"

cat >"${fake_bin_dir}/pactl" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

fixture="${PACTL_FIXTURE:-missing}"
command_name="${1:-}"
list_name="${2:-}"

[[ "${command_name}" == "list" ]] || exit 0

case "${fixture}:${list_name}" in
  missing:sinks)
    cat <<'OUT'
Sink #42
    State: RUNNING
    Name: custom.virtual_sink
OUT
    ;;
  missing:sources)
    cat <<'OUT'
Source #24
    State: SUSPENDED
    Name: custom.virtual_source
    Description: Custom virtual source
OUT
    ;;
  complete:sinks)
    cat <<'OUT'
Sink #42
    State: RUNNING
    Name: custom.virtual_sink
    Description: Custom virtual sink
OUT
    ;;
  complete:sources)
    cat <<'OUT'
Source #24
    State: SUSPENDED
    Name: custom.virtual_source
    Description: Custom virtual source
OUT
    ;;
esac
EOF
chmod +x "${fake_bin_dir}/pactl"

"${repo_root}/scripts/write-client-wrapper.sh" "${wrapper_path}" "${client_dir}"

missing_output="$(
  PATH="${fake_bin_dir}:${PATH}" PACTL_FIXTURE=missing "${wrapper_path}" --demo 2>&1
)"
expect_fragment "${missing_output}" "RUNSCRIPT_OK" "wrapper should still launch the client runscript"
expect_fragment "${missing_output}" "CLIENT_DIR=${client_dir}" "wrapper should export TS3_CLIENT_DIR"
expect_fragment "${missing_output}" "ARGS=--demo" "wrapper should preserve launch arguments"
expect_fragment "${missing_output}" "[ts3client] detected PulseAudio/PipeWire audio nodes without a Description field." \
  "wrapper should warn when a sink is missing a description"
expect_fragment "${missing_output}" "[ts3client] suspect sink#42 name=custom.virtual_sink" \
  "wrapper should identify the missing-description sink"

complete_output="$(
  PATH="${fake_bin_dir}:${PATH}" PACTL_FIXTURE=complete "${wrapper_path}" --demo 2>&1
)"
expect_fragment "${complete_output}" "RUNSCRIPT_OK" "wrapper should launch cleanly when descriptions are present"
expect_not_fragment "${complete_output}" "[ts3client] detected PulseAudio/PipeWire audio nodes without a Description field." \
  "wrapper should stay quiet when sink and source descriptions are present"
