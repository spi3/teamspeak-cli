#!/usr/bin/env bash
set -euo pipefail

wrapper_path="${1:-}"
client_dir="${2:-}"

if [[ -z "${wrapper_path}" || -z "${client_dir}" ]]; then
  printf 'usage: %s <wrapper-path> <client-dir>\n' "${0##*/}" >&2
  exit 1
fi

install -d "$(dirname "${wrapper_path}")"

client_dir_quoted=""
printf -v client_dir_quoted '%q' "${client_dir}"

wrapper_tmp="$(mktemp)"
cat >"${wrapper_tmp}" <<EOF
#!/usr/bin/env bash
set -euo pipefail

client_dir=${client_dir_quoted}
runtime_root="\${TS3_CLIENT_RUNTIME_ROOT:-\${client_dir}/runtime-libs}"
runtime_library_path="\${TS3_CLIENT_LIBRARY_PATH:-}"

have_command() {
  command -v "\$1" >/dev/null 2>&1
}

resolve_executable() {
  local executable_name="\$1"
  local explicit_path="\$2"
  local candidate=""

  if [[ -n "\${explicit_path}" && -x "\${explicit_path}" ]]; then
    printf '%s\n' "\${explicit_path}"
    return 0
  fi

  if have_command "\${executable_name}"; then
    command -v "\${executable_name}"
    return 0
  fi

  shift 2
  for candidate in "\$@"; do
    [[ -n "\${candidate}" && -x "\${candidate}" ]] || continue
    printf '%s\n' "\${candidate}"
    return 0
  done

  return 1
}

log_warning() {
  printf '[ts3client] %s\n' "\$*" >&2
}

library_search_path_contains_soname() {
  local soname="\$1"
  local search_path="\$2"
  local dir=""

  while IFS= read -r dir; do
    [[ -n "\${dir}" ]] || continue
    if [[ -e "\${dir}/\${soname}" ]]; then
      return 0
    fi
  done < <(printf '%s\n' "\${search_path}" | tr ':' '\n')

  return 1
}

ldconfig_cache_contains_soname() {
  local soname="\$1"
  local ldconfig_path="\$2"

  [[ -n "\${ldconfig_path}" ]] || return 1
  "\${ldconfig_path}" -p 2>/dev/null | awk -v soname="\${soname}" '
    \$1 == soname { found = 1; exit }
    END { exit(found ? 0 : 1) }
  '
}

require_runtime_libraries() {
  local search_path="\$1"
  local ldconfig_path=""
  local missing=()
  local soname=""

  [[ "\${TS3CLIENT_SKIP_LIBRARY_PREFLIGHT:-0}" == "1" ]] && return 0
  ldconfig_path="\$(resolve_executable ldconfig "\${TS3_CLIENT_LDCONFIG:-}" /usr/sbin/ldconfig /sbin/ldconfig || true)"

  while IFS= read -r soname; do
    [[ -n "\${soname}" ]] || continue
    if library_search_path_contains_soname "\${soname}" "\${search_path}"; then
      continue
    fi
    if ldconfig_cache_contains_soname "\${soname}" "\${ldconfig_path}"; then
      continue
    fi
    if [[ -z "\${ldconfig_path}" ]]; then
      continue
    fi
    missing+=("\${soname}")
  done <<'RUNTIME_LIBS'
libXi.so.6
RUNTIME_LIBS

  [[ "\${#missing[@]}" -eq 0 ]] && return 0

  log_warning "required shared library missing: \${missing[*]}"
  log_warning "Install a package that provides the missing library, or add it under \${runtime_root}."
  log_warning "Set TS3CLIENT_SKIP_LIBRARY_PREFLIGHT=1 only if you have already verified the TeamSpeak runtime can resolve it."
  return 127
}

emit_missing_description_entries() {
  local node_kind="\$1"
  local heading="\$2"
  local current_id=""
  local current_name=""
  local current_description=""
  local line=""
  local trimmed=""

  flush_current() {
    if [[ -n "\${current_id}" && -z "\${current_description}" ]]; then
      printf '%s#%s' "\${node_kind}" "\${current_id}"
      if [[ -n "\${current_name}" ]]; then
        printf ' name=%s' "\${current_name}"
      fi
      printf '\n'
    fi
  }

  while IFS= read -r line; do
    case "\${line}" in
      "\${heading}"\ \#*)
        flush_current
        current_id="\${line#\${heading} #}"
        current_name=""
        current_description=""
        continue
        ;;
    esac

    trimmed="\${line#"\${line%%[![:space:]]*}"}"
    case "\${trimmed}" in
      Name:*)
        current_name="\${trimmed#Name: }"
        ;;
      Description:*)
        current_description="\${trimmed#Description: }"
        ;;
    esac
  done < <(pactl list "\${node_kind}s" 2>/dev/null || true)

  flush_current
}

