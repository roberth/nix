# Trace Index Data Model (Sets-Based)

## Try it

With a build of this branch (`build/src/nix/nix` on `PATH`,
`build/src/lib*` on `LD_LIBRARY_PATH`):

```sh
# Cold + warm an eval with the tracing cache enabled.
nix eval --impure --option tracing-eval-cache true \
    .#nixosConfigurations.X.config.system.build.toplevel.drvPath

# Inspect what landed in the sets-based index.
nix eval-cache stats

# Run intersection-learning + GC + VACUUM across every queryHash.
nix eval-cache compact-all

# Or, on a single queryHash you got from another inspection tool:
nix eval-cache compact <queryHash>
```

The bench harnesses under `tests/perf/tracing-cache/` (synthetic
file-edit correctness, git-history cross-commit reuse, multi-branch
cross-branch reuse) exercise the cache end-to-end against any local
nix repo.

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

Three SQLite tables:

```
PreconditionSets(
  setHash  BLOB PRIMARY KEY,  -- SHA-256 of the sorted members CBOR
  members  BLOB NOT NULL,     -- CBOR of [(queryHash, responseHash), ...] sorted by queryHash
  bloom    BLOB               -- 256-bit Bloom summary of member queryHashes
)

SetResponses(
  responseHash BLOB PRIMARY KEY,
  payload      BLOB NOT NULL  -- CBOR-serialised d=0 Response
)

Bindings(
  queryHash        BLOB NOT NULL,
  preconditionHash BLOB NOT NULL,  -- references PreconditionSets.setHash
  responseHash     BLOB NOT NULL,  -- references SetResponses.responseHash
  createdAt        INTEGER DEFAULT (unixepoch()),
  PRIMARY KEY (queryHash, preconditionHash)
)
CREATE INDEX BindingsByQueryHash ON Bindings(queryHash);
```

PreconditionSets and SetResponses are content-addressed: two recordings that observe the same precondition (or produce the same response) share storage automatically via `INSERT OR IGNORE` on the hash key. Bindings is the per-`queryHash` index into the two content-addressed pools.

There is no separate "Shortcut" table — `BindingsByQueryHash` *is* the shortcut. The synthetic key remains `queryHash`, exactly as in the existing design; only what lives inside each bucket changes.

The current implementation stores each PreconditionSet as a single flat CBOR blob rather than a HAMT — see *HAMT-shared ResponseSets* under Known limitations for why structural sharing across sets was deferred.

## Operations

Notation: `H(set)` is the SHA-256 of `set`'s sorted CBOR encoding; `B(set)` is its Bloom summary; `mems(set)` is its decoded sorted member list.

### Subset test (the hot inner loop)

```
isSubset(P, C):
  # P precondition, C current; both as decoded sorted member lists
  if B(P) & B(C) != B(P): return false   # Bloom rejects definite non-subsets
  # Linear two-pointer merge: every (q, r) in P must appear in C.
  i = j = 0
  while i < |P|:
    if j == |C|: return false
    if P[i].q == C[j].q:
      if P[i].r != C[j].r: return false
      i += 1; j += 1
    elif C[j].q < P[i].q: j += 1
    else: return false
  return true
```

Cost: Bloom is O(1) on a 32-byte word; the merge is O(|P| + |C|).

### Replay lookup

```
lookupReplay(queryHash, C):
  # Pull (preconditionHash, responseHash, bloom) for this queryHash in one join.
  candidates = SELECT b.preconditionHash, b.responseHash, p.bloom
               FROM Bindings b LEFT JOIN PreconditionSets p
                 ON b.preconditionHash = p.setHash
               WHERE b.queryHash = ?
  for (preHash, response, bloom) in candidates:
    if not bloomMayBeSubset(bloom, B(C)): continue   # cheap reject
    P = mems(getPreconditionSet(preHash))            # blob load + CBOR decode
    if isSubset(P, C): return response
  return MISS
```

Cost: O(k · (|P| + |C|)) worst case, where k = candidates for this queryHash. The Bloom prefilter cuts most candidates to O(1) once preconditions diverge enough.

### Recording

