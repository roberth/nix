#!/usr/bin/env bash

# Without --keep-going, a failed build cancels a substitution that runs at
# the same time. The substitution goal is a task, and its copy runs in
# `runInThread`: dropping the goal cancels the download of the NAR, also
# when the server sends nothing more. Before, the goal waited for the
# copy to finish, here until the stalled download timed out.

source common.sh

TODO_NixOS

needLocalStore "'--no-require-sigs' can't be used with the daemon"

clearStore
clearCacheCache

server="${_NIX_TEST_BUILD_DIR}/test-http-server/test-http-server"
www=$TEST_ROOT/www
serverLog=$TEST_ROOT/server.log
export SERVER_LOG=$serverLog
: > "$serverLog"

# Wait until the file $1 contains the text $2, for at most 60 s.
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

# A binary cache that has `big`, whose NAR the server stalls after
# 100 kB.
big=$(nix-build --no-out-link substitute-cancel.nix -A big)
nix copy --to "file://$www" "$big"
nix-store --delete "$big"
sed -i 's|^URL: nar/|URL: stall/nar/|' "$www"/*.narinfo

"$server" "$www" "$TEST_ROOT/port" "$serverLog" 100000 &
serverPid=$!
trap 'kill "$serverPid"' EXIT
waitForText "$TEST_ROOT/port" ""
port=$(cat "$TEST_ROOT/port")

# Microseconds since the epoch, without `date +%N`, which BSD date lacks.
start=${EPOCHREALTIME/[.,]/}
expectStderr 100 nix-build --no-out-link substitute-cancel.nix -A big -A fail \
    --substituters "http://127.0.0.1:$port" --no-require-sigs \
    --stalled-download-timeout 3600 > "$TEST_ROOT/build.err"
end=${EPOCHREALTIME/[.,]/}
cat "$TEST_ROOT/build.err"
grepQuiet "Cannot build .*-substitute-cancel-fail.drv" "$TEST_ROOT/build.err"

echo "the build ended $(( (end - start) / 1000 )) ms after it started"
(( end - start < 60000000 ))

# The substitution was cancelled, not completed. The cancelled copy can
# leave a partial directory at the path, but not a valid path, as an
# interrupt does.
grepQuiet "^closed by client nar/" "$serverLog"
expect 1 nix-store --check-validity "$big"
