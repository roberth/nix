#!/usr/bin/env bash
# Correctness on file edits and reverts.
# See README.md for usage.

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

WORK="${1:-/tmp/sets-validation-synthetic}"
rm -rf "$WORK"
mkdir -p "$WORK/proj"
export HOME="$WORK/home"
mkdir -p "$HOME"
cd "$WORK/proj"

cat > expr.nix <<'EOF'
let
  contents = builtins.readFile ./data.txt;
  flag = builtins.readFile ./flag.txt;
in "${contents}|${flag}"
EOF

echo "data1" > data.txt
echo "flagA" > flag.txt

eval() {
    nix eval --impure --option tracing-eval-cache true \
        --extra-experimental-features 'nix-command' \
        --expr "import $WORK/proj/expr.nix" 2>&1
}

check() {
    local label="$1" expected="$2" actual="$3"
    if [[ "$actual" != "$expected" ]]; then
        echo "FAIL $label: expected $expected, got $actual" >&2
        exit 1
    fi
    echo "OK   $label: $actual"
}

check "cold"                 '"data1\n|flagA\n"' "$(eval)"
check "warm-same"            '"data1\n|flagA\n"' "$(eval)"
sleep 1; echo "data2" > data.txt
check "after data-edit"      '"data2\n|flagA\n"' "$(eval)"
check "warm with new data"   '"data2\n|flagA\n"' "$(eval)"
sleep 1; echo "flagB" > flag.txt
check "after flag-edit"      '"data2\n|flagB\n"' "$(eval)"
sleep 1; echo "data1" > data.txt
check "after data revert"    '"data1\n|flagB\n"' "$(eval)"

echo
echo "=== Pre-compact stats ==="
nix eval-cache stats --extra-experimental-features nix-command

echo
echo "=== compact-all ==="
nix eval-cache compact-all --extra-experimental-features nix-command

echo
echo "=== Post-compact stats ==="
nix eval-cache stats --extra-experimental-features nix-command

echo
echo "=== Re-verify correctness on the compacted cache ==="
sleep 1; echo "data1" > data.txt; echo "flagA" > flag.txt
check "post-compact data1/flagA"   '"data1\n|flagA\n"' "$(eval)"
sleep 1; echo "data2" > data.txt; echo "flagB" > flag.txt
check "post-compact data2/flagB"   '"data2\n|flagB\n"' "$(eval)"
sleep 1; echo "data99" > data.txt; echo "flagX" > flag.txt
check "post-compact novel state"   '"data99\n|flagX\n"' "$(eval)"

echo
echo "=== Final stats (after re-verify evals) ==="
nix eval-cache stats --extra-experimental-features nix-command

echo
echo "ALL CHECKS PASSED"