```
startRecording(queryHash, startingResponseSet):
  return { qh: queryHash, observed: copy(startingResponseSet) }

observeChildQuery(rec, childQh, childResponse):
  insertSorted(rec.observed, (childQh, childResponse))

finalizeRecording(rec, finalResponse):
  insertBinding(rec.qh, precondition = rec.observed, response = finalResponse)
```

A precondition is exactly what the recorder observed during the Query's evaluation: the set of (d>0 Query, d>0 Response) pairs the box asked while computing the Response.

### Binding insertion

```
insertBinding(qh, precondition, response):
  setHash      = H(precondition)
  responseHash = SHA-256(response.payload)
  INSERT OR IGNORE INTO PreconditionSets(setHash, members, bloom)
    VALUES (setHash, CBOR(precondition), bloomOf(precondition))
  INSERT OR IGNORE INTO SetResponses(responseHash, payload)
  INSERT OR IGNORE INTO Bindings(qh, setHash, responseHash)
```

Idempotent on `(queryHash, preconditionHash)`: a recording that observed the same precondition and response as a prior one is a no-op write.

## Cost analysis

For an evaluation that issues N Queries against a cache with average k Bindings per queryHash and average precondition size |P|:

- **Per Query lookup**: one indexed `SELECT` on Bindings (O(log total_bindings)); for each candidate, an O(1) Bloom test and (if it passes) an O(|P| + |C|) merge after a blob load.
- **Per Query insert** (cache miss path): three `INSERT OR IGNORE`s, each O(log total) on their respective primary-key indexes; serialising the precondition CBOR is O(|P|).
- **Per evaluation total**: O(N · k_avg · (|P_avg| + |C_avg|)) in the worst case; k_avg is suppressed in practice by content-addressed deduplication of identical preconditions.

For typical workloads (repeated runs of the same expressions on stable sources), k_avg stays small because identical recordings collapse via the `INSERT OR IGNORE` on `(queryHash, preconditionHash)`. |P_avg| is bounded by the eval depth at which the Query is issued. The hot-path cost is therefore close to O(N).

Pathological growth modes worth flagging:

- **k_avg blowup**: a single queryHash recorded under many distinct preconditions (e.g. across many configurations). Mitigations: time-based eviction on Bindings, or an inverted index `(queryHash, memberQuery) → Bindings` so lookup can pick the most selective member of `C` to narrow the candidate set.
- **|P_avg| blowup**: preconditions grow with the depth at which a Query is issued and with concurrent in-flight Queries (see "Recording attribution" below). Intersection-learning (now shipped — see Known limitations) shrinks them by deriving smaller equivalent preconditions from pairs of same-response Bindings.
- **Bloom false positives**: with a fixed-width filter, false-positive rate is `(1 − e^(−mk/n))^k` for n bits, k hashes, m members. The current 256-bit / 4-hash configuration is fine for member counts up to ~100; larger preconditions would warrant widening.

## Recording attribution

The recorder observes a single event stream from the interpreter. Attributing each d>0 event to the right pending Query's precondition is exact when at most one Query is outstanding, and an over-approximation when multiple are in flight (each in-flight Query absorbs every observed d>0 event into its precondition).

Over-approximation is sound: the recorded precondition is always a superset of the Query's true dependencies, so lookup still validates correctly when current `C ⊇ precondition`. The cost is bloated precondition size and reduced hit rate, both of which the intersection-learning future work is designed to recover.

## Known limitations and future work

