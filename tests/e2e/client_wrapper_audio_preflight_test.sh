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

expect_eq() {
  local actual="$1"
  local expected="$2"
  local message="$3"
  if [[ "${actual}" != "${expected}" ]]; then
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
printf 'PULSE_SINK=%s\n' "${PULSE_SINK:-}"
printf 'PULSE_SOURCE=%s\n' "${PULSE_SOURCE:-}"
printf 'ARGS=%s\n' "$*"
EOF
chmod +x "${client_dir}/ts3client_runscript.sh"

cat >"${fake_bin_dir}/pactl" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

fixture="${PACTL_FIXTURE:-missing}"
command_name="${1:-}"
list_name="${2:-}"
log_path="${PACTL_LOG_PATH:-}"

case "${command_name}" in
  info)
    case "${fixture}" in
      missing)
        cat <<'OUT'
Default Sink: custom.virtual_sink
Default Source: custom.virtual_source
OUT
        ;;
      complete)
        cat <<'OUT'
Default Sink: custom.virtual_sink
Default Source: custom.virtual_source
OUT
        ;;
      unsafe)
        cat <<'OUT'
Default Sink: auto_null
Default Source: auto_null.monitor
OUT
        ;;
    esac
    exit 0
    ;;
  load-module)
    [[ -n "${log_path}" ]] && printf '%s\n' "$*" >>"${log_path}"
    printf '42\n'
    exit 0
    ;;
esac

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
  unsafe:sinks)
    cat <<'OUT'
Sink #1
    State: IDLE
    Name: auto_null
    Description: Dummy Output
OUT
    ;;
  unsafe:sources)
    cat <<'OUT'
Source #2
    State: IDLE
    Name: auto_null.monitor
    Description: Monitor of Dummy Output
    Monitor of Sink: auto_null
OUT
    ;;
esac
EOF
chmod +x "${fake_bin_dir}/pactl"

"${repo_root}/scripts/write-client-wrapper.sh" "${wrapper_path}" "${client_dir}"
pactl_log_path="${tmp_dir}/pactl.log"
: >"${pactl_log_path}"

missing_output="$(
  PATH="${fake_bin_dir}:${PATH}" PACTL_FIXTURE=missing PACTL_LOG_PATH="${pactl_log_path}" "${wrapper_path}" --demo 2>&1
)"
expect_fragment "${missing_output}" "RUNSCRIPT_OK" "wrapper should still launch the client runscript"
expect_fragment "${missing_output}" "CLIENT_DIR=${client_dir}" "wrapper should export TS3_CLIENT_DIR"
expect_fragment "${missing_output}" "ARGS=--demo" "wrapper should preserve launch arguments"
expect_fragment "${missing_output}" "[ts3client] detected PulseAudio/PipeWire audio nodes without a Description field." \
  "wrapper should warn when a sink is missing a description"
expect_fragment "${missing_output}" "[ts3client] suspect sink#42 name=custom.virtual_sink" \
  "wrapper should identify the missing-description sink"

complete_output="$(
  PATH="${fake_bin_dir}:${PATH}" PACTL_FIXTURE=complete PACTL_LOG_PATH="${pactl_log_path}" "${wrapper_path}" --demo 2>&1
)"
expect_fragment "${complete_output}" "RUNSCRIPT_OK" "wrapper should launch cleanly when descriptions are present"
expect_not_fragment "${complete_output}" "[ts3client] detected PulseAudio/PipeWire audio nodes without a Description field." \
  "wrapper should stay quiet when sink and source descriptions are present"
expect_fragment "${complete_output}" "PULSE_SINK=" "wrapper should leave the playback override unset when defaults are safe"
expect_fragment "${complete_output}" "PULSE_SOURCE=" "wrapper should leave the capture override unset when defaults are safe"

: >"${pactl_log_path}"
unsafe_output="$(
  PATH="${fake_bin_dir}:${PATH}" PACTL_FIXTURE=unsafe PACTL_LOG_PATH="${pactl_log_path}" "${wrapper_path}" --demo 2>&1
)"
expect_fragment "${unsafe_output}" "[ts3client] detected audio defaults: sink=auto_null source=auto_null.monitor." \
  "wrapper should report the detected default audio topology"
expect_fragment "${unsafe_output}" "[ts3client] unsafe audio routing detected: source auto_null.monitor monitors sink auto_null." \
  "wrapper should detect when capture points at the playback monitor"
expect_fragment "${unsafe_output}" "PULSE_SINK=teamspeak_cli.playback" \
  "wrapper should launch TeamSpeak against the provisioned playback sink"
expect_fragment "${unsafe_output}" "PULSE_SOURCE=teamspeak_cli.capture.monitor" \
  "wrapper should launch TeamSpeak against the isolated capture source"

unsafe_pactl_log="$(<"${pactl_log_path}")"
expect_fragment "${unsafe_pactl_log}" "load-module module-null-sink sink_name=teamspeak_cli.playback" \
  "wrapper should provision a dedicated virtual playback sink"
expect_fragment "${unsafe_pactl_log}" "load-module module-null-sink sink_name=teamspeak_cli.capture" \
  "wrapper should provision a dedicated virtual capture sink"

: >"${pactl_log_path}"
override_output="$(
  PATH="${fake_bin_dir}:${PATH}" \
    PACTL_FIXTURE=unsafe \
    PACTL_LOG_PATH="${pactl_log_path}" \
    PULSE_SINK=user.playback \
    PULSE_SOURCE=user.capture \
    "${wrapper_path}" --demo 2>&1
)"
expect_fragment "${override_output}" "[ts3client] using caller-provided PulseAudio overrides: PULSE_SINK=user.playback PULSE_SOURCE=user.capture." \
  "wrapper should make manual PulseAudio overrides explicit"
expect_fragment "${override_output}" "PULSE_SINK=user.playback" \
  "wrapper should preserve caller-provided playback overrides"
expect_fragment "${override_output}" "PULSE_SOURCE=user.capture" \
  "wrapper should preserve caller-provided capture overrides"
expect_eq "$(<"${pactl_log_path}")" "" \
  "wrapper should not provision virtual devices when PulseAudio overrides are already set"

: >"${pactl_log_path}"
skip_routing_output="$(
  PATH="${fake_bin_dir}:${PATH}" \
    PACTL_FIXTURE=unsafe \
    PACTL_LOG_PATH="${pactl_log_path}" \
    TS3CLIENT_SKIP_AUDIO_ROUTING=1 \
    "${wrapper_path}" --demo 2>&1
)"
expect_fragment "${skip_routing_output}" "PULSE_SINK=" \
  "wrapper should keep the playback override unset when audio routing is skipped"
expect_fragment "${skip_routing_output}" "PULSE_SOURCE=" \
  "wrapper should keep the capture override unset when audio routing is skipped"
expect_eq "$(<"${pactl_log_path}")" "" \
  "wrapper should not provision virtual devices when TS3CLIENT_SKIP_AUDIO_ROUTING=1 is set"
