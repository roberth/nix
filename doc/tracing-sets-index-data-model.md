# Trace Index Data Model (Sets-Based)

## Motivation

The tracing cache layer records cache traces as it observes a Nix interpreter that, treated as a black box, computes several answers concurrently and interleaves their environment queries into a single event stream.
While at most one d=0 Query is outstanding, every Query and Response that the recorder observes is attributable to it, but once a second d=0 Query begins before the first responds, the recorder can no longer attribute subsequent Queries and Responses to a specific d=0 Query.
The existing trace index ([`tracing-index-data-model.md`](tracing-index-data-model.md)) records this stream as a temporal trie where each Query and Response is a node and edges link an observation to its temporal successor.
This adds unnecessary ordering information that causes excessive backtracking on replay.
The sets-based model drops the temporal order and indexes each d=0 Response by the unordered set of (d>0 Query, d>0 Response) pairs it depends on.
The aim is for lookup to descend a decision tree keyed on these pairs and reach a cached Response without traversing unrelated branches.

## Abstract Model

- **QuerySet** — an unordered set of d>0 Queries.
- **ResponseSet** — an unordered set of (d>0 Query, d>0 Response) pairs whose Queries form a QuerySet.
- **Binding** — for a granular Query (identified by its `queryHash`) and a *precondition* ResponseSet, the recorded Response that the box produces when the precondition holds.
- **Decision graph** — for a given `queryHash`, the Hasse diagram of its Bindings under ResponseSet subset. A lookup descends this graph by extending the current ResponseSet until it matches a Binding's precondition. This view is logical, not stored as a separate structure.

## Storage Model

Two layers:

- **HAMT-shared ResponseSets.** A ResponseSet is stored as a hash-array-mapped trie keyed by Query, mapping each member Query to its Response. Construction is by extension from a parent ResponseSet — the new root shares all unchanged structure with the parent. Roots are content-addressed: two ResponseSets with the same `(Query, Response)` members produce the same root hash, so all sharing is automatic across all queries' Bindings. The HAMT is the global storage; `queryHash` indexes and Bindings reference it.
- **Bloom summary per ResponseSet root.** A fixed-width bitmap derived from member Query hashes, stored alongside the HAMT root. Used to reject obvious non-subsets in O(1) before the HAMT walk.

The Bindings table is the per-`queryHash` index into this shared storage:

```
Bindings(
  queryHash       BLOB,   -- granular Query identity (operation, params, inputHashes)
  preconditionRoot BLOB,  -- ResponseSet HAMT root
  responseHash    BLOB,   -- the recorded Response for this Query under this precondition
  PRIMARY KEY (queryHash, preconditionRoot)
)
INDEX BindingsByQueryHash ON Bindings(queryHash);
```

There is no separate "Shortcut" table — `BindingsByQueryHash` *is* the shortcut. The synthetic key remains `queryHash`, exactly as in the existing design; only what lives inside each bucket changes.

## Operations

Notation: `H(set)` is the HAMT root hash of `set`; `B(set)` is its Bloom summary.

### Subset test (the hot inner loop)

```
isSubset(P, C):
  # P precondition, C current; both are ResponseSet roots
  if B(P) & B(C) != B(P): return false           # Bloom rejects definite non-subsets
  for (q, r) in walk(P):                          # HAMT iteration over P's members
    if lookup(C, q) != r: return false            # HAMT lookup in C
  return true
```

Cost: Bloom step is O(1). HAMT step is O(|P|·log n) where n is the HAMT branching factor (constant in practice). Total: O(|P|).

### Replay lookup

```
lookupReplay(queryHash, C):
  candidates = SELECT preconditionRoot, responseHash
               FROM Bindings WHERE queryHash = ?
  for (P, response) in candidates:
    if isSubset(P, C):
      return response
  return MISS
```

Cost: O(k · |P_avg|) where k = |candidates for this queryHash|. k grows with how many distinct preconditions have been recorded for the same Query.

### Recording

```
startRecording(queryHash, startingResponseSet):
  return { qh: queryHash, observed: startingResponseSet, base: startingResponseSet }

observeChildQuery(rec, childQh, childResponse):
  rec.observed = extend(rec.observed, childQh, childResponse)
  # extend returns a new HAMT root sharing structure with rec.observed.

finalizeRecording(rec, finalResponse):
  insertBinding(rec.qh, precondition = rec.observed, response = finalResponse)
```

A precondition is exactly what the recorder observed during the Query's evaluation: the set of (d>0 Query, d>0 Response) pairs the box asked while computing the Response.

### Binding insertion

```
insertBinding(qh, precondition, response):
  ensureHAMTRoot(precondition)       # idempotent: writes if not present
  ensureBloom(precondition)          # idempotent
  INSERT OR IGNORE INTO Bindings(qh, H(precondition), H(response))
```

Idempotent on `(queryHash, preconditionRoot)`: a recording that observed the same precondition and response as a prior one is a no-op write.

### HAMT extension

```
extend(C, q, r):
  return hamtInsert(C, q, r)
  # hamtInsert produces a new root that shares all unchanged nodes with C.
  # New nodes are written to the HAMT storage if not already present
  # (content-addressed).
```

Cost: O(log n) new nodes per extension (path-copy along the trie path to the inserted key).

## Cost analysis

For an evaluation that issues N Queries against a cache with average k Bindings per queryHash and average precondition size |P|:

- **Per Query lookup**: O(k · |P|). The Bloom prefilter rejects unrelated preconditions in O(1); confirmed candidates cost O(|P|) HAMT lookups.
- **Per Query insert** (cache miss path): O(|P| · log n) for HAMT extension, O(1) for Bloom update, O(1) for Binding insert.
- **Per evaluation total**: O(N · k_avg · |P_avg|).

