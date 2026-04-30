#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/e2e/runtime_common.sh
source "${script_dir}/runtime_common.sh"

[[ "$(ts3_runtime_client_package_for_soname "libGL.so.1")" == "libgl1" ]]
[[ "$(ts3_runtime_client_package_for_soname "libGLdispatch.so.0")" == "libglvnd0" ]]
[[ "$(ts3_runtime_client_package_for_soname "libGLX.so.0")" == "libglx0" ]]
[[ "$(ts3_runtime_client_package_for_soname "libXi.so.6")" == "libxi6" ]]
[[ "$(ts3_runtime_client_package_for_soname "libasound.so.2")" == "libasound2" ]]
[[ "$(ts3_runtime_client_package_for_soname "libevent-2.1.so.7")" == "libevent-2.1-7" ]]
[[ "$(ts3_runtime_client_package_for_soname "libXfont2.so.2")" == "libxfont2" ]]
[[ "$(ts3_runtime_client_package_for_soname "libpixman-1.so.0")" == "libpixman-1-0" ]]
[[ "$(ts3_runtime_client_package_for_soname "libaudit.so.1")" == "libaudit1" ]]

mapfile -t xvfb_packages < <(ts3_runtime_xvfb_bootstrap_packages)
[[ "${xvfb_packages[*]}" == "xvfb xserver-common xkb-data x11-xkb-utils" ]]

mapfile -t asound_packages < <(ts3_runtime_client_packages_for_soname "libasound.so.2")
[[ "${asound_packages[*]}" == "libasound2 libasound2t64" ]]

mapfile -t event_packages < <(ts3_runtime_client_packages_for_soname "libevent-2.1.so.7")
[[ "${event_packages[*]}" == "libevent-2.1-7 libevent-2.1-7t64" ]]

tmp_dir="$(mktemp -d)"
trap 'rm -rf "${tmp_dir}"' EXIT

cat >"${tmp_dir}/apt-cache" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" != "policy" || -z "${2:-}" ]]; then
  exit 2
fi

case "$2" in
  libasound2|libevent-2.1-7|libglib2.0-0)
    printf '%s:\n  Installed: (none)\n  Candidate: (none)\n' "$2"
    ;;
  libasound2t64|libevent-2.1-7t64|libglib2.0-0t64)
    printf '%s:\n  Installed: (none)\n  Candidate: 1.0\n' "$2"
    ;;
  *)
    printf '%s:\n  Installed: (none)\n  Candidate: 1.0\n' "$2"
    ;;
esac
EOF
chmod +x "${tmp_dir}/apt-cache"

(
  PATH="${tmp_dir}:${PATH}"
  [[ "$(ts3_runtime_resolve_client_package_for_soname "libasound.so.2")" == "libasound2t64" ]]
  [[ "$(ts3_runtime_resolve_client_package_for_soname "libevent-2.1.so.7")" == "libevent-2.1-7t64" ]]
  [[ "$(ts3_runtime_resolve_client_package_for_soname "libglib-2.0.so.0")" == "libglib2.0-0t64" ]]
)
