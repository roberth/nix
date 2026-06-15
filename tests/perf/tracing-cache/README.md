# Tracing eval cache performance + correctness benches

Bench harnesses for the sets-based tracing eval cache (see
`doc/tracing-sets-index-data-model.md`). These are developer tools, not
automated tests — they require pointing at a built `nix` binary and a
target git repository.

## Common environment

All scripts expect:

- `NIX_BIN_DIR` — directory containing the `nix` binary to test (e.g.
  `$nix_repo/build/src/nix`).
- `NIX_LIB_DIR` — directory containing libraries the `nix` binary needs
  on `LD_LIBRARY_PATH` (e.g. `$nix_repo/build/src/lib*`).

If both are unset the scripts will try to derive them from `$NIX_REPO`
(defaults to `$PWD/../../..` from this directory, i.e. the nix repo
this file lives in).

## Scripts

### `synthetic.sh` — correctness on file edits and reverts

Builds a small fixture (two file reads + string concat), evals it cold,
mutates files, re-evals, and asserts the cache returns the correct value
across cold/warm/edit/revert scenarios. Exits non-zero on any incorrect
result.

```
bash synthetic.sh [work_dir]
```

### `git-history-bench.sh` — cross-commit cache reuse

Walks N commits of a git repo, checks each out in place on a clone
(shared paths so the cache's `inputHashes` match across commits),
evaluates a fixed expression with the tracing eval cache enabled and
shared. Reports per-commit timing, cache size, and Bindings delta.

```
bash git-history-bench.sh [repo_path] [n_commits] [expr]
```

### `multi-branch-bench.sh` — cross-branch cache reuse

Evals a fixed expression against a sequence of git branches, sharing
the cache. Reports per-branch result hash, timing, cache size, and
Bindings delta.

```
bash multi-branch-bench.sh [repo_path] [expr]
```

## A note on filesystem mtime granularity

The synthetic harness inserts `sleep 1` between file mutations
because the `readFile` accessor caches responses by mtime, and a
write that happens within the same second as a prior write to the
same path leaves mtime unchanged — the cache then serves the stale
value. The bench harnesses already insert the sleep where needed.
If you script your own evals against this cache, hold the same
discipline (or `touch -d` the file with an explicit later mtime).

## Interpreting results

- **Bindings delta** is the most informative metric: it tells you how
  many *new* per-query bindings the cache had to record. Zero means a
  full cache hit (the entire eval reused prior recordings).
- **Cache size** in KB reveals storage growth. The legacy temporal
  trie tables dominate; sets-based contributes ~14% by current
  measurements.
- **Timing** includes the `nix` binary's startup overhead (~50ms),
  so warm-hit timings cluster at 60-80ms.