warn_if_audio_nodes_missing_descriptions() {
  local suspects=""
  local suspect=""

  [[ "\${TS3CLIENT_SKIP_AUDIO_PREFLIGHT:-0}" == "1" ]] && return 0
  have_command pactl || return 0

  suspects+="\$(emit_missing_description_entries sink Sink)"
  if [[ -n "\${suspects}" ]]; then
    suspects+=$'\n'
  fi
  suspects+="\$(emit_missing_description_entries source Source)"
  suspects="\${suspects#\$'\n'}"
  suspects="\${suspects%\$'\n'}"

  [[ -n "\${suspects}" ]] || return 0

  log_warning "detected PulseAudio/PipeWire audio nodes without a Description field."
  log_warning "TeamSpeak can segfault on startup when custom sinks or sources are missing device.description or node.description."
  while IFS= read -r suspect; do
    [[ -n "\${suspect}" ]] || continue
    log_warning "suspect \${suspect}"
  done <<<"\${suspects}"
  log_warning "Fix the node metadata or set TS3CLIENT_SKIP_AUDIO_PREFLIGHT=1 to suppress this warning."
}

pactl_info_value() {
  local key="\$1"
  local prefix="\${key}: "
  local line=""
  local trimmed=""

  while IFS= read -r line; do
    trimmed="\${line#"\${line%%[![:space:]]*}"}"
    case "\${trimmed}" in
      "\${prefix}"*)
        printf '%s\n' "\${trimmed#\${prefix}}"
        return 0
        ;;
    esac
  done < <(pactl info 2>/dev/null || true)

  return 1
}

audio_node_exists() {
  local node_kind="\$1"
  local heading="\$2"
  local target_name="\$3"
  local line=""
  local trimmed=""
  local current_name=""

  while IFS= read -r line; do
    case "\${line}" in
      "\${heading}"\ \#*)
        current_name=""
        continue
        ;;
    esac

    trimmed="\${line#"\${line%%[![:space:]]*}"}"
    case "\${trimmed}" in
      Name:*)
        current_name="\${trimmed#Name: }"
        if [[ "\${current_name}" == "\${target_name}" ]]; then
          return 0
        fi
        ;;
    esac
  done < <(pactl list "\${node_kind}s" 2>/dev/null || true)

  return 1
}

audio_sink_exists() {
  audio_node_exists sink Sink "\$1"
}

audio_source_monitor_sink() {
  local target_source="\$1"
  local line=""
  local trimmed=""
  local current_name=""
  local current_monitor=""

  while IFS= read -r line; do
    case "\${line}" in
      Source\ \#*)
        if [[ "\${current_name}" == "\${target_source}" ]]; then
          printf '%s\n' "\${current_monitor}"
          return 0
        fi
        current_name=""
        current_monitor=""
        continue
        ;;
    esac

    trimmed="\${line#"\${line%%[![:space:]]*}"}"
    case "\${trimmed}" in
      Name:*)
        current_name="\${trimmed#Name: }"
        ;;
      Monitor\ of\ Sink:*)
        current_monitor="\${trimmed#Monitor of Sink: }"
        ;;
    esac
  done < <(pactl list sources 2>/dev/null || true)

  if [[ "\${current_name}" == "\${target_source}" ]]; then
    printf '%s\n' "\${current_monitor}"
    return 0
  fi

  return 1
}

ensure_virtual_null_sink() {
  local sink_name="\$1"
  local description="\$2"

  if audio_sink_exists "\${sink_name}"; then
    log_warning "reusing virtual audio sink \${sink_name}."
    return 0
  fi

  if ! pactl load-module module-null-sink \
      "sink_name=\${sink_name}" \
      "sink_properties=device.description=\${description}" >/dev/null 2>&1; then
    log_warning "failed to provision virtual audio sink \${sink_name}."
    return 1
  fi

  log_warning "provisioned virtual audio sink \${sink_name}."
  return 0
}

