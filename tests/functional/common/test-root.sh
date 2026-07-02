# shellcheck shell=bash

TEST_SUBDIR="${TEST_SUITE_NAME:-default}/${TEST_NAME:-tests/functional/}"
# `meson test --repeat=N` runs each iteration with a shared TEST_NAME but
# a per-iteration `MESON_TEST_ITERATION`. Without appending it, all
# iterations resolve to the same TEST_ROOT and race on the shared
# working directory (cache dirs, TEST_HOME, store). Suffix it when set.
if [[ -n "${MESON_TEST_ITERATION-}" && "${MESON_TEST_ITERATION-}" != "1" ]]; then
    TEST_SUBDIR+=".iter${MESON_TEST_ITERATION}"
fi
TEST_ROOT=$(realpath "${TMPDIR:-/tmp}/nix-test")/"$TEST_SUBDIR"
export TEST_ROOT
