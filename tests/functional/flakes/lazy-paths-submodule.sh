#!/usr/bin/env bash

# End-to-end proof of the lazy-paths invariant against a git input that
# has a submodule. The shape:
#
#   consumer flake
#     └─ inputs.parent (git+file with submodules=1)
#         └─ sub (git submodule)
#
# After the consumer is initially locked, the test makes the
# submodule's storage unable to serve blob contents (partial clone
# filtered to `blob:none`). Tree objects survive — git can discover
# what the submodule directory should look like — but reading any
# file inside it fails.
#
# Three claims, with different requirement status:
#
#  (1) DEFINITIVE — the lazy invariant.
#      Reading a path-typed file rooted on the parent's own
#      (non-submodule) tree evaluates successfully. The lazy
#      `readFile` only touches the parent's blobs; the submodule's
#      missing blobs are never demanded. If this regresses, lazy paths
#      stopped doing their job.
#
#  (2) DEFINITIVE — loud-failure-on-missing-data.
#      Reading a path inside the submodule fails with a missing-blob
#      error, not silently with empty content or with stale data from
#      somewhere else. This also self-validates the test setup: it
#      proves the gitv3 corruption took and that no other data source
#      is masking the missing blob.
#
#  (3) DEFINITIVE — upstream-shape async materialisation.
#      Coercing `parent.outPath` to a string returns the predicted
#      storePath (derived from the lockfile narHash via the
#      fixed-output formula, with no walk). That string lands on
#      stdout. Only afterwards, when `ensureLazyPathsCopied` runs, do
#      we walk the accessor for real — and that walk hits the missing
#      blob. Net result: stdout has the storePath, stderr has the
#      error, exit code is non-zero.
#
#  (4) DEFINITIVE — read-only is a no-op for materialisation.
#      Same expression with `--read-only`: `ensureLazyPathsCopied`
#      early-returns when `settings.isReadOnly()` is set, so no walk
#      ever happens. Eval prints the storePath and exits cleanly. The
#      submodule's missing blob is never demanded.

source ./common.sh

requireGit

# Force file:// URLs through the gitv3 cache path. Without this, the
# git fetcher special-cases file:// to read the source directory
# directly, skipping gitv3 entirely — which would mean there's no
# cache for us to surgically corrupt below.
export _NIX_FORCE_HTTP=1

root="$TEST_ROOT/lazypaths-submodule"
rm -rf "$root"
mkdir -p "$root"

# ── Phase 1: set up sub-source, parent (with submodule), consumer ──

sub="$root/sub"
git init -q -b master "$sub"
echo "submodule content" > "$sub/value.txt"
git -C "$sub" add value.txt
git -C "$sub" -c user.email=t@t -c user.name=T commit -q -m initial

parent="$root/parent"
git init -q -b master "$parent"
echo "parent content" > "$parent/parent-only.txt"
cat > "$parent/flake.nix" <<'EOF'
{
  outputs = { self }: {
    # Reads via path-Values rooted on `self`'s accessor; no
    # `self.outPath` involved, so no store materialisation forced.
    parentValue = builtins.readFile ./parent-only.txt;
    subValue = builtins.readFile ./sub/value.txt;
  };
}
EOF
git -C "$parent" -c protocol.file.allow=always submodule add "$sub" sub
git -C "$parent" add parent-only.txt flake.nix
git -C "$parent" -c user.email=t@t -c user.name=T commit -q -m initial

consumer="$root/consumer"
mkdir -p "$consumer"
cat > "$consumer/flake.nix" <<EOF
{
  inputs.parent.url = "git+file://$parent?submodules=1";
  outputs = { self, parent }: {
    parentValue = parent.parentValue;
    subValue = parent.subValue;
    forceWhole = "\${parent.outPath}";
  };
}
EOF

# ── Phase 2: initial lock + sanity check, everything intact ──

nix flake lock "path:$consumer"
[[ -f "$consumer/flake.lock" ]]

