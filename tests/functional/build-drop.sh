#!/usr/bin/env bash

# A task on the reactor awaits a build (`Worker::buildPathsWithResultsAsync`)
# and is dropped while the builder runs. The drop cancels the build: the
# builder is killed and reaped, and the lock on the output is released, so
# that the same build can run again at once.

source common.sh

TODO_NixOS

needLocalStore "the program builds in its own process"

clearStore

pidFile=$TEST_ROOT/builder.pid
doneFile=$TEST_ROOT/builder.done
export BUILDER_PID_FILE=$pidFile BUILDER_DONE_FILE=$doneFile
rm -f "$pidFile" "$doneFile"

drv=$(nix-instantiate build-interrupt.nix)

# Microseconds since the epoch, without `date +%N`, which BSD date lacks.
start=${EPOCHREALTIME/[.,]/}
result=$("${_NIX_TEST_BUILD_DIR}/test-libstoreconsumer/test-build-drop" "$drv" "$pidFile")
end=${EPOCHREALTIME/[.,]/}
echo "the program ended $(( (end - start) / 1000 )) ms after it started"

[[ $result == dropped ]]
(( end - start < 30000000 ))

# The builder is gone, reaped: a zombie would still accept `kill -0`.
builderPid=$(cat "$pidFile")
(! kill -0 "$builderPid" 2> /dev/null)

# The lock is free: a new build of the same derivation does not wait for
# it, and its builder finishes at once.
touch "$doneFile"
start=${EPOCHREALTIME/[.,]/}
out=$(nix-build --no-out-link "$drv")
end=${EPOCHREALTIME/[.,]/}
echo "the second build took $(( (end - start) / 1000 )) ms"
[[ $(cat "$out") == "done" ]]
(( end - start < 10000000 ))
