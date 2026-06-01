#!/usr/bin/env bash

# `builtins.fetchTree { ...; lazy = true; }` returns a tree attrset
# whose `outPath` is a Nix path value (not a store-path string). All
# other attributes are emitted identically to the legacy form.

source common.sh

src="$TEST_ROOT/lazy-src"
mkdir -p "$src"
echo "hello" > "$src/payload"
touch -t 202301010000 "$src/payload"

xpFlags="flakes nix-command fetch-tree"

# typeOf .outPath is "path" with `lazy = true`, "string" without.
[[ "$(nix eval --impure --extra-experimental-features "$xpFlags" --raw \
    --expr "builtins.typeOf (builtins.fetchTree { type = \"path\"; path = \"$src\"; lazy = true; }).outPath")" \
    = path ]]

[[ "$(nix eval --impure --extra-experimental-features "$xpFlags" --raw \
    --expr "builtins.typeOf (builtins.fetchTree { type = \"path\"; path = \"$src\"; }).outPath")" \
    = string ]]

# Despite the different shape, both forms resolve to the same store
# path when coerced: the lazy form goes through copyPathToStore + the
# fingerprint short-circuit, the eager form through mkStorePathString.
lazy=$(nix eval --impure --extra-experimental-features "$xpFlags" --raw \
    --expr "\"\${(builtins.fetchTree { type = \"path\"; path = \"$src\"; lazy = true; }).outPath}\"")
eager=$(nix eval --impure --extra-experimental-features "$xpFlags" --raw \
    --expr "(builtins.fetchTree { type = \"path\"; path = \"$src\"; }).outPath")
[[ "$lazy" = "$eager" ]]

# Other attributes that the input already carries (narHash supplied
# explicitly here so both forms have it; lazy mode doesn't compute it
# from scratch the way `mountInput` does in the eager form, which is
# the whole point of being lazy).
narHashSRI=$(nix eval --impure --extra-experimental-features "$xpFlags" --raw \
    --expr "(builtins.fetchTree { type = \"path\"; path = \"$src\"; }).narHash")
lazyHash=$(nix eval --impure --extra-experimental-features "$xpFlags" --raw \
    --expr "(builtins.fetchTree { type = \"path\"; path = \"$src\"; lazy = true; narHash = \"$narHashSRI\"; }).narHash")
[[ "$lazyHash" = "$narHashSRI" ]]

# `import` of a lazy path goes through the accessor: a sibling `.nix`
# file is readable without forcing a store copy of the whole tree.
echo "{ value = 42; }" > "$src/expr.nix"
[[ "$(nix eval --impure --extra-experimental-features "$xpFlags" --raw \
    --expr "toString (import ((builtins.fetchTree { type = \"path\"; path = \"$src\"; lazy = true; }).outPath + \"/expr.nix\")).value")" \
    = 42 ]]

# `lazy` must be a Boolean.
expectStderr 1 nix eval --impure --extra-experimental-features "$xpFlags" \
    --expr "builtins.fetchTree { type = \"path\"; path = \"$src\"; lazy = \"yes\"; }" \
    | grepQuiet "argument 'lazy'"