[[ "$(nix eval --raw "path:$consumer#parentValue")" = "parent content" ]]
[[ "$(nix eval --raw "path:$consumer#subValue")" = "submodule content" ]]
nix eval --raw "path:$consumer#forceWhole" >/dev/null

# ── Phase 3: surgically strip the submodule's blob from Nix's
#            gitv3 cache (keep commits + trees so the fetcher can
#            still construct an accessor, lose the blob so any read
#            fails). ──

# Find the gitv3 cache dir corresponding to the submodule URL by
# probing each one for the blob we want to remove.
blob_sha="$(git -C "$sub" rev-parse HEAD:value.txt)"
sub_cache=
for cache in "$TEST_HOME/.cache/nix/gitv3"/*; do
    [[ -d "$cache/objects" ]] || continue
    if git -C "$cache" cat-file -e "$blob_sha" 2>/dev/null; then
        sub_cache="$cache"
        break
    fi
done
[[ -n "$sub_cache" ]]

# Rebuild the cache's pack to contain every object EXCEPT blobs.
# `git pack-objects <name>` produces `<name>-<sha>.pack` and `.idx`.
mkdir -p "$sub_cache/objects/pack-new"
git -C "$sub_cache" cat-file --batch-check --batch-all-objects --unordered \
    | awk '$2 != "blob" { print $1 }' \
    | git -C "$sub_cache" pack-objects --quiet "$sub_cache/objects/pack-new/pack" >/dev/null
rm -f "$sub_cache"/objects/pack/pack-*.pack "$sub_cache"/objects/pack/pack-*.idx
mv "$sub_cache"/objects/pack-new/pack-*.pack "$sub_cache"/objects/pack/
mv "$sub_cache"/objects/pack-new/pack-*.idx "$sub_cache"/objects/pack/
rm -rf "$sub_cache/objects/pack-new"

# Also strip any loose copy of the blob (gitv3 may have written some
# objects loose alongside the pack).
rm -f "$sub_cache/objects/${blob_sha:0:2}/${blob_sha:2}"

# Sanity: the cache's commit/tree pair survives, but reading the
# value.txt blob from this gitv3 repo now fails.
git -C "$sub_cache" ls-tree "$(git -C "$sub" rev-parse HEAD)" >/dev/null
git -C "$sub_cache" cat-file -e "$blob_sha" && exit 1

# ── Phase 4: force Nix to re-fetch through gitv3 ──
#
# Clearing the store kills the parent's cached storePath, so the
# fetcher's substitution shortcut at fetchers.cc:317 fails
# (`ensurePath`). The fetcher then falls through to a real fetch,
# which consults gitv3 — where the parent's cached repo is intact and
# the submodule's cache is now blob-stripped.

clearStore

# ── Phase 5: re-eval and assert the three claims ──

# (1) Parent-only readFile succeeds — the lazy path through the
#     parent's accessor doesn't read the submodule's blobs.
[[ "$(nix eval --raw "path:$consumer#parentValue")" = "parent content" ]]

# (2) Submodule readFile fails — the lazy path eventually reaches the
#     submodule's accessor and tries to read a missing blob.
expectStderr 1 nix eval --raw "path:$consumer#subValue" | grepQuiet -i "blob\|object\|missing"

# (3) Forcing the whole tree prints the predicted storePath on stdout,
#     then errors during ensureLazyPathsCopied when the actual walk
#     hits the missing blob. Capture stdout and stderr separately so
#     we can assert both.
force_stderr="$TEST_ROOT/forceWhole.stderr"
force_stdout=$(nix eval --raw "path:$consumer#forceWhole" 2>"$force_stderr") && force_rc=0 || force_rc=$?
[[ "$force_rc" -ne 0 ]]
[[ "$force_stdout" = "$NIX_STORE_DIR"/*-source ]]
grepQuiet -i "blob\|object\|missing" < "$force_stderr"

# (4) Read-only mode: ensureLazyPathsCopied is a no-op. The storePath
#     gets printed and the eval exits cleanly. The missing blob is
#     never demanded.
ro_out=$(nix eval --raw --read-only "path:$consumer#forceWhole")
[[ "$ro_out" = "$NIX_STORE_DIR"/*-source ]]
[[ "$ro_out" = "$force_stdout" ]]
