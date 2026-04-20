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

log_warning() {
  printf '[ts3client] %s\n' "\$*" >&2
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

if [[ -z "\${runtime_library_path}" && -d "\${runtime_root}" ]]; then
  runtime_library_path="\$(find "\${runtime_root}" -type f -name '*.so*' -printf '%h\n' 2>/dev/null | sort -u | paste -sd ':' -)"
fi

launch_library_path="\${client_dir}"
if [[ -n "\${runtime_library_path}" ]]; then
  launch_library_path="\${launch_library_path}:\${runtime_library_path}"
fi

warn_if_audio_nodes_missing_descriptions

exec env \
  LD_LIBRARY_PATH="\${launch_library_path}\${LD_LIBRARY_PATH:+:\${LD_LIBRARY_PATH}}" \
  TS3_CLIENT_DIR="\${client_dir}" \
  "\${client_dir}/ts3client_runscript.sh" "\$@"
EOF

install -m 0755 "${wrapper_tmp}" "${wrapper_path}"
rm -f "${wrapper_tmp}"
