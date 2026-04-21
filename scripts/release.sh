#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  scripts/release.sh [--dry-run] [patch|minor|major|X.Y.Z]

Examples:
  scripts/release.sh patch
  scripts/release.sh minor
  scripts/release.sh 1.2.3
  scripts/release.sh --dry-run patch

The script will:
1. Bump the version in CMakeLists.txt and src/teamspeak_cli/build/version.hpp
2. Run the release check command (default: make test)
3. Commit and push the release commit
4. Create and push an annotated git tag (vX.Y.Z)
5. Create a GitHub release using gh with generated notes
6. Let GitHub Actions attach the packaged release assets after the tag push

Environment:
  RELEASE_CHECK_CMD         Command to run before committing. Default: make test
  RELEASE_REMOTE            Git remote to push to. Default: origin
  RELEASE_CREATE_GH_RELEASE Set to 0 to skip gh release create. Default: 1
USAGE
}

log() {
  printf '[release] %s\n' "$*"
}

die() {
  printf '%s\n' "$*" >&2
  exit 1
}

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    die "Missing required command: $1"
  fi
}

run_cmd() {
  log "$*"
  if [[ "${dry_run}" -eq 0 ]]; then
    "$@"
  fi
}

read_current_version() {
  sed -nE 's/^[[:space:]]*VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+)$/\1/p' CMakeLists.txt | head -n 1
}

update_version_files() {
  local version="$1"

  log "updating version files to ${version}"
  if [[ "${dry_run}" -eq 1 ]]; then
    return 0
  fi

  sed -E -i \
    "s/^([[:space:]]*VERSION[[:space:]]+)[0-9]+\.[0-9]+\.[0-9]+$/\1${version}/" \
    CMakeLists.txt
  sed -E -i \
    "s/^#define TSCLI_VERSION \"[0-9]+\.[0-9]+\.[0-9]+\"$/#define TSCLI_VERSION \"${version}\"/" \
    src/teamspeak_cli/build/version.hpp
}

run_check_cmd() {
  log "running release checks: ${check_cmd}"
  if [[ "${dry_run}" -eq 0 ]]; then
    bash -lc "${check_cmd}"
  fi
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

dry_run=0
positionals=()

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --dry-run)
      dry_run=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      positionals+=("$1")
      shift
      ;;
  esac
done

if [[ "${#positionals[@]}" -ne 1 ]]; then
  usage
  exit 1
fi

bump_arg="${positionals[0]}"
remote="${RELEASE_REMOTE:-origin}"
check_cmd="${RELEASE_CHECK_CMD:-make test}"
create_gh_release="${RELEASE_CREATE_GH_RELEASE:-1}"

require_cmd git

if [[ "${create_gh_release}" != "0" ]]; then
  require_cmd gh
fi

git remote get-url "${remote}" >/dev/null 2>&1 || die "Unknown git remote: ${remote}"

if [[ -n "$(git status --porcelain)" ]]; then
  die "Working tree is not clean. Commit or stash changes before releasing."
fi

current_version="$(read_current_version)"
[[ -n "${current_version}" ]] || die "Unable to read current version from CMakeLists.txt."

if [[ "${bump_arg}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  new_version="${bump_arg}"
else
  IFS='.' read -r major minor patch <<<"${current_version}"
  case "${bump_arg}" in
    patch)
      patch=$((patch + 1))
      ;;
    minor)
      minor=$((minor + 1))
      patch=0
      ;;
    major)
      major=$((major + 1))
      minor=0
      patch=0
      ;;
    *)
      die "Invalid bump type: ${bump_arg}"
      ;;
  esac
  new_version="${major}.${minor}.${patch}"
fi

if [[ "${new_version}" == "${current_version}" ]]; then
  die "New version is the same as current version (${current_version})."
fi

tag="v${new_version}"
branch="$(git rev-parse --abbrev-ref HEAD)"
[[ "${branch}" != "HEAD" ]] || die "Release from a branch, not a detached HEAD."

if git rev-parse --verify --quiet "refs/tags/${tag}" >/dev/null; then
  die "Tag already exists locally: ${tag}"
fi

if git ls-remote --exit-code --tags "${remote}" "refs/tags/${tag}" >/dev/null 2>&1; then
  die "Tag already exists on ${remote}: ${tag}"
fi

if [[ "${create_gh_release}" != "0" ]] && gh release view "${tag}" >/dev/null 2>&1; then
  die "GitHub release already exists: ${tag}"
fi

update_version_files "${new_version}"
run_check_cmd

run_cmd git add CMakeLists.txt src/teamspeak_cli/build/version.hpp
run_cmd git commit -m "release: ${tag}"
run_cmd git push "${remote}" "${branch}"
run_cmd git tag -a "${tag}" -m "${tag}"
run_cmd git push "${remote}" "${tag}"

if [[ "${create_gh_release}" != "0" ]]; then
  run_cmd gh release create "${tag}" --title "${tag}" --generate-notes --verify-tag
else
  log "skipping gh release create because RELEASE_CREATE_GH_RELEASE=${create_gh_release}"
fi

log "Release complete: ${tag}"
