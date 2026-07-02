# Tracing eval cache (v13)

This document describes the tracing eval cache as it is currently
shipped on the `eval-cache-v13` branch. It supersedes the three legacy
docs now under `doc/outdated/` (`tracing-index-data-model.md`,
`tracing-sets-index-data-model.md`, `tracing-decision-graph-data-model.md`);
those describe earlier attempts (v12 trie, v12.5 flat-CBOR sets, and
a Phase-1 sketch that diverged from the eventual implementation).

## What it is

A persistent cache for `nix eval` / `nix build` / any other
`EvalCommand`-derived CLI. When the evaluator reaches a result for
some query `Q` — `evalFile`, `getAttr`, `apply`, `getString`, etc. —
the cache stores a *trace* of the environment reads that led to that
result. On a subsequent invocation, the cache replays `Q` by walking
the trace against the current environment: each recorded file-read or
env-var lookup is re-issued and its current response compared against
what was recorded. If everything matches, the cache returns the
stored result without invoking the inner evaluator.

The cache treats the inner evaluator as a black box. It doesn't try
to predict, parse, or instrument Nix expressions — it just observes
`(request → response)` pairs at the environment boundary and
remembers which results those pairs preceded.

Enable with `--option tracing-eval-cache true` plus the
`tracing-eval-cache` experimental feature flag. Storage path is
`$NIX_TRACING_CACHE_DIR/decision-graph.sqlite` (defaults to
`$XDG_CACHE_HOME/nix/eval-tracing-decision-graph/index.sqlite`). The
cache is non-destructive: misses fall through to the inner evaluator
and the answer is correct either way.

## Vocabulary

The cache distinguishes two layers of interaction:

