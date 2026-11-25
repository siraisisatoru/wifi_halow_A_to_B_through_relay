#!/usr/bin/env bash
#
# Copyright 2026 siraisisatoru
#
# SPDX-License-Identifier: Apache-2.0
#
# Apply (or revert) the mm-iot-sdk fixes in patches/mm-iot-sdk/ to one or more SDK trees.
#
# Usage: tools/apply_sdk_patches.sh [--revert | --check | --unhide] [sdk_dir...]
#
# With no sdk_dir the default trees are used (paths relative to the repository root):
#   third_party/mm-iot-sdk
#   third_party/esp-halow/halow/components/mm-iot-sdk   (only if it exists)
#
# Each patch is handled on its own and the script is idempotent:
#   apply:  skip if already applied, apply if it applies cleanly, otherwise fail.
#   revert: skip if not applied, reverse it if it reverses cleanly, otherwise fail.
#   check:  report the state of every patch and change nothing; exits non-zero if any patch
#           is neither applied nor cleanly applicable.
#   unhide: only clear the skip-worktree flags (see below), so the patched files show as
#           modified again; tools/apply_sdk_patches.sh (apply) hides them again.
#
# Only the working tree is modified, never a commit. After a successful apply, the patched files
# are marked skip-worktree in the SDK's git index (a local flag, nothing is committed), so
# `git status`, the parent repository and editors such as VS Code show the SDK as clean.
# `git diff` therefore does not show the patches either: use --check, or --unhide first.
# --revert clears the flags. Revert before checking out another SDK version.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PATCH_DIR="${REPO_ROOT}/patches/mm-iot-sdk"

DEFAULT_SDK_DIRS=(
    "third_party/mm-iot-sdk"
    "third_party/esp-halow/halow/components/mm-iot-sdk"
)

