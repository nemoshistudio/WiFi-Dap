#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# test_wifi_cred_logic.sh -- host unit tests for the WiFi multi-credential pure
# logic (project_components/wifi_manager/wifi_cred_logic.c, WP-201/WP-210).
#
# Compiles wifi_cred_logic.c with scripts/wifi_cred_logic_test.c using the host
# C compiler and runs the TAP-ish assertions. No ESP-IDF, no Docker, no
# hardware required (gcc/clang only).
#
# Usage:
#   scripts/test_wifi_cred_logic.sh [-h] [-v] [--]
#
# Options:
#   -v, --verbose   keep the temp build dir and print the full compiler command
#   -h, --help      show this help
#
# Environment:
#   CC        C compiler (default: cc, then gcc, then clang)
#   CFLAGS    extra compiler flags (appended after the default flags)
#   WERROR=1  add -Werror (recommended for CI)
#   KEEP=1    keep the temp build dir (same as --verbose)
#   TEST_BIN  write the test binary to this path instead of a temp dir
#
# Exit status:
#   0  all checks passed
#   1  assertion failure
#   2  toolchain / usage / compile error
#
# Notes:
#   - The test binary needs a POSIX-y host (Linux/macOS/WSL). Windows native is
#     not supported; use WSL or the Docker image.
#   - This script only builds/link a standalone host binary; it never touches
#     git and never builds firmware (use the build scripts for that).
set -uo pipefail

readonly SCRIPT_NAME="${0##*/}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly SCRIPT_DIR
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
readonly ROOT_DIR
readonly COMPONENT_DIR="${ROOT_DIR}/project_components/wifi_manager"
readonly LOGIC_SRC="${COMPONENT_DIR}/wifi_cred_logic.c"
readonly TEST_SRC="${SCRIPT_DIR}/wifi_cred_logic_test.c"
readonly HEADER="${COMPONENT_DIR}/wifi_credential.h"

VERBOSE=0

usage() {
	cat <<'USAGE'
Usage: test_wifi_cred_logic.sh [-h] [-v] [--]

Host unit tests for the WiFi multi-credential pure logic
(project_components/wifi_manager/wifi_cred_logic.c, WP-201/WP-210).
Compiles it with scripts/wifi_cred_logic_test.c and runs the TAP-ish checks.

Options:
  -h, --help      show this help
  -v, --verbose   keep the temp build dir and print the compiler command

Environment:
  CC        C compiler (default: cc, then gcc, then clang)
  CFLAGS    extra compiler flags (appended after the defaults)
  WERROR=1  add -Werror (recommended for CI)
  KEEP=1    keep the temp build dir (same as --verbose)
  TEST_BIN  write the test binary to this path instead of a temp dir

Exit status: 0 = all checks passed, 1 = assertion failure,
             2 = usage / toolchain / compile error.
USAGE
}

die() {
	printf '%s: error: %s\n' "${SCRIPT_NAME}" "$*" >&2
	exit 2
}

while [ "$#" -gt 0 ]; do
	case "$1" in
		-h|--help)
			usage
			exit 0
			;;
		-v|--verbose)
			VERBOSE=1
			shift
			;;
		--)
			shift
			break
			;;
		*)
			printf '%s: unknown argument: %s\n' "${SCRIPT_NAME}" "$1" >&2
			usage >&2
			exit 2
			;;
	esac
done

[ -f "${LOGIC_SRC}" ] || die "missing ${LOGIC_SRC} (run from the repository worktree)"
[ -f "${TEST_SRC}" ] || die "missing ${TEST_SRC}"
[ -f "${HEADER}" ] || die "missing ${HEADER}"

# ---------------------------------------------------------------- toolchain
if [ -z "${CC:-}" ]; then
	for candidate in cc gcc clang; do
		if command -v "${candidate}" >/dev/null 2>&1; then
			CC="${candidate}"
			break
		fi
	done
fi
[ -n "${CC:-}" ] || die "no host C compiler found (set CC=...)"
command -v "${CC}" >/dev/null 2>&1 || die "C compiler not executable: ${CC}"

# ---------------------------------------------------------------- work dir
if [ -n "${TEST_BIN:-}" ]; then
	BUILD_DIR="$(dirname -- "${TEST_BIN}")"
	mkdir -p "${BUILD_DIR}" || die "cannot create ${BUILD_DIR}"
	WORK_DIR=""
	BIN="${TEST_BIN}"
else
	WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/wifi-cred-logic.XXXXXX")" || die "mktemp failed"
	BUILD_DIR="${WORK_DIR}"
	BIN="${BUILD_DIR}/wifi_cred_logic_test"
fi
cleanup() {
	if [ "${VERBOSE}" -eq 0 ] && [ -n "${WORK_DIR}" ] && [ -z "${KEEP:-}" ]; then
		rm -rf "${WORK_DIR}"
	fi
}
trap cleanup EXIT

# ---------------------------------------------------------------- compile
CFLAGS_ALL=(-std=c11 -Wall -Wextra -I "${COMPONENT_DIR}")
if [ "${WERROR:-0}" = "1" ]; then
	CFLAGS_ALL+=(-Werror)
fi
# shellcheck disable=SC2206  # word splitting of user-provided CFLAGS is intended
if [ -n "${CFLAGS:-}" ]; then
	CFLAGS_ALL+=(${CFLAGS})
fi

printf '# %s: host test for %s\n' "${SCRIPT_NAME}" "wifi_cred_logic.c"
printf '# compiler: %s\n' "$("${CC}" --version 2>/dev/null | head -n 1)"
printf '# cflags: %s\n' "${CFLAGS_ALL[*]}"
if [ "${VERBOSE}" -eq 1 ]; then
	printf '# command: %s %s %s -o %s\n' "${CC}" "${CFLAGS_ALL[*]}" \
		"${LOGIC_SRC} ${TEST_SRC}" "${BIN}"
fi

compile_log="${BUILD_DIR}/compile.log"
if ! "${CC}" "${CFLAGS_ALL[@]}" "${LOGIC_SRC}" "${TEST_SRC}" -o "${BIN}" \
	>"${compile_log}" 2>&1; then
	printf 'Bail out! host compilation failed\n'
	sed 's/^/# /' "${compile_log}" >&2
	printf '# see: %s\n' "${compile_log}" >&2
	exit 2
fi
if [ "${VERBOSE}" -eq 1 ]; then
	sed 's/^/# /' "${compile_log}"
fi

# ---------------------------------------------------------------- run
"${BIN}"
rc=$?

if [ "${VERBOSE}" -eq 1 ]; then
	printf '# binary: %s\n' "${BIN}"
fi
if [ "${rc}" -eq 0 ]; then
	exit 0
fi
printf '# %s: FAILED (exit %d)\n' "${SCRIPT_NAME}" "${rc}" >&2
exit 1
