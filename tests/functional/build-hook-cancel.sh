#!/usr/bin/env bash

# Without --keep-going, a failed build cancels a goal that waits for the
# reply of the build hook. The goal reads the reply with a task that waits
# on the reactor, so the drop ends the wait and kills the hook. Before, the
# goals waited in a blocking read of the reply, here forever, because the
# hook never answers.

source common.sh

TODO_NixOS

needLocalStore "the hook runs in the process of the client"

clearStore

hookStarted=$TEST_ROOT/hook-started
rm -f "$hookStarted"
export HOOK_STARTED=$hookStarted

# A build hook that starts, and never answers.
hook=$TEST_ROOT/silent-hook
cat > "$hook" <<EOF
#!$SHELL
echo \$\$ > "$hookStarted"
exec sleep 3600
EOF
chmod +x "$hook"

# Microseconds since the epoch, without `date +%N`, which BSD date lacks.
start=${EPOCHREALTIME/[.,]/}
expectStderr 100 nix-build --no-out-link build-hook-cancel.nix -A remote -A fail \
    --option build-hook "$hook" --max-jobs 1 > "$TEST_ROOT/build.err"
end=${EPOCHREALTIME/[.,]/}
cat "$TEST_ROOT/build.err"
grepQuiet "Cannot build .*-build-hook-cancel-fail.drv" "$TEST_ROOT/build.err"

echo "the build ended $(( (end - start) / 1000 )) ms after it started"
(( end - start < 60000000 ))

# The drop killed the hook.
hookPid=$(cat "$hookStarted")
(! kill -0 "$hookPid" 2> /dev/null)