usage()
{
    sed -n '7,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

die()
{
    echo "error: $*" >&2
    exit 1
}

# Print a path relative to the repository root when it lies inside it.
rel()
{
    case "$1" in
        "${REPO_ROOT}") echo "." ;;
        "${REPO_ROOT}"/*) echo "${1#"${REPO_ROOT}"/}" ;;
        *) echo "$1" ;;
    esac
}

mode="apply"
sdk_dirs=()
while [ $# -gt 0 ]; do
    case "$1" in
        --revert) mode="revert" ;;
        --check) mode="check" ;;
        --unhide) mode="unhide" ;;
        -h | --help)
            usage
            exit 0
            ;;
        --)
            shift
            while [ $# -gt 0 ]; do
                sdk_dirs+=("$1")
                shift
            done
            break
            ;;
        -*) die "unknown option '$1' (see --help)" ;;
        *) sdk_dirs+=("$1") ;;
    esac
    shift
done

patches=()
for p in "${PATCH_DIR}"/*.patch; do
    [ -e "${p}" ] && patches+=("${p}")
done
[ ${#patches[@]} -gt 0 ] || die "no patches found in $(rel "${PATCH_DIR}")"

if [ ${#sdk_dirs[@]} -eq 0 ]; then
    for d in "${DEFAULT_SDK_DIRS[@]}"; do
        if [ -d "${REPO_ROOT}/${d}/framework" ]; then
            sdk_dirs+=("${REPO_ROOT}/${d}")
        elif [ "${d}" = "${DEFAULT_SDK_DIRS[0]}" ]; then
            die "${d} is missing or not checked out (git submodule update --init ${d})"
        fi
    done
fi

# Run "git apply" so that the a/ b/ paths in the patches resolve against the SDK root. Inside a
# git work tree, git apply interprets patch paths relative to the top of that work tree and ignores
# anything outside the current directory, so when the SDK is a sub-directory of some repository
# (for example a vendored copy rather than a submodule) the prefix must be passed explicitly.
sdk_git_apply()
{
    local sdk="$1"
    shift
    local prefix=""
    if git -C "${sdk}" rev-parse --is-inside-work-tree > /dev/null 2>&1; then
        prefix="$(git -C "${sdk}" rev-parse --show-prefix)"
    fi
    if [ -n "${prefix}" ]; then
        (cd "${sdk}" && git apply --directory="${prefix%/}" "$@")
    else
        (cd "${sdk}" && git apply "$@")
    fi
}

# Files touched by the patches, relative to the SDK root.
patched_files()
{
    git apply --numstat "${patches[@]}" | awk '{print $3}' | sort -u
}

# Set (on) or clear (off) skip-worktree on the patched files, if the SDK is in a git work tree.
set_hidden()
{
    local sdk="$1" flag f
    git -C "${sdk}" rev-parse --is-inside-work-tree > /dev/null 2>&1 || return 0
    [ "$2" = "on" ] && flag="--skip-worktree" || flag="--no-skip-worktree"
    while IFS= read -r f; do
        git -C "${sdk}" update-index "${flag}" -- "${f}"
    done < <(patched_files)
}

# Number of patched files currently marked skip-worktree.
count_hidden()
{
    local sdk="$1" n=0 f
    git -C "${sdk}" rev-parse --is-inside-work-tree > /dev/null 2>&1 || { echo 0; return; }
    while IFS= read -r f; do
        case "$(git -C "${sdk}" ls-files -v -- "${f}")" in
            S\ *) n=$((n + 1)) ;;
        esac
    done < <(patched_files)
    echo "${n}"
}

process_sdk()
{
    local sdk="$1"
    local failed=0
    local list=()
    local p name

    [ -d "${sdk}/framework/morselib" ] || die "$(rel "${sdk}") does not look like an mm-iot-sdk tree"
    echo "== $(rel "${sdk}") (${mode})"

    if [ "${mode}" = "unhide" ]; then
        set_hidden "${sdk}" off
        echo "   patched files no longer hidden from git status"
        return 0
    fi

    if [ "${mode}" = "revert" ]; then
        # Undo in reverse order so that later patches never depend on missing context.
        local i
        for ((i = ${#patches[@]} - 1; i >= 0; i--)); do
            list+=("${patches[$i]}")
        done
    else
        list=("${patches[@]}")
    fi

    for p in "${list[@]}"; do
        name="$(basename "${p}")"
        if sdk_git_apply "${sdk}" -R --check "${p}" > /dev/null 2>&1; then
            case "${mode}" in
                apply | check) echo "   applied      ${name}" ;;
                revert)
                    sdk_git_apply "${sdk}" -R "${p}" || die "failed to reverse ${name}"
                    echo "   reverted     ${name}"
                    ;;
            esac
        elif sdk_git_apply "${sdk}" --check "${p}" > /dev/null 2>&1; then
            case "${mode}" in
                apply)
                    sdk_git_apply "${sdk}" "${p}" || die "failed to apply ${name}"
                    echo "   applied now  ${name}"
                    ;;
                check | revert) echo "   not applied  ${name}" ;;
            esac
        else
            echo "   CONFLICT     ${name}" >&2
            echo "   ---- git apply --check output:" >&2
            sdk_git_apply "${sdk}" --check "${p}" 2>&1 | sed 's/^/   | /' >&2 || true
            if [ "${mode}" = "check" ]; then
                failed=1
                continue
            fi
            die "$(rel "${p}") neither applies nor reverses cleanly in $(rel "${sdk}");" \
                "the tree is not at the expected SDK version or has local edits"
        fi
    done

    case "${mode}" in
        apply)
            set_hidden "${sdk}" on
            echo "   patched files hidden from git status (skip-worktree)"
            ;;
        revert) set_hidden "${sdk}" off ;;
        check) echo "   $(count_hidden "${sdk}") of $(patched_files | wc -l | tr -d ' ') patched files hidden from git status" ;;
    esac

    return ${failed}
}

status=0
for sdk in "${sdk_dirs[@]}"; do
    [ -d "${sdk}" ] || die "no such directory: ${sdk}"
    sdk="$(cd "${sdk}" && pwd)"
    process_sdk "${sdk}" || status=1
done
exit ${status}
