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
echo "data2" > data.txt
check "after data-edit"      '"data2\n|flagA\n"' "$(eval)"
check "warm with new data"   '"data2\n|flagA\n"' "$(eval)"
echo "flagB" > flag.txt
check "after flag-edit"      '"data2\n|flagB\n"' "$(eval)"
echo "data1" > data.txt
check "after data revert"    '"data1\n|flagB\n"' "$(eval)"

# nix eval-cache stats and compact-all subcommands were removed with
# the eval-cache subcommand rework; restoring introspection tooling
# is tracked as future work in tracing-eval-cache-primop.md. Read the
# row counts directly from SQLite for the stats we can still capture.

db_stats() {
    local label="$1"
    # Path per tracing-decision-graph.cc's dg_defaultDbPath: an
    # override lives at $NIX_TRACING_CACHE_DIR/decision-graph.sqlite;
    # default at $XDG_CACHE_HOME/eval-tracing-decision-graph/index.sqlite.
    local db
    if [[ -n "${NIX_TRACING_CACHE_DIR:-}" ]]; then
        db="$NIX_TRACING_CACHE_DIR/decision-graph.sqlite"
    else
        db="${XDG_CACHE_HOME:-$HOME/.cache}/nix/eval-tracing-decision-graph/index.sqlite"
    fi
    [[ -f "$db" ]] || { echo "$label: db not present at $db"; return; }
    sqlite3 "$db" "
        SELECT '$label: ask=' || (SELECT COUNT(*) FROM Ask) ||
                     ' terminal=' || (SELECT COUNT(*) FROM Terminal) ||
                     ' requests=' || (SELECT COUNT(*) FROM Requests) ||
                     ' results=' || (SELECT COUNT(*) FROM Results);
    "
}

echo
echo "=== Row counts (via sqlite) ==="
db_stats "post-eval"

echo
echo "=== Re-verify correctness across edit sequences ==="
echo "data1" > data.txt; echo "flagA" > flag.txt
check "data1/flagA"   '"data1\n|flagA\n"' "$(eval)"
echo "data2" > data.txt; echo "flagB" > flag.txt
check "data2/flagB"   '"data2\n|flagB\n"' "$(eval)"
echo "data99" > data.txt; echo "flagX" > flag.txt
check "novel state"   '"data99\n|flagX\n"' "$(eval)"

echo
db_stats "final"

echo
echo "ALL CHECKS PASSED"
