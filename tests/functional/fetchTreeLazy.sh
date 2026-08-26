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

# `builtins.storePath` accepts the lazy form's `outPath`. That value is
# a path rooted at the fetcher's accessor, where the store path only
# shows up once the path is rendered as a string; the result must be
# indistinguishable from the eager form's, string context included.
lazyStorePath=$(nix eval --impure --extra-experimental-features "$xpFlags" --raw \
    --expr "builtins.storePath (builtins.fetchTree { type = \"path\"; path = \"$src\"; lazy = true; }).outPath")
eagerStorePath=$(nix eval --impure --extra-experimental-features "$xpFlags" --raw \
    --expr "builtins.storePath (builtins.fetchTree { type = \"path\"; path = \"$src\"; }).outPath")
[[ "$lazyStorePath" = "$eagerStorePath" ]]

lazyContext=$(nix eval --impure --extra-experimental-features "$xpFlags" --json \
    --expr "builtins.getContext (builtins.storePath (builtins.fetchTree { type = \"path\"; path = \"$src\"; lazy = true; }).outPath)")
eagerContext=$(nix eval --impure --extra-experimental-features "$xpFlags" --json \
    --expr "builtins.getContext (builtins.storePath (builtins.fetchTree { type = \"path\"; path = \"$src\"; }).outPath)")
[[ "$lazyContext" = "$eagerContext" ]]

# `storePath` promises a store path that is actually there, so the
# deferred copy must have happened by the end of evaluation.
nix path-info --extra-experimental-features "$xpFlags" "$lazyStorePath"

# Coercing the argument to a string must not make a relative path
# absolute. `CanonPath` reroots one at `/`, which would let `storePath`
# accept a path the caller never named -- and this one would land in the
# store.
expectStderr 1 nix eval --impure --extra-experimental-features "$xpFlags" --raw \
    --expr "builtins.storePath \"${lazyStorePath#/}\"" \
    | grepQuiet "doesn't represent an absolute path"

# `lazy` must be a Boolean.
expectStderr 1 nix eval --impure --extra-experimental-features "$xpFlags" \
    --expr "builtins.fetchTree { type = \"path\"; path = \"$src\"; lazy = \"yes\"; }" \
    | grepQuiet "argument 'lazy'"
