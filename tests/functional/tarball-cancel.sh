#!/usr/bin/env bash

# An interrupt during a tarball fetch must leave the tarball cache as it
# was: no pack, no temporary file, and a repository that `git fsck`
# accepts. Each case interrupts Nix in one phase of the fetch: the
# download, the unpacking, and the flush that writes the pack. The
# fetch runs as one task (`downloadIntoTarballCache()`), and the
# interrupt cancels it.

source common.sh

requireGit
clearStoreIfPossible

server="${_NIX_TEST_BUILD_DIR}/test-http-server/test-http-server"
cache=$TEST_HOME/.cache/nix/tarball-cache-v2

# A tarball of incompressible files, so that the download stalls in the
# middle of the tarball and not in the middle of its compressed header.
src=$TEST_ROOT/src
mkdir -p "$src/tree"
for i in $(seq 1 100); do
    head -c 20000 /dev/urandom > "$src/tree/file$i"
done
www=$TEST_ROOT/www
mkdir -p "$www"
tar -C "$src" -czf "$www/tree.tar.gz" tree
expectedHash=$(nix hash path "$src/tree")

serverLog=$TEST_ROOT/server.log
: > "$serverLog"
"$server" "$www" "$TEST_ROOT/port" "$serverLog" 500000 &
serverPid=$!
trap 'kill "$serverPid"' EXIT

# Wait until the file $1 contains the line or text $2, for at most 60 s.
waitForText() {
    for _ in $(seq 1 600); do
        if [[ -e $1 ]] && grep -qF -- "$2" "$1"; then
            return 0
        fi
        sleep 0.1
    done
    echo "timed out waiting for '$2' in $1" >&2
    cat "$1" >&2 || true
    return 1
}

waitForText "$TEST_ROOT/port" ""
port=$(cat "$TEST_ROOT/port")

fetchExpr() {
    echo "builtins.fetchTarball \"$1\""
}

# Interrupt a fetch of URL $1 in PHASE $2, once the file $3 contains the
# text $4. A phase other than the empty one needs the plugin in
# `tarball-cache-pause-plugin`, which holds the write still in that phase.
# With $5 = fiber, `nix-instantiate` evaluates on a fiber
# (`_NIX_EVAL_ON_FIBER`), which suspends in the fetch.
interruptFetch() {
    local url=$1 phase=$2 file=$3 text=$4 mode=${5:-}
    rm -rf "$TEST_HOME/.cache"
    local err=$TEST_ROOT/nix-$phase.err
    : > "$err"

    local pluginArgs=()
    if [[ -n $phase ]]; then
        pluginArgs=(--option plugin-files "$_NIX_TEST_TARBALL_CACHE_PAUSE_PLUGIN")
    fi
    if [[ $mode == fiber ]]; then
        _NIX_EVAL_ON_FIBER=1 nix-instantiate --eval --expr "$(fetchExpr "$url")" 2> "$err" &
    else
        _NIX_TEST_TARBALL_CACHE_PAUSE=$phase \
            nix "${pluginArgs[@]}" eval --impure --raw --expr "$(fetchExpr "$url")" 2> "$err" &
    fi
    local pid=$!

    waitForText "$file" "$text"

    # The flush holds still with its temporary pack on disk, which the
    # interrupt must remove.
    if [[ $phase == flush ]]; then
        [[ $(find "$cache/objects/pack" -type f -name 'tmp_pack_*' | wc -l) == 1 ]]
    fi

    # Microseconds since the epoch, without `date +%N`, which BSD date lacks.
    local start end status=0
    start=${EPOCHREALTIME/[.,]/}
    kill -INT "$pid"
    wait "$pid" || status=$?
    end=${EPOCHREALTIME/[.,]/}

    echo "interrupted in phase '$phase'${mode:+ on a $mode}; exited with $status after $(( (end - start) / 1000 )) ms"
    (( status != 0 ))
    (( end - start < 5000000 ))
    grepQuiet "interrupted by the user" "$err"

    # Nothing of the fetch is in the cache, and the cache is valid.
    [[ $(find "$cache/objects/pack" -type f | wc -l) == 0 ]]
    git -C "$cache" fsck --full
}

# curl runs on the reactor, and the fetch with `runOnReactor()`, or curl runs
# on a thread of its own, and the fetch with `runBlocking()`. Both must
# stop in each phase.
for backend in reactor thread; do
    export _NIX_FILE_TRANSFER_BACKEND=$backend
    : > "$serverLog"

    # During the download: the server stalls after 500 kB, while the
    # unpacking waits for more data. The interrupt also stops the transfer.
    interruptFetch "http://127.0.0.1:$port/stall/tree.tar.gz" "" "$serverLog" "stalled tree.tar.gz"
    waitForText "$serverLog" "closed by client tree.tar.gz 500000"

    # The same, with the evaluation on a fiber. The interrupt drops the
    # evaluation while it is suspended in the fetch, which drops the fetch.
    : > "$serverLog"
    interruptFetch "http://127.0.0.1:$port/stall/tree.tar.gz" "" "$serverLog" "stalled tree.tar.gz" fiber
    waitForText "$serverLog" "closed by client tree.tar.gz 500000"

    # A static Nix cannot load the plugin. The unit tests in
    # `libfetchers-tests/tarball-unpack.cc` cover these phases there.
    if [[ -n ${_NIX_TEST_TARBALL_CACHE_PAUSE_PLUGIN:-} ]]; then
        # While unpacking: the unpacking holds still after the first file.
        interruptFetch "http://127.0.0.1:$port/tree.tar.gz" unpack "$TEST_ROOT/nix-unpack.err" "paused in phase 'unpack'"

        # During the flush: the flush holds still after it wrote the
        # temporary pack, before the index. The interrupt removes the
        # temporary pack.
        interruptFetch "http://127.0.0.1:$port/tree.tar.gz" flush "$TEST_ROOT/nix-flush.err" "paused in phase 'flush'"
    else
        echo "no tarball cache pause plugin; skipping the unpack and flush phases"
    fi

    # A fetch after the interrupted ones unpacks the same tree, into one
    # pack.
    out=$(nix eval --impure --raw --expr "$(fetchExpr "http://127.0.0.1:$port/tree.tar.gz")")
    [[ $(nix hash path "$out") == "$expectedHash" ]]
    [[ $(find "$cache/objects/pack" -type f -name 'pack-*.pack' | wc -l) == 1 ]]
    [[ $(find "$cache/objects/pack" -type f -name 'tmp_*' | wc -l) == 0 ]]
    git -C "$cache" fsck --full
done
