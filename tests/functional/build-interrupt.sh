#!/usr/bin/env bash

# A user interrupt ends a build at once. The Worker waits for the output
# of the builder without a timeout; the interrupt wakes it, and the drop
# of the goals kills the builder.

source common.sh

TODO_NixOS

needLocalStore "the test checks the builder process, which the daemon would run"

clearStore

pidFile=$TEST_ROOT/builder.pid
export BUILDER_PID_FILE=$pidFile
rm -f "$pidFile"

nix-build --no-out-link build-interrupt.nix 2> "$TEST_ROOT/err" &
pid=$!

for _ in $(seq 1 600); do
    [[ -s $pidFile ]] && break
    sleep 0.1
done
builderPid=$(cat "$pidFile")

# Microseconds since the epoch, without `date +%N`, which BSD date lacks.
start=${EPOCHREALTIME/[.,]/}
kill -INT "$pid"
status=0
wait "$pid" || status=$?
end=${EPOCHREALTIME/[.,]/}

echo "interrupted; exited with $status after $(( (end - start) / 1000 )) ms"
(( status != 0 ))
(( end - start < 5000000 ))
grepQuiet "interrupted by the user" "$TEST_ROOT/err"

# The builder is gone.
for _ in $(seq 1 50); do
    kill -0 "$builderPid" 2> /dev/null || break
    sleep 0.1
done
(! kill -0 "$builderPid" 2> /dev/null)
