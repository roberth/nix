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
echo "=== final DB summary ==="
sqlite3 "$HOME/.cache/nix/eval-tracing-index-v2/index.sqlite" \
  "SELECT 'Bindings: ' || COUNT(*) FROM Bindings;
   SELECT 'PreconditionSets: ' || COUNT(*) FROM PreconditionSets;
   SELECT 'SetResponses: ' || COUNT(*) FROM SetResponses;
   SELECT 'Shortcuts: ' || COUNT(*) FROM Shortcuts;
   SELECT 'Queries (d=0): ' || COUNT(*) FROM Queries WHERE depth=0;
   SELECT 'Queries (d=1): ' || COUNT(*) FROM Queries WHERE depth=1;
   SELECT 'Results: ' || COUNT(*) FROM Results;"

echo
echo "ALL CHECKS PASSED"
