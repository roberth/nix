#!/usr/bin/env bash

# Interrupt tarball fetches at random times, and check the tarball cache
# after each one.
#
# Each run fetches TARBALL from a local HTTP server into an empty tarball
# cache, and sends SIGINT to Nix after a random delay. A run either
# completes before the signal, or Nix exits with "interrupted by the
# user". After each run, the script checks that the cache has no
# temporary file, that each pack has its index, and that `git fsck --full`
# accepts the cache. A completed run must give the hash of a reference
# run.
#
# Usage:
#
#     maintainers/tarball-cancel-stress.sh TARBALL [RUNS] [MAX_DELAY_MS]
#
# - TARBALL: an archive, for example a Nixpkgs tarball from GitHub.
# - RUNS: the number of runs, 50 by default.
# - MAX_DELAY_MS: the longest delay before the signal. By default, 1.2
#   times the duration of a reference fetch, so that the signals fall into
#   every phase: download, unpacking, flush, and the hashing after it.
#
# Environment:
#
# - NIX: the `nix` to test, `nix` from PATH by default.
# - TEST_HTTP_SERVER: the test HTTP server of the functional tests,
#   `build/src/nix-functional-tests/test-http-server/test-http-server` by
#   default (`ninja -C build src/nix-functional-tests/test-http-server/test-http-server`).
# - SEED: the seed of the random delays, for a repeatable sequence.
# - _NIX_FILE_TRANSFER_BACKEND: passed on to Nix, to test the `reactor` or
#   the `thread` backend. Give MAX_DELAY_MS too when comparing the two, so
#   that both get the same delays.
#
# The summary gives the outcomes and the time from the signal to the exit
# of Nix.

set -euo pipefail

tarball=$(realpath "$1")
runs=${2:-50}
maxDelay=${3:-}
nix=${NIX:-nix}
server=${TEST_HTTP_SERVER:-build/src/nix-functional-tests/test-http-server/test-http-server}
RANDOM=${SEED:-$$}

# The signal must arrive after Nix sets up its signal handling: a
# background job of a script starts with SIGINT ignored.
minDelay=200

work=$(mktemp -d)
trap 'kill "$serverPid" 2>/dev/null || true; rm -rf "$work"' EXIT

mkdir -p "$work/www"
ln -s "$tarball" "$work/www/$(basename "$tarball")"
"$server" "$work/www" "$work/port" "$work/server.log" 0 &
serverPid=$!
while [[ ! -s $work/port ]]; do sleep 0.1; done
url="http://127.0.0.1:$(cat "$work/port")/$(basename "$tarball")"

export NIX_CONFIG="experimental-features = nix-command flakes"
store="dummy://?read-only=false"

# Fetch into a fresh cache under $work/$1, and print the NAR hash.
fetch() {
    XDG_CACHE_HOME=$work/$1 "$nix" flake prefetch --store "$store" --json "tarball+$url" | sed 's/.*"hash":"\([^"]*\)".*/\1/'
}

# Check the invariants of the cache under $work/$1.
checkCache() {
    local cache=$work/$1/nix/tarball-cache-v2
    [[ -d $cache ]] || return 0
    local tmp
    tmp=$(find "$cache/objects/pack" -name 'tmp_*' | wc -l)
    if (( tmp != 0 )); then
        echo "run $1: $tmp temporary files in the cache" >&2
        return 1
    fi
    for pack in "$cache"/objects/pack/*.pack; do
        [[ -e $pack ]] || continue
        if [[ ! -e ${pack%.pack}.idx ]]; then
            echo "run $1: pack without index: $pack" >&2
            return 1
        fi
    done
    git -C "$cache" fsck --full --no-progress > "$work/fsck.log" 2>&1 || {
        echo "run $1: git fsck failed:" >&2
        cat "$work/fsck.log" >&2
        return 1
    }
}

echo "reference fetch of $url"
start=${EPOCHREALTIME/[.,]/}
expected=$(fetch reference)
end=${EPOCHREALTIME/[.,]/}
referenceMs=$(( (end - start) / 1000 ))
echo "expected hash: $expected; the fetch took $referenceMs ms"
rm -rf "$work/reference"
if [[ -z $maxDelay ]]; then
    maxDelay=$(( referenceMs * 12 / 10 ))
fi
echo "signals after $minDelay to $maxDelay ms"

completed=0
interrupted=0
times=()

for run in $(seq 1 "$runs"); do
    delay=$(( minDelay + RANDOM % (maxDelay - minDelay) ))
    XDG_CACHE_HOME=$work/run "$nix" flake prefetch --store "$store" --json "tarball+$url" \
        > "$work/out" 2> "$work/err" &
    pid=$!

    sleep "$(printf '%d.%03d' $(( delay / 1000 )) $(( delay % 1000 )))"

    start=${EPOCHREALTIME/[.,]/}
    kill -INT "$pid" 2>/dev/null || true
    status=0
    wait "$pid" || status=$?
    end=${EPOCHREALTIME/[.,]/}

    if (( status == 0 )); then
        hash=$(sed 's/.*"hash":"\([^"]*\)".*/\1/' "$work/out")
        if [[ $hash != "$expected" ]]; then
            echo "run $run: completed with hash $hash, expected $expected" >&2
            exit 1
        fi
        completed=$(( completed + 1 ))
        outcome=completed
    elif grep -q "interrupted by the user" "$work/err"; then
        interrupted=$(( interrupted + 1 ))
        times+=( $(( (end - start) / 1000 )) )
        outcome="interrupted, exit after $(( (end - start) / 1000 )) ms"
    else
        echo "run $run: exit status $status without an interrupt:" >&2
        cat "$work/err" >&2
        exit 1
    fi

    checkCache run
    packs=$(find "$work/run/nix/tarball-cache-v2/objects/pack" -name '*.pack' 2>/dev/null | wc -l)
    echo "run $run: signal after $delay ms: $outcome; packs in the cache: $packs"
    rm -rf "$work/run"
done

echo
echo "runs: $runs; completed before the signal: $completed; interrupted: $interrupted"
if (( ${#times[@]} > 0 )); then
    printf '%s\n' "${times[@]}" | sort -n | awk '
        { t[NR] = $1 }
        END {
            printf "time from SIGINT to exit (ms): min %d, median %d, p90 %d, max %d\n",
                t[1], t[int((NR + 1) / 2)], t[int(NR * 0.9 + 0.5)], t[NR]
        }'
fi
