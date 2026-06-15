# Shared environment derivation for tracing-cache bench scripts.
# Sourced from each script.

# Derive NIX_REPO from this file's location if not set.
if [[ -z "${NIX_REPO:-}" ]]; then
    here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    NIX_REPO="$(cd "$here/../../.." && pwd)"
fi

NIX_BIN_DIR="${NIX_BIN_DIR:-$NIX_REPO/build/src/nix}"
NIX_LIB_DIR="${NIX_LIB_DIR:-$NIX_REPO/build/src}"

if [[ ! -x "$NIX_BIN_DIR/nix" ]]; then
    echo "error: $NIX_BIN_DIR/nix not executable; set NIX_BIN_DIR or build the repo at $NIX_REPO" >&2
    exit 1
fi

# Add the lib*/ subdirectories under NIX_LIB_DIR.
for d in "$NIX_LIB_DIR"/lib*; do
    [[ -d "$d" ]] && LD_LIBRARY_PATH="${LD_LIBRARY_PATH:+$LD_LIBRARY_PATH:}$d"
done
export LD_LIBRARY_PATH
export PATH="$NIX_BIN_DIR:$PATH"
