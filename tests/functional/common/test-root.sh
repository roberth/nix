# shellcheck shell=bash

TEST_SUBDIR="${TEST_SUITE_NAME:-default}/${TEST_NAME:-tests/functional/}"
# `meson test --repeat=N` runs each iteration with a shared TEST_NAME but
# a per-iteration `MESON_TEST_ITERATION`. Without isolation, iterations
# resolve to the same TEST_ROOT and race on the shared working directory
# (cache dirs, TEST_HOME, store). Suffix the /tmp base rather than
# TEST_SUBDIR itself, so TEST_ROOT still ends with the test name (which
# some tests, e.g. builtins-cache's path-passthrough assertion, rely on).
NIX_TEST_TMP_BASE="${TMPDIR:-/tmp}/nix-test"
if [[ -n "${MESON_TEST_ITERATION-}" && "${MESON_TEST_ITERATION-}" != "1" ]]; then
    NIX_TEST_TMP_BASE+=".iter${MESON_TEST_ITERATION}"
fi
mkdir -p "$NIX_TEST_TMP_BASE"
TEST_ROOT=$(realpath "$NIX_TEST_TMP_BASE")/"$TEST_SUBDIR"
export TEST_ROOT