- **`Query` / `Result`** — what the outside world (the user, a higher
  layer) asks the evaluator. `evalFile path`, `getAttr name on object
  O`, `apply f x`. Each has a content-addressed `queryHash` (operation
  + parameters + parent's `queryHash` for Merkle provenance) and
  `resultHash`.
- **`Request` / `Response`** — what the evaluator asks the
  environment during evaluation. `read /home/me/file.nix`, `getEnv
  HOME`. Each has its own `requestHash` and `responseHash`.

A **`Fact`** is a `(requestHash, responseHash)` pair — the atomic
unit of "the environment behaved this way at this moment." A
**`FactSet`** is a set of Facts; a **`RequestSet`** is a set of
Request hashes. The cache identifies sets by their content hash and
relies on those identities heavily, so the set hashing scheme matters
(more below).

A trace through the cache for `Q` is a chain:

```
(Q, ∅)
   │ asks: dispatch this RequestSet's Requests, observe Responses
   ▼
(Q, FactSet')
   │ asks: dispatch this RequestSet's Requests
   ▼
   ...
(Q, FactSet_final) ── terminal ──▶ Result R
```

Every trace starts at the empty FactSet `∅`. Phase 1 records a single
`asks` edge per first-time recording covering all remaining
Requests; Patricia split (below) factors shared prefixes when later
recordings overlap.

## Storage layer

Six SQLite tables, all in one file. All are append-only via
`INSERT OR IGNORE`; reads use prepared statements with a per-hash
in-process cache.

```
Requests(requestHash BLOB PRIMARY KEY, payload BLOB)
Queries (queryHash   BLOB PRIMARY KEY, payload BLOB)
Results (resultHash  BLOB PRIMARY KEY, payload BLOB)

RequestSetNodes(nodeHash BLOB PRIMARY KEY, payload BLOB) WITHOUT ROWID

Asks     (queryHash BLOB, factSetHash BLOB, requestSetHash BLOB,
          PRIMARY KEY (queryHash, factSetHash, requestSetHash)) WITHOUT ROWID
Terminals(queryHash BLOB, factSetHash BLOB, resultHash BLOB,
          PRIMARY KEY (queryHash, factSetHash, resultHash))     WITHOUT ROWID
```

`WITHOUT ROWID` on edge tables collapses the data into the primary-key
B-tree, halving on-disk size vs the default heap + duplicate-PK
layout.

Notable absences (intentional):

- **No `Responses` table.** Walk recomputes responses from the live
  environment and compares hashes; the recorded response bytes
  never come back into play. We keep request payloads — walk needs
  the path to dispatch — but not response payloads.
- **No `FactSets` table.** FactSet members are reconstructed
  incrementally in-process during `record()` and `walk()`. The hash
  itself is identity (the `factSetHash` columns above); the members
  don't need to live on disk. Earlier sketches persisted them and
  paid 94% of the DB size for it.

### RequestSet trie

`RequestSetNodes` is the storage for the RequestSet pool: each row is
one trie node, content-addressed by `SHA-256(payload)`. A node is one
of:

```
Leaf:     [0x00] hash_1 hash_2 ... hash_n           (n ≤ TRIE_SPLIT_THRESHOLD)
Internal: [0x01] (bucket_idx_byte || child_hash)+   (sparse, sorted by bucket)
```

Bucket index at depth `d` for an element hash `h` is the `TRIE_RADIX_BITS`
bits of `h` starting at bit `d * TRIE_RADIX_BITS` (MSB first). The
constants live in `src/libexpr/tracing-decision-graph.cc`:
`TRIE_RADIX_BITS = 4` (16-way fanout), `TRIE_SPLIT_THRESHOLD = 16`.
SHA-256 outputs are uniformly random, so buckets balance in
expectation — no content-defined chunking needed.

This shape buys two things:

1. **Structural sharing.** Two RequestSets that overlap share the
   subtree rows for the overlap, automatically via content
   addressing of the node payloads. At K=10000 nixpkgs-attr
   recordings, total DB size is 41 MB; without trie sharing it would
   be ~2 GB of mostly-duplicate RS blobs.
2. **Cheap symmetric difference between any two roots.** Descend
   both tries in parallel; any subtree where the two roots agree on
   the hash collapses to a no-op. Cost is `O(|symmetric diff| ·
   branching)`, not `O(|either set|)`. This is what the replay-side
   fast path (below) keys off.

### FactSet hashing

FactSet hashes are XOR-fold over per-element hashes:

```
H_element(req, resp) = SHA-256(req.bytes || resp.bytes)
H(set)               = XOR over H_element of each member, starting at 0
emptySetHash         = all-zero (XOR identity)
H(S ∪ {e})           = H(S) XOR H_element(e)   when e ∉ S
```

Properties: commutative (order-independent), associative,
identity-with-zero, self-inverse on XOR. Extension is O(1) given the
disjointness invariant `e ∉ S`. Note this only gives the canonical
`H(A ∪ B) = H(A) XOR H(B)` when `A ∩ B = ∅`; the general
`H(A) XOR H(B) = H(A △ B)` (symmetric difference) is what falls out
of XOR over hashes.

Algebraically weaker than a sorted-Merkle hash — a chosen-input
attacker could construct collisions — but the threat model is an
internal cache; a hash collision yields a wrong cache hit detected on
next use, no security impact.

## Recording

`TracingEnvironment` wraps the inner environment (currently
`SystemEnvironment`). Every `getFileHash`, `getEnv`, and ambient
interaction on the wrapped accessor flows through it:

```
TracingEnvironment::getFileHash(path):
    hash = inner.getFileHash(path)
    writer.logResponse(Response<FileReadRequest>{request: {path}, response: {hash}})
    return hash
```

`TracingSourceAccessor` does the same for the underlying
`SourceAccessor` so individual file reads also get logged.

`TracingWriter::logResponse(resp)` does two things:

1. Sink to `TraceFile` (a JSON log under the cache directory, kept
   for debugging and offline analysis — independent of the SQLite DB).
2. Update incremental in-process state used by `record()` later:

```cpp
// per-process state in TracingWriter:
vector<Fact>              v13FactSet;          // insertion order
SetHash                   v13FactSetHash;      // XOR-fold, incremental
unordered_set<Hash>       seenRequests;        // dedup
unordered_map<Hash, Hash> responseFor;         // request → response
TrieBuilder               allRequestsTrie;     // canonical RS hash, incremental
```

Each new fact (one not already in `seenRequests`) is XOR'd into
`v13FactSetHash`, mapped in `responseFor`, and inserted into
`allRequestsTrie` (O(log N) — one path-copy from leaf to root, split
on leaf-overflow).

When the evaluator finishes a query and produces a result,
`TracingEvaluator` (the recording counterpart to
`TracingReplayEvaluator`) calls `writer.logResult(value, result,
queryHandle)`. That handler:

1. Inserts `(resultHash, resultPayload)` into `Results`.
2. Pushes any unpersisted nodes from `allRequestsTrie` into
   `RequestSetNodes`.
3. Primes the in-process FactSet cache with the current `v13FactSet`
   under `v13FactSetHash`.
4. Calls `decisionGraph.record(queryHash, v13FactSetHash, resultHash,
   responseFor, seenRequests, allRequestsTrie.rootHash())` — the
   fastest of three overloads, which uses the precomputed RS hash to
   skip the per-call trie rebuild.

### `record()` algorithm

`record(Q, factSetHash, result, responseFor, allRequests,
allRequestsRsHash)` integrates the recording into the decision graph.
It tracks a local `curRequests` (set of requests "consumed" so far)
and computes a current cur hash via XOR-fold:

```
cur          = ∅
curRequests  = {}
while curRequests ≠ allRequests:
    # Eager Patricia split pass: any existing edge whose useful
    # dispatch partially overlaps `remaining` gets split.
    for rs in Asks(Q, cur):
        useful = members(rs) \ curRequests
        shared = useful ∩ (allRequests \ curRequests)
        if ∅ ⊊ shared ⊊ useful:
            patricia_split(Q, cur, rs, shared)

    # Find a followable edge: usefulDispatch ⊆ remaining
    if some edge e in Asks(Q, cur) has useful(e) ⊆ remaining:
        consume useful(e) into cur, curRequests
    elif curRequests = ∅ and allRequestsRsHash provided:
        # Fast path for first-time recording: jump straight to factSet
        insert Asks(Q, ∅, allRequestsRsHash)
        cur = factSetHash; break
    else:
        # Slow path: insert a new whole-remaining edge
        insert Asks(Q, cur, insertRequestSet(remaining))
        consume remaining into cur, curRequests

insert Terminals(Q, factSetHash, result)
```

The fast path is what makes per-record cost `O(1)` for fresh queries
in a session-long mapAttrs trace. Without it, the writer would
re-sort and re-trie the whole growing factSet on every `record()`,
giving cold-record cost `O(K² · F · log)`.

`useful` dispatch is the per-edge subset of requests not already in
`cur` — a Patricia-split tail edge keeps its original whole-set RS
reference (including the shared-prefix Requests that the
intermediate cur now contains), but the "useful" part is just what
the tail adds.

### Patricia split

When a new recording's `remaining` partially overlaps an existing
edge's useful-dispatch (∅ ⊊ shared ⊊ useful), the existing edge is
split:

```
Before:
   (Q, cur) ── RS_existing ──▶ FactSet_existing

After:
   (Q, cur)         ── RS_shared    ──▶ FactSet_intermediate
                          (new content-addressed RS node = the shared part)
   (Q, intermediate) ── RS_existing ──▶ FactSet_existing  (re-pointed)
   (Q, intermediate) ── RS_new      ──▶ FactSet_new       (added later)
```

Three properties of how this is implemented in
`dg_recordImpl`:

- Both tail edges keep their original `RS_*` references — the
  RequestSet pool already stores them, so no duplication. Only
  `RS_shared` is a freshly inserted node, and it dedupes against any
  other recording that produced the same intersection.
- `FactSet_intermediate` is `FactSet ∪ Facts(shared)`, computed via
  XOR extension. If two recordings observed the same Responses for
  the shared Requests, they land at the same intermediate by content
  addressing; if not, they end up at different intermediates and
  coexist as sibling paths.
- The split removes the old `Asks(Q, cur, RS_existing)` row via
  `removeAsks` (which exists in the schema specifically for this
  case).

## Replay

`TracingReplayEvaluator` sits on top of the inner `Interpreter`. On
each Query, it consults the cache; on a hit it returns a
`TracingReplayObject` whose method calls are answered from the cache
too (recursively, by descending into child Queries' recorded
results); on a miss it activates the inner evaluator. The replay
object only "activates inner" lazily — for a hit chain that
doesn't reach into the package's value tree at all, the inner is
never constructed.

The cache-side primitive is `v13Walk(queryHash)`. It tries two paths
in order.

### Fast path: trie diff against `lastQFactsHash`

`TracingReplayEvaluator` maintains:

```cpp
unordered_map<Hash, Hash>  dispatchCache;   // request → response, per-process
SetHash                    lastQFactsHash;  // cur the last successful walk landed at
TrieBuilder                dispatchedTrie;  // cumulative requests dispatched
```

In a session that just walked Q_{k-1} successfully, the next walk
for Q_k usually only differs in a handful of new Requests — Q_k's
evaluation imports a new package, reads a few extra files, etc.
Instead of walking the chain from ∅, fast-path:

1. Look at `Asks(Q_k, ∅)`. If exactly one outgoing edge, take its
   RS root hash `edgeRsHash`.
2. `dispatchedTrie.diff(decisionGraph, edgeRsHash, onlyInThis,
   onlyInOther)`. This is a parallel descent of the in-memory
   `dispatchedTrie` and the stored trie rooted at `edgeRsHash`.
   Subtrees with matching content hashes collapse to no-ops via
   short-circuit at the recursive descent. Result: the symmetric
   difference in `O(|delta| · branching)`.
3. For each request in `onlyInOther` (added by Q_k): dispatch it
   (memoised in `dispatchCache`), XOR `H_element(req, resp)` into a
   candidate cur starting from `lastQFactsHash`.
4. For each request in `onlyInThis` (we dispatched it for an earlier
   Q but Q_k's RS doesn't include it): look up the cached response,
   XOR `H_element(req, resp)` into the candidate cur — XOR is its
   own inverse, so the "out" operation is the same XOR.
5. Check `Terminals(Q_k, candidateCur)`. Hit → commit (extend
   `dispatchedTrie` with `onlyInOther`, update `lastQFactsHash`),
   return the result. Miss → fall through to slow walk.

In the sequential mapAttrs case, `onlyInThis` is empty and
`onlyInOther` is a handful per Q. Per-Q warm cost drops from
`O(|Q.RS|)` to `O(|delta| · log N)`. Across the whole session:
linear in total facts.

### Navigation invariant: IDs flow *into* lookups as keys, never *out*
of lookups

The whole point of the Asks/Terminals machinery is that hash values
— `factSetHash` (`cur`), `queryHash` (`Q`), request-set hashes — are
*produced* by the walker via hashing. They serve as *keys* to
look up content (request sets, terminals) in the trie. They are
never outputs of a lookup — the walker never asks a table "what's
the ID for X?" or "what does this ID belong to?"

Concretely, the pattern is:

1. Walker holds a `cur` (its current hashed state, produced by
   prior hashing steps).
2. Walker uses cur as a key: `getAsks(Q, cur)` returns *content*
   stored at that key — the request sets outgoing from cur.
3. Walker dispatches each request against the live environment,
   XOR-folds each `H_element(req, resp)` into cur to produce a new
   hashed state.
4. New state is a fresh output of hashing. Walker then uses it as
   the next key in step 2.
5. If no edge exists at a computed key, the walker misses cleanly
   — it does not invent a substitute key or search for a subject
   that could have hashed to that key.

This is the property that makes the cache sound under environmental
change: a divergent response naturally lands the walker at a computed
key that has no recorded content, and the miss is a graceful
fall-back. There is no "which state matches this hash?" step
because that question would treat IDs as outputs of lookups.

> **Recording and replay must index queries by the** ***old*** **hash
> — the walker's state** ***before*** **making the observation, not
> after.** The whole point of the walk is that walker starts with
> some `cur` (produced by prior hashing) and uses it to look up what
> to do next. If a query were indexed by the *post-observation* hash
> — the state you get *after* folding in the query's own response —
> the walker couldn't look anything up without first knowing the
> response, which is exactly what it's asking for. That's
> chicken-and-egg. The old hash is what makes replay possible;
> observations turn it into the new hash, which becomes the old hash
> for the next query. This applies to every table (`Asks`, `Terminals`,
> `AmbientAsks`) and every reqhash construction (d=1 request payloads,
> d=2 chain-advance keys, and cb-apply's `from` field alike): if you
> ever find yourself writing `scopeStateIdAt(subject, scope, walk,
> walk.size())` at query-key time, you're using the new hash —
> reverse it to `walk[0..K)` where `K` is the state **before** this
> query's own observation folds in.

### Slow path: `decisionGraph.walk(Q, dispatch)`

Walks the chain from ∅, one edge at a time. At each `(Q, cur)`:

1. `getAsks(Q, cur)` for outgoing edges. If empty: miss.
2. For each edge's RS: compute `usefulDispatch(rs, curRequests)`.
   Dispatch the useful Requests (via `dispatch` callback — memoised
   in `dispatchCache`), XOR-fold their `H_element` into a candidate
   `nextCur`.
3. Validate: `hasAnyEdge(Q, nextCur)`? That is, is there some
   `Asks(Q, nextCur, *)` or `Terminals(Q, nextCur, *)` row? If yes,
   advance `cur = nextCur` and continue. If no, this branch of the
   recording isn't reachable from the current env — try the next
   outgoing edge.
4. If `Terminals(Q, cur)` exists, return that result.

The existence check is per-`Q` rather than per-FactSet — what
matters is that *this* query reached this position in some recorded
trace, not just that some other query happened to land at the same
FactSet hash.

### Replay-object methods

`TracingReplayObject::maybeGetAttr("foo")` etc. each go through
`lookupResult<Q, R>` which calls `v13Walk(QueryGetAttr{"foo",
parentTriePosition})`. The Merkle parent's `queryHashStr` flows into
the child query's hash, so the cache key reflects "getAttr foo on
*this specific* recorded result," not just "getAttr foo on
anything." If `v13Walk` misses, the replay object lazily constructs
its inner counterpart and forwards the call. Some methods that the
cache can't model (`getStringWithoutContext`, `getPath`, `defeatCache`)
always fall through to inner.

`apply(fn, arg)` is the awkward case: the function and argument may
be virtual values that don't correspond to a recorded Object. The
writer assigns them virtual-root IDs; replay tracks an `ambientState`
mapping ID → live Object so that ambient interactions (`getType`,
`getAttr`, `getString`, …) dispatched during the recorded apply can
be answered against the runtime value the caller actually passed in.
The ambient-query bridge in
`TracingReplayEvaluator::dispatchAmbientQuery` handles the per-tag
cases (`getType`, `getAttr` returning a virtual child id,
`getStringWithContext` etc).

**Nondeterminism.** If the box genuinely produces different
Responses to the same Request, the FactSet hashes for the two
recordings diverge naturally (different XOR inputs → different
hashes). The data model can store both — multiple Terminals exist
for the same Q at different factSets — but which one a walk lands
at depends on what the live env returns at dispatch time. The
cache makes no attempt to detect or arbitrate genuine
nondeterminism; it's a model-level concern Phase 2 sketched
policies for and left open.

## Integration

```
EvalCommand::getEvalState                  // src/libcmd/command.cc
    ├── opens decision-graph.sqlite        // TracingDecisionGraph
    ├── creates a new TraceFile under the cache dir
    ├── wraps SystemEnvironment in TracingEnvironment
    │   (which wraps SourceAccessor in TracingSourceAccessor)
    ├── builds the evaluator stack:
    │       Interpreter         (inner)
    │     ↑ TracingEvaluator    (records via TracingWriter)
    │     ↑ TracingReplayEvaluator (replays; falls back to inner)
    └── shares the single TracingWriter across all of the above
```

The recording path and the replay path use the *same* writer; the
writer's job is to be the canonical sink for both. A successful
replay still feeds the writer (so its v13FactSet, allRequestsTrie,
etc. stay in sync) — that's important if a later miss falls through
to inner and produces a fresh `record()`.

`builtins.cache` lives in `src/libexpr/primops/cache.cc`. It
creates a nested evaluator stack
(`TracingReplayEvaluator → TracingEvaluator → Interpreter`) that
shares the outer `TracingDecisionGraph` and persists its
recordings to the same SQLite file. Ambient interactions with the
outer-provided arg are recorded via an `AmbientResolver` that
identifies derived values by their producer query's `queryHash`
(so the replay walker can resolve them by recursive lookup
against the `Requests` pool), while seed roots use
`hashString("seed:"|"local:" + counter)` strings. The full primop
design — including the `<cached-fn>`/`<ambient-fn>` PrimOp split,
the input-traced env-chain nesting that lets the outer's
`TracingEnvironment` see inner file reads as its own Facts, and
the content-tracing relationship that keeps the inner cacheable
across outer contexts — is documented in
[`tracing-eval-cache-primop.md`](./tracing-eval-cache-primop.md).
Covariant-callback replay (the inner calling back into the outer
via the ambient channel) currently falls through to inner
re-evaluation; cache hits for that case are a planned follow-up.

## Concurrency and durability

Single-writer-process model — a `nix eval` invocation holds the
SQLite connection for its lifetime. Multiple concurrent `nix`
processes can read+write the same DB; SQLite WAL mode handles the
coexistence, but the in-process caches (`responseFor`,
`dispatchedTrie`, etc.) are per-process and don't synchronise. That's
fine: every persistent write is `INSERT OR IGNORE` on a
content-addressed key, so concurrent recorders either land identical
rows (no conflict) or land genuinely different rows (no conflict).

`SQLite::isCache()` sets `PRAGMA synchronous = OFF` and `PRAGMA
journal_mode = WAL`. The cache is non-durable in the
power-loss sense — a crash mid-recording can lose recent rows. For an
eval cache that's the right trade.

## Failure modes and fall-through

There are four categories of cache "miss" and the system handles
them transparently:

1. **Q never recorded.** `getAsks(Q, ∅)` is empty. Fall through to
   inner. The recording layer captures the fresh evaluation as a new
   `(Q, factSet, result)`.
2. **Q recorded, but the live env differs.** Walk dispatches and the
   XOR of live response hashes lands at a `cur` not present in any
   recorded chain. `hasAnyEdge` returns false at the divergent
   position; walk gives up. Fall through.
3. **Fast path lookup miss.** `dispatchedTrie.diff` produces a delta;
   we compute candidateCur; `Terminals(Q, candidateCur)` is absent.
   We fall through to slow walk (which may itself hit or miss).
4. **TracingReplayObject can't model the call** (e.g.
   `getStringWithoutContext`, `getPath`). Fall through to inner via
   `ensureInner()`.

The fall-through is always to the inner evaluator, which produces a
correct answer regardless. The cost is just the missed cache benefit.

## Performance

The two K² scaling problems that earlier iterations of this cache hit:

| K (nixpkgs attrs) | cold-record | warm-replay | DB |
|---|---|---|---|
| 1,000 | 65s (0.9s cache overhead) | 0.42s | 7.4 MB |
| 5,000 | 159s (7.2s) | 1.24s | 23 MB |
| 10,000 | 263s (16s) | **2.19s** | 41 MB |

Both numbers are linear in K. Cold-record overhead is bounded by the
incremental writer-side state (each new fact costs O(log N) trie
insert, each `record()` is O(1) for the fresh-Q case via the
precomputed RS hash). Warm-replay is bounded by the per-Q delta size
times trie depth via the fast path.

For comparison: `nix-env -qa` on the same nixpkgs takes 113s to
enumerate 108k packages — it does not currently go through the
tracing cache (constructs `EvalState` directly, bypassing
`EvalCommand`). Wiring it through would be a separate piece of work.

### Cache directory layout

```
$NIX_TRACING_CACHE_DIR/
    decision-graph.sqlite      # the SQLite DB
    eval-tracing-v1/
        traces/<hash>.json     # JSON log per nix invocation, debugging only
        latest.json -> ...     # symlink to most recent trace
```

The JSON traces are not load-bearing for cache correctness — they're
a side channel for offline inspection. Cache hits read no JSON; cache
misses still write one.

## Test surface

Unit tests at `src/libexpr-tests/tracing-decision-graph.cc` cover:

- Atomic pool round-trips, set canonicity, set extension
- Edge insert/get/remove, per-Q isolation
- `record()` + `walk()` end-to-end including divergent responses,
  Patricia split, edge-following on supersets, multi-element
  RequestSets

Test-only APIs:

- `insertFactSet(members)` — caller-side construction of a FactSet
  by value. Production uses `primeFactSetCache` plus the
  incrementally-maintained `v13FactSetHash` in `TracingWriter`.
- `removeAsks(...)` — used by Patricia split internally, and by
  unit tests directly.

Performance harness under `tests/perf/tracing-cache/`:

- `nixpkgs-validate.sh` — end-to-end correctness sweep across
  commits, attribute variants, and file-edit replay churn.
- `v13-scaling-threshold.sh` — K=1..1000 cold/warm/db sweep on
  recent nixpkgs.
- `v13-large-scale.sh` — K=1k..10k push toward `nix-env -qa` scale.

## Open work

- **Phase 2 from the legacy design doc**: passive-replay-before-insert
  (skip records whose factSet is a redundant superset of an existing
  Terminal) and a distance-to-any-R navigation heuristic. The K²
  motivations that drove Phase 2 are gone — the writer-side
  `TrieBuilder` and the replay-side fast-path closed them within
  Phase 1 — but the cross-session amortisation and post-Patricia-split
  divergence handling Phase 2 was designed for remain valid future
  work.
- **`builtins.cache` covariant callbacks ship validated.** See
  [`tracing-eval-cache-primop.md`](./tracing-eval-cache-primop.md).
  The inner records outer accesses on the callback arg via
  `TracingLocalObject`; on replay the dispatcher invokes the apply
  live (`resolveAmbientId` `tag == "apply"` branch) using a
  `ReplayLocalObject` frozen image of the recorded arg. Outer
  lambda body changes are caught — no Responses-pool fallback in
  the dispatcher. Storage cost: one Responses-pool entry per
  ambient interaction (bounded by the apply-result fanout) plus
  one localArg sidecar Request per apply.
- **Eviction / compaction**: none. The DB grows with the workload.
  At 41 MB per 10k recorded attrs, that's tolerable for a while.
- **Wiring `nix-env -qa`** through the cache. Currently bypasses.

## Source map

- `src/libexpr/include/nix/expr/tracing-decision-graph.hh` — public
  API (schema, atom pool methods, `record`/`walk`/`TrieBuilder`)
- `src/libexpr/tracing-decision-graph.cc` — implementation
- `src/libexpr/include/nix/expr/tracing-writer.hh` — `TracingWriter`
  with its incremental state and the `logResponse` / `logResult`
  hot path
- `src/libexpr/tracing-environment.cc` — `TracingEnvironment` and
  `TracingSourceAccessor` (file-read capture)
- `src/libexpr/tracing-replay-evaluator.cc` — `v13Walk` including
  the trie-diff fast path
- `src/libexpr/tracing-replay-object.cc` — per-method lookup &
  fall-through to inner
- `src/libcmd/command.cc` — wiring into `EvalCommand`
- `src/libexpr/primops/cache.cc` — the `builtins.cache` primop
- `src/libexpr/expr-from-object.cc` — `ExprFromObject`,
  `AmbientResolver`, `makeCachedFnPrimOp` / `makeAmbientFnPrimOp`
- `src/libexpr/ambient-object.cc` — `AmbientObject` (outer value
  reached via ambient query)
- `tests/perf/tracing-cache/` — perf scripts and validation sweeps
- `tests/functional/builtins-cache.sh` — `builtins.cache` functional tests