For typical workloads (repeated runs of the same expressions on stable sources), k_avg stays small because identical recordings collapse via content addressing. |P_avg| is bounded by the eval depth at which the Query is issued. The hot-path cost is therefore close to O(N).

Pathological growth modes worth flagging:

- **k_avg blowup**: a single queryHash recorded under many distinct preconditions (e.g. across many configurations). Mitigations: LRU eviction on Bindings, or an inverted index `(queryHash, memberQuery) → Bindings` so lookup can pick the most selective member of `C` to narrow the candidate set.
- **|P_avg| blowup**: preconditions grow with the depth at which a Query is issued and with concurrent in-flight Queries (see "Recording attribution" below). Intersection-learning future work shrinks them.
- **Bloom false positives**: with a fixed-width filter, false-positive rate is `(1 − e^(−mk/n))^k` for n bits, k hashes, m members. Tuning is straightforward; recommend 256 bits and 4 hashes for member counts up to ~100.

## Recording attribution

The recorder observes a single event stream from the interpreter. Attributing each d>0 event to the right pending Query's precondition is exact when at most one Query is outstanding, and an over-approximation when multiple are in flight (each in-flight Query absorbs every observed d>0 event into its precondition).

Over-approximation is sound: the recorded precondition is always a superset of the Query's true dependencies, so lookup still validates correctly when current `C ⊇ precondition`. The cost is bloated precondition size and reduced hit rate, both of which the intersection-learning future work is designed to recover.

## Known limitations and future work

- **Intersection learning**: when two recordings of the same queryHash produce different preconditions but identical Responses, the actually-required precondition is their intersection. Recording this back as a third (smaller) precondition lets future lookups match more contexts. Deferred to v2.
- **Prefetch hints**: per-queryHash union of past recordings' precondition QuerySets, used to fire d>0 lookups concurrently rather than waiting for the box to ask each in turn. Pure perf, layered on top.
- **Cross-feed from legacy trie walker**: when the temporal-trie walker validates d>0 events on the path to a shortcut Result, those (queryHash, responseHash) pairs should be added to `currentSetMembers` so subsequent sets-based lookups see them. The current implementation only cross-feeds the top-level Query's (queryHash, responseHash) pair, leaving sub-queries that depend on intermediate d>0 events to fall back to the trie. Perf only — correctness is preserved because the trie still hits.
- **GC**: Bindings with no recent matches and HAMT nodes with no live references should be evicted. Deferred.
- **`d=2` (ambient incoming)**: handled out of band by a different mechanism, not modelled here.
- **`builtins.cache`**: requires extra theory around explicit user-controlled cache scopes; out of scope for the initial implementation.

## Observed validation results (v1)

Synthetic test fixture (two file reads + concatenation, mutated across six invocations):

- All six invocations return correct values across cold/warm/edit/revert scenarios.
- 6 Bindings recorded across 4 distinct (data, flag) content combinations.
- The dominant queryHash gets the same Response across all 4 preconditions (a structural query whose answer doesn't actually depend on the file contents) — concrete evidence of the *wide precondition* pattern that intersection learning addresses.
- 5 PreconditionSets stored: 1 empty + 4 with 3 members each (the recorder pulls in two file reads + the parent eval per recording context).

NixOS bench (`nixosConfigurations.test...drvPath`):

- Cold: 14.6s (recording).
- Warm: 60ms across three consecutive runs (≈ 240× speedup).
- DB size after one cold+warm cycle: 80 KB.

History-walking bench (5 sequential nixpkgs commits, `lib.version`, same on-disk path so paths match across commits):

| Commit | Time | Bindings added |
|--------|------|----------------|
| b2d6ae39 (cold)              | 1.544s | +12 |
| 1da9d535                     | 0.069s | 0   |
| 08325fd4                     | 1.492s | +2  |
| bf574729                     | 1.503s | +1  |
| 2670f356                     | 1.502s | +1  |

Commit 2 has lib content identical to commit 1 (its diff touches nixos modules only), so it hits the cache fully: zero new bindings, ~22× warm speedup. Subsequent commits add only the bindings that genuinely changed — concrete demonstration of incremental cross-commit reuse. The cache stayed at 1 MB after walking 5 commits.

Extended to 10 commits, 3 commits hit fully cached (~70ms each) — the ones whose diffs don't touch `lib/`. Total cache grew from 408 KB to 1.6 MB, averaging ~150 KB per non-trivial commit. The growth is dominated by the legacy temporal-trie tables, not the sets-based index:

- Legacy trie tables (Queries, Results, Shortcuts, + their indexes): ~2 MB (86% of storage)
- Sets-based tables (PreconditionSets, SetResponses, Bindings, + their indexes): ~318 KB (14% of storage)

When the sets-based path serves all lookups (no fall-through to the trie walker), the per-event d>0 Query and Result writes become redundant and can be dropped — a future optimization.

Cross-branch reuse (evaluating `lib.version` against upstream/main then against lazy-paths-v3, both branches having slightly different `.version` content):

| Branch | Result hash | Bindings added |
|--------|-------------|----------------|
| upstream/main      | 77591fa322de | +3 (cold) |
| lazy-paths-v3      | 410e4847695f | +1        |

The two branches produce different `lib.version` outputs, yet switching from upstream/main's cached state to lazy-paths-v3 only added one new binding (the one whose precondition includes the changed `.version` file). All other intermediate Queries hit the cache.

Bench harnesses live at `/tmp/sets-validation/` in the sandbox: `synthetic.sh` (correctness on edit/revert), `git-history-bench.sh` (cross-commit cache reuse), `multi-branch-bench.sh` (cross-branch cache reuse). They're sandbox-specific (hardcoded paths) and not yet integrated into the test suite.