- **Intersection learning (implemented)**: when two recordings of the same queryHash produce different preconditions but identical Responses, the actually-required precondition is their intersection. `runLearningPass` (invoked by `compactAll` or `nix eval-cache compact <queryHash>`) records that intersection as a third Binding and then evicts the two original Bindings strictly subsumed by it. Bench-validated: a 6-invocation synthetic walk collapses 6 Bindings → 3 post-compact.
- **HAMT-shared ResponseSets**: stored preconditions could share structure across recordings via a HAMT keyed by Query, so an `extend` operation would write O(log n) new nodes rather than re-CBOR-encoding the whole set. The current flat-CBOR-blob storage avoided the implementation cost at the price of duplicated bytes across overlapping preconditions. Worth revisiting if profiling on long-lived caches shows `PreconditionSets` bytes dominating.
- **Prefetch hints**: per-queryHash union of past recordings' precondition QuerySets, used to fire d>0 lookups concurrently rather than waiting for the box to ask each in turn. Pure perf, layered on top.
- **Bloom prescreen (implemented)**: a 256-bit Bloom filter is stored alongside every PreconditionSet and used in `lookupSetsReplay` to skip candidates whose precondition is definitely not a subset of the current context. At the current bench scale (single-digit candidates per queryHash) the prescreen doesn't show measurable timing impact because the per-candidate subset test was already fast; the benefit shows up once a single queryHash accumulates many recorded Bindings (e.g. after extensive intersection learning across long-lived caches). The skip counter is reported by `nix eval-cache stats` (`lookupBloomPrescreenSkips`) so its effectiveness can be measured per session.
- **compactAll cost (measured)**: on a representative history-bench cache (3 queryHashes, 9 Bindings, 287 SetResponses, 1.66 MB DB) the first compactAll completes in 57 ms (learning + eviction + GC + VACUUM). The idempotent second run is 54 ms — essentially the cost of opening the DB, counting rows, and discovering nothing changed. Fast enough that an opt-in end-of-session auto-trigger is plausible; on by default would still add 50 ms to short-eval shutdowns.
- **Cross-feed from legacy trie walker (implemented)**: when the temporal-trie walker validates a shortcut Result, every validated d>0 event along the chain *and* the top-level Query's (queryHash, responseHash) pair are appended to `currentSetMembers`, so downstream sets-based lookups can match preconditions whose context was populated by a trie hit. Reduces the asymmetry where switching between trie and sets-based paths within one eval would lose context.
- **GC (implemented)**: `compactAll` runs the learning pass (which evicts Bindings strictly subsumed by an intersection of two same-Response Bindings) and then `runGC`, which deletes PreconditionSets and SetResponses no longer referenced by any Binding and `VACUUM`s the database. Time-based Binding eviction (drop entries with no matches in N invocations) is not yet implemented and would need a per-Binding `lastHit` column.
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

Bench harnesses live under [`tests/perf/tracing-cache/`](../tests/perf/tracing-cache/) — three scripts (`synthetic.sh`, `git-history-bench.sh`, `multi-branch-bench.sh`) plus a shared `common.sh` that derives the nix binary location from the repo layout. They're developer tools, not CI-automated tests; the readme covers usage and result interpretation.

### Intersection learning observed effect

The `nix eval-cache compact` and `compact-all` subcommands run the intersection-learning pass (see `runLearningPass` in `tracing-index.hh`). Empirical findings:

- **Single fresh eval (NixOS bench, 17 queryHashes × 1 binding each)**: 0 intersections found. Learning needs ≥2 bindings per queryHash to produce evidence.
- **History walk (10 nixpkgs commits on `lib.version`)**: dominant queryHash had 7 bindings (one per content-state variation); learning inserted 6 intersected bindings. Other queryHashes with single bindings were unchanged.
- **Synthetic test (6 invocations across 4 content states)**: dominant structural queryHash had 4 bindings (all same Response); learning inserted 5 intersected bindings.

In all scenarios learning is **purely additive** — the wider preconditions are not evicted. This grows the `PreconditionSets` table (466 KB after the 10-commit history walk + compact) but enables hits in narrower contexts. (Update: eviction of subsumed bindings is now implemented; see "Try it" → `compact-all`.)

**Safety of empty-precondition Bindings.** After learning, some Bindings collapse to an empty precondition — a "matches any context" hit. This is *safe* because the `queryHash` itself carries Merkle provenance via `inputHashes`: a queryHash refers to a particular Query asked on a specific cached Result, so the empty-precondition Binding only fires for lookups on that exact identity. Cross-context contamination is structurally impossible — the synthetic test confirms this by correctly returning `data99|flagX` against a cache that has an empty-precondition Binding for the earlier `data1|flagA` result, because the two evaluations produce different intermediate Results with different `inputHashes`.