configure_safe_virtual_audio() {
  local default_sink=""
  local default_source=""
  local source_monitor_sink=""
  local launch_sink=""
  local launch_source=""
  local playback_sink_name="teamspeak_cli.playback"
  local capture_sink_name="teamspeak_cli.capture"
  local capture_source_name="\${capture_sink_name}.monitor"

  [[ "\${TS3CLIENT_SKIP_AUDIO_PREFLIGHT:-0}" == "1" ]] && return 0
  [[ "\${TS3CLIENT_SKIP_AUDIO_ROUTING:-0}" == "1" ]] && return 0
  have_command pactl || return 0

  default_sink="\$(pactl_info_value "Default Sink" || true)"
  default_source="\$(pactl_info_value "Default Source" || true)"

  [[ -n "\${default_sink}" || -n "\${default_source}" ]] || return 0

  log_warning "detected audio defaults: sink=\${default_sink:-<unknown>} source=\${default_source:-<unknown>}."

  if [[ -n "\${PULSE_SINK:-}" || -n "\${PULSE_SOURCE:-}" ]]; then
    log_warning "using caller-provided PulseAudio overrides: PULSE_SINK=\${PULSE_SINK:-<unset>} PULSE_SOURCE=\${PULSE_SOURCE:-<unset>}."
    return 0
  fi

  source_monitor_sink="\$(audio_source_monitor_sink "\${default_source}" || true)"
  if [[ "\${default_source}" != *.monitor && -z "\${source_monitor_sink}" ]]; then
    log_warning "audio defaults look safe; keeping sink=\${default_sink:-<unknown>} source=\${default_source:-<unknown>}."
    return 0
  fi

  if [[ -n "\${source_monitor_sink}" ]]; then
    log_warning "unsafe audio routing detected: source \${default_source} monitors sink \${source_monitor_sink}."
  else
    log_warning "unsafe audio routing detected: source \${default_source} is a monitor source."
  fi

  if ! ensure_virtual_null_sink "\${playback_sink_name}" "TeamSpeak_CLI_Playback"; then
    log_warning "continuing with the existing audio defaults because safe playback provisioning failed."
    return 0
  fi

  if ! ensure_virtual_null_sink "\${capture_sink_name}" "TeamSpeak_CLI_Capture"; then
    log_warning "continuing with the existing audio defaults because safe capture provisioning failed."
    return 0
  fi

  launch_sink="\${playback_sink_name}"
  launch_source="\${capture_source_name}"
  log_warning "launching TeamSpeak with isolated virtual audio devices: PULSE_SINK=\${launch_sink} PULSE_SOURCE=\${launch_source}."
  export TS3CLIENT_LAUNCH_PULSE_SINK="\${launch_sink}"
  export TS3CLIENT_LAUNCH_PULSE_SOURCE="\${launch_source}"
}

if [[ -z "\${runtime_library_path}" && -d "\${runtime_root}" ]]; then
  runtime_library_path="\$(find "\${runtime_root}" -type f -name '*.so*' -printf '%h\n' 2>/dev/null | sort -u | paste -sd ':' -)"
fi

launch_library_path="\${client_dir}"
if [[ -n "\${runtime_library_path}" ]]; then
  launch_library_path="\${launch_library_path}:\${runtime_library_path}"
fi

require_runtime_libraries "\${launch_library_path}\${LD_LIBRARY_PATH:+:\${LD_LIBRARY_PATH}}"
warn_if_audio_nodes_missing_descriptions
configure_safe_virtual_audio

launch_env=(
  env
  "LD_LIBRARY_PATH=\${launch_library_path}\${LD_LIBRARY_PATH:+:\${LD_LIBRARY_PATH}}"
  "TS3_CLIENT_DIR=\${client_dir}"
)
if [[ -n "\${TS3CLIENT_LAUNCH_PULSE_SINK:-}" ]]; then
  launch_env+=("PULSE_SINK=\${TS3CLIENT_LAUNCH_PULSE_SINK}")
fi
if [[ -n "\${TS3CLIENT_LAUNCH_PULSE_SOURCE:-}" ]]; then
  launch_env+=("PULSE_SOURCE=\${TS3CLIENT_LAUNCH_PULSE_SOURCE}")
fi

exec "\${launch_env[@]}" "\${client_dir}/ts3client_runscript.sh" "\$@"
EOF

install -m 0755 "${wrapper_tmp}" "${wrapper_path}"
rm -f "${wrapper_tmp}"
