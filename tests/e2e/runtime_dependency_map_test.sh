#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/e2e/runtime_common.sh
source "${script_dir}/runtime_common.sh"

[[ "$(ts3_runtime_client_package_for_soname "libGL.so.1")" == "libgl1" ]]
[[ "$(ts3_runtime_client_package_for_soname "libGLdispatch.so.0")" == "libglvnd0" ]]
[[ "$(ts3_runtime_client_package_for_soname "libGLX.so.0")" == "libglx0" ]]
[[ "$(ts3_runtime_client_package_for_soname "libXi.so.6")" == "libxi6" ]]
