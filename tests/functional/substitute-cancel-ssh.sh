#!/usr/bin/env bash

# Without --keep-going, a failed build cancels a substitution that runs at
# the same time. Here the substituter is an SSH store (`ssh-ng://`, the
# daemon protocol, and `ssh://`, the serve protocol), whose remote program
# stops sending in the middle of the NAR. The copy runs in `runInThread`,
# and its reads from the connection wake up when the goal is dropped.
# Before, the drop waited for a read that never returned.

source common.sh

TODO_NixOS

needLocalStore "'--no-require-sigs' can't be used with the daemon"

clearStore

stallLog=$TEST_ROOT/stall.log
stallPids=$TEST_ROOT/stall.pids
: > "$stallLog"
: > "$stallPids"
export SERVER_LOG=$stallLog

# The stalled remote programs keep running after Nix closes the
# connection; stop them at the end.
trap 'kill $(cat "$stallPids") 2> /dev/null || true' EXIT

# A remote program that forwards the first 100 kB of the output of the
# real one, and then sends nothing more, without closing the connection.
# `dd` with blocks of one byte passes on each byte at once; `head -c`
# can keep the output until it has all of it.
makeStallingProgram() {
    local real=$1 wrapper=$TEST_ROOT/stall-$1
    cat > "$wrapper" <<EOF
#!$SHELL
$real "\$@" | {
    dd bs=1 count=100000 status=none
    echo "stalled nar/ by $real" >> "$stallLog"
    echo "\$BASHPID" >> "$stallPids"
    exec sleep 3600
}
EOF
    chmod +x "$wrapper"
    echo "$wrapper"
}

# A store that has `big`, and that the remote programs serve.
big=$(nix-build --no-out-link substitute-cancel.nix -A big)
remoteRoot=$TEST_ROOT/remote
remoteStore="$remoteRoot?store=$NIX_STORE_DIR&real=$remoteRoot$NIX_STORE_DIR"
nix copy --no-check-sigs --to "$remoteStore" "$big"
nix-store --delete "$big"

encodedRemoteStore="$remoteRoot%3fstore=$NIX_STORE_DIR%26real=$remoteRoot$NIX_STORE_DIR"

for proto in ssh-ng ssh; do
    if [[ $proto == ssh-ng ]]; then
        program=$(makeStallingProgram nix-daemon)
    else
        program=$(makeStallingProgram nix-store)
    fi
    substituter="$proto://localhost?store=$NIX_STORE_DIR&remote-store=$encodedRemoteStore&remote-program=$program"
    : > "$stallLog"

    # Microseconds since the epoch, without `date +%N`, which BSD date lacks.
    start=${EPOCHREALTIME/[.,]/}
    expectStderr 100 nix-build --no-out-link substitute-cancel.nix -A big -A fail \
        --substituters "$substituter" --no-require-sigs > "$TEST_ROOT/build.err"
    end=${EPOCHREALTIME/[.,]/}
    cat "$TEST_ROOT/build.err"
    grepQuiet "Cannot build .*-substitute-cancel-fail.drv" "$TEST_ROOT/build.err"

    echo "$proto: the build ended $(( (end - start) / 1000 )) ms after it started"
    (( end - start < 60000000 ))

    # The substitution stalled and was cancelled, not completed.
    grepQuiet "^stalled nar/" "$stallLog"
    expect 1 nix-store --check-validity "$big"
done
