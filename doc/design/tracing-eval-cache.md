# Tracing eval cache

Design doc for the tracing eval-cache. Vocabulary is defined in
[`tracing-eval-cache-vocabulary.md`](./tracing-eval-cache-vocabulary.md);
this document is the reference for *why*, referring to those terms.
For git history: earlier data-model attempts (trie, flat-CBOR sets,
Phase-1 sketch) have been removed.

## What it is

A persistent cache for `nix eval` / `nix build` / any other
`EvalCommand`-derived CLI. When the evaluator reaches a Result for
some Query — `evalFile`, `getAttr`, `apply`, `getString`, etc. —
the cache stores a *trace* of the environment reads that led to that
Result. On a subsequent invocation, the cache replays the Query by
walking the trace against the current environment: each recorded
file-read or env-var lookup is re-issued and its current response
compared against what was recorded. If everything matches, the cache
returns the stored Result without invoking the inner evaluator.

The cache treats the inner evaluator as a black box. It doesn't try
to predict, parse, or instrument Nix expressions — it just observes
`(Request → Response)` pairs at the environment boundary and
remembers which Results those pairs preceded.

Enable with `--option tracing-eval-cache true` plus the
`tracing-eval-cache` experimental feature flag. Storage path is
`$NIX_TRACING_CACHE_DIR/decision-graph.sqlite` (defaults to
`$XDG_CACHE_HOME/nix/eval-tracing-decision-graph/index.sqlite`). The
cache is non-destructive: misses fall through to the inner evaluator
and the answer is correct either way.

## Vocabulary recap

Full definitions in
[`tracing-eval-cache-vocabulary.md`](./tracing-eval-cache-vocabulary.md).
The essentials for this doc:

- **Query message pairing** — `Query` / `Result` between the caller
  and the evaluator. Each hashed by SHA-256 of its serialized payload
  (`queryHash`, `resultHash`); Query payloads carry a `from` field
  for Merkle provenance.
- **Env message pairing** — `Request` / `Response` between the
  evaluator and its environment (filesystem, env vars, outer
  evaluator).
- **Fact** = `(requestHash, responseHash)`; **FactSet** = a set of
  Facts, hashed by XOR-fold; **RequestSet** = a set of Request
  hashes, hashed by canonical Merkle over a sorted-dedup member
  list.

Set identity by hash is load-bearing, so the hashing schemes matter
(more below).

A trace through the cache for a Query is a chain of Asks ending
at a Terminal:

```
(queryHash, ∅)
   │ Ask: dispatch this RequestSet's Requests, observe Responses
   ▼
(queryHash, cur')
   │ Ask: dispatch this RequestSet's Requests
   ▼
   ...
(queryHash, cur_final) ── Terminal ──▶ resultHash
```

Every trace starts at `cur = ∅`. First-time recordings insert a
single Ask covering all remaining Requests; Patricia split (below)
factors shared prefixes when later recordings overlap.

## Storage layer

Six SQLite tables (Query + Env layers). All append-only via
`INSERT OR IGNORE`; reads use prepared statements with a per-hash
in-process cache. Ambient adds three more (see the vocab's
[Storage tables (Ambient and Subject-evolution additions)](./tracing-eval-cache-vocabulary.md#storage-tables-ambient-and-subject-evolution-additions)).

```
Requests(requestHash BLOB PRIMARY KEY, payload BLOB)
Queries (queryHash   BLOB PRIMARY KEY, payload BLOB)
Results (resultHash  BLOB PRIMARY KEY, payload BLOB)

RequestSetNodes(nodeHash BLOB PRIMARY KEY, payload BLOB) WITHOUT ROWID

Ask     (queryHash BLOB, factSetHash BLOB, requestSetHash BLOB,
         PRIMARY KEY (queryHash, factSetHash, requestSetHash)) WITHOUT ROWID
Terminal(queryHash BLOB, factSetHash BLOB, resultHash BLOB,
         PRIMARY KEY (queryHash, factSetHash, resultHash))     WITHOUT ROWID
```

`WITHOUT ROWID` on the Ask and Terminal tables halves on-disk size
vs the default layout.

Notable absences (intentional):

- **No `Responses` table.** Walk recomputes responses from the live
  environment and compares hashes; the recorded response bytes
  never come back into play. We keep request payloads — walk needs
  the path to dispatch — but not response payloads.
- **No `FactSets` table.** FactSet members are reconstructed
  incrementally in-process during `record()` and `walk()`. The
  XOR-fold hash *is* the identity (the `factSetHash` columns
  above); members don't need to live on disk. Earlier sketches
  persisted them and paid 94% of the DB size for it.

### RequestSet trie

`RequestSetNodes` is the storage for the RequestSet pool: each row is
one trie node, keyed by SHA-256 of its serialized node payload. A
node is one of:

```
Leaf:     [0x00] hash_1 hash_2 ... hash_n           (n ≤ TRIE_SPLIT_THRESHOLD)
Internal: [0x01] (bucket_idx_byte || child_hash)+   (sparse, sorted by bucket)
```

Bucket index at depth `d` for an element hash `h` is the `TRIE_RADIX_BITS`
bits of `h` starting at bit `d * TRIE_RADIX_BITS` (MSB first). Constants
in `src/libexpr/tracing-decision-graph.cc`: `TRIE_RADIX_BITS = 4`
(16-way fanout), `TRIE_SPLIT_THRESHOLD = 16`. SHA-256 outputs are
uniformly random, so buckets balance in expectation — no
content-defined chunking needed.

This shape buys two things:

1. **Structural sharing.** Two RequestSets that overlap share the
   subtree rows for the overlap, automatically via node-hash
   equality on the shared payloads. At K=10000 nixpkgs-attr
   recordings, total DB size is 41 MB; without trie sharing it would
   be ~2 GB of mostly-duplicate RequestSet payloads.
2. **Cheap symmetric difference between any two roots.** Descend
   both tries in parallel; any subtree where the two roots agree on
   the hash collapses to a no-op. Cost tracks the size of the
   difference, not the size of either input set. This is what the
   replay-side fast path (below) keys off.

### FactSet hashing

FactSet hashes are XOR-fold over per-element hashes:

```
H_element(req, resp) = SHA-256(req.bytes || resp.bytes)
H(set)               = XOR over H_element of each member, starting at 0
emptySetHash         = all-zero (XOR identity)
H(S ∪ {e})           = H(S) XOR H_element(e)   when e ∉ S
```

Properties: commutative (order-independent), associative,
identity-with-zero, self-inverse on XOR. Extension against a
known-disjoint element is a single in-place XOR. Note this only
gives the canonical
`H(A ∪ B) = H(A) XOR H(B)` when `A ∩ B = ∅`; the general
`H(A) XOR H(B) = H(A △ B)` (symmetric difference) is what falls out
of XOR over hashes.

Algebraically weaker than a sorted-Merkle hash — a chosen-input
attacker could construct collisions — but the threat model is an
internal cache; a hash collision yields a wrong cache hit detected on
next use, no security impact.

## Recording

`TracingEnvironment` wraps the inner environment (currently
`SystemEnvironment`). Every `getFileHash`, `getEnv`, and Env
message-pairing event on the wrapped accessor flows through it:

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
vector<Fact>              envFactSet;          // insertion order
SetHash                   envFactSetHash;      // XOR-fold, incremental
unordered_set<Hash>       seenRequests;        // dedup
unordered_map<Hash, Hash> responseFor;         // request → response
TrieBuilder               sessionRequestsTrie; // canonical requestSetHash, incremental
```

Each new Fact (one not already in `seenRequests`) is XOR'd into
`envFactSetHash`, mapped in `responseFor`, and inserted into
`sessionRequestsTrie` — one path-copy from leaf to root, split on
leaf-overflow.

When the evaluator finishes a Query and produces a Result,
`TracingEvaluator` (the recording counterpart to
`TracingReplayEvaluator`) calls `writer.logResult(value, result,
queryHandle)`. That handler:

1. Inserts `(resultHash, resultPayload)` into `Results`.
2. Pushes any unpersisted nodes from `sessionRequestsTrie` into
   `RequestSetNodes`.
3. Installs the current `envFactSet` under `envFactSetHash` via
   `installFactSet`.
4. Calls `decisionGraph.record(queryHash, envFactSetHash,
   resultHash, responseFor, seenRequests,
   sessionRequestsTrie.rootHash())` — the fastest overload, using
   the precomputed requestSetHash to skip the per-call trie rebuild.

### `record()` algorithm

`record(queryHash, factSetHash, result, responseFor, sessionRequests,
sessionRequestsRsHash)` integrates the recording into the decision
graph. It tracks a local `dispatchedSoFar` (Requests consumed so far)
and a running `cur` (XOR-fold hash):

```
cur              = ∅
dispatchedSoFar  = {}
while dispatchedSoFar ≠ sessionRequests:
    # Eager Patricia split pass: any existing Ask whose useful
    # dispatch partially overlaps `remaining` gets split.
    for requestSet in Ask(queryHash, cur):
        useful = members(requestSet) \ dispatchedSoFar
        shared = useful ∩ (sessionRequests \ dispatchedSoFar)
        if ∅ ⊊ shared ⊊ useful:
            patricia_split(queryHash, cur, requestSet, shared)

    # Find a followable Ask: usefulDispatch ⊆ remaining
    if some Ask a in Ask(queryHash, cur) has useful(a) ⊆ remaining:
        consume useful(a) into cur, dispatchedSoFar
    elif dispatchedSoFar = ∅ and sessionRequestsRsHash provided:
        # Fast path for first-time recording: jump straight to factSet
        insert Ask(queryHash, ∅, sessionRequestsRsHash)
        cur = factSetHash; break
    else:
        # Slow path: insert a new whole-remaining Ask
        insert Ask(queryHash, cur, insertRequestSet(remaining))
        consume remaining into cur, dispatchedSoFar

insert Terminal(queryHash, factSetHash, result)
```

The fast path uses the precomputed requestSetHash so the writer
does not re-hash the growing FactSet per recording. Without it the
writer would re-sort and re-trie the whole FactSet on every
`record()`, which was the O(n²) failure mode in earlier prototypes.

`useful` dispatch is the per-Ask subset of Requests not already in
`cur` — a Patricia-split tail Ask keeps its original whole-set
RequestSet reference (including the shared-prefix Requests the
intermediate cur now contains), but the "useful" part is just what
the tail adds.

### Patricia split

When a new recording's `remaining` partially overlaps an existing
Ask's useful-dispatch (∅ ⊊ shared ⊊ useful), the existing Ask is
split:

```
Before:
   (queryHash, cur) ── existingRequestSet ──▶ FactSet_existing

After:
   (queryHash, cur)          ── sharedRequestSet   ──▶ FactSet_intermediate
                           (new RequestSet node keyed by SHA-256 of the shared part)
   (queryHash, intermediate) ── existingRequestSet ──▶ FactSet_existing  (re-pointed)
   (queryHash, intermediate) ── newRequestSet      ──▶ FactSet_new       (added later)
```

Three properties of how this is implemented in `dg_recordImpl`:

- Both tail Asks keep their original RequestSet references — the
  RequestSet pool already stores them, so no duplication. Only
  `sharedRequestSet` is a freshly inserted node, deduping against
  any other recording that produced the same intersection via
  `INSERT OR IGNORE` on the node hash.
- `FactSet_intermediate` is `FactSet ∪ Facts(shared)`, computed via
  XOR extension. Two recordings that observed the same Responses for
  the shared Requests land at the same intermediate by hash equality;
  divergent Responses land them at different intermediates and they
  coexist as sibling paths.
- The split removes the old `Ask(queryHash, cur, existingRequestSet)`
  row via `removeAsks` (in the schema specifically for this case).

## Replay

`TracingReplayEvaluator` sits on top of the inner `Interpreter`. On
each Query, it consults the cache; on a hit it returns a
`TracingReplayObject` whose method calls are answered from the cache
too (recursively, by descending into child Queries' recorded
Results); on a miss it activates the inner evaluator. The replay
object activates inner lazily — for a hit chain that doesn't reach
into the package's value tree at all, the inner is never
constructed.

The cache-side primitive is `walk(queryHash)`. Currently there is
only one path: the walk-from-∅ described under "Slow path" below.
The subsection that follows describes a fast-path design that has
not been implemented; the code has no `envCur`, no
`dispatchedTrie`, and no diff routine on `TrieBuilder`.

### Candidate design: trie diff against `envCur`

Not present in the code today. Recorded here as a design worth
trying if the walk-from-∅ cost per Query becomes the bottleneck.

The idea: `TracingReplayEvaluator` would maintain

```cpp
unordered_map<Hash, Hash>  responseFor;     // request → response, per-process
SetHash                    envCur;          // cur the last successful walk landed at
TrieBuilder                dispatchedTrie;  // cumulative requests dispatched
```

In a session that just walked a previous Query successfully, the
next Query usually only differs in a handful of new Requests — it
imports a new package, reads a few extra files, etc. Instead of
walking the chain from ∅, the design would:

1. Look at the Query's outgoing Ask at `cur = ∅` and take its
   RequestSet root hash `askRequestSetHash`.
2. `dispatchedTrie.diff(decisionGraph, askRequestSetHash, onlyInThis,
   onlyInOther)`. A parallel descent of the in-memory
   `dispatchedTrie` and the stored trie rooted at
   `askRequestSetHash`; subtrees with matching node hashes collapse
   to no-ops via short-circuit at the recursive descent.
3. For each request in `onlyInOther` (added by this Query): dispatch
   it (memoised in `responseFor`), XOR `H_element(req, resp)` into
   a candidate cur starting from `envCur`.
4. For each request in `onlyInThis` (dispatched for an earlier
   Query but not in this one's RequestSet): look up the cached
   response, XOR `H_element(req, resp)` into the candidate cur —
   XOR is its own inverse, so the "out" operation is the same XOR.
5. Check `Terminal(queryHash, candidateCur)`. Hit → commit (extend
   `dispatchedTrie` with `onlyInOther`, update `envCur`), return the
   Result. Miss → fall through to walk-from-∅.

The goal would be that session-cumulative warm cost tracks the
delta between successive Queries' RequestSets, not the size of
either RequestSet. Whether it's worth landing depends on measured
walk-from-∅ cost on realistic workloads.

### Navigation invariant: hashes flow *into* lookups as keys, never *out*

The whole point of the Ask/Terminal machinery is that hash values —
`factSetHash` (`cur`), `queryHash` (`queryHash`), RequestSet hashes — are
*produced* by the walker via hashing. They serve as *keys* to look
up content (RequestSets, Terminals) in the trie. They are never
outputs of a lookup — the walker never asks a table "what's the hash
for X?" or "what does this hash belong to?"

Concretely, the pattern is:

1. Walker holds a `cur` (its current hashed state, produced by
   prior hashing steps).
2. Walker uses cur as a key: `getAsks(queryHash, cur)` returns *content*
   stored at that key — the RequestSets outgoing from cur.
3. Walker dispatches each Request against the live environment,
   XOR-folds each `H_element(req, resp)` into cur to produce a new
   hashed state.
4. New state is a fresh output of hashing. Walker uses it as the
   next key in step 2.
5. If no edge exists at a computed key, the walker misses cleanly —
   it does not invent a substitute key or search for a state that
   could have hashed to that key.

This is what makes the cache sound under environmental change: a
divergent response naturally lands the walker at a computed key
that has no recorded content, and the miss is a graceful fall-back.
There is no "which state matches this hash?" step because that
question would treat hashes as outputs of lookups.

> **Every Query in the chain must be indexed by the** ***old*** **hash
> — the walker's state** ***before*** **making that Query's own
> observation, not after.** The walker starts each step at some `cur`
> (produced by prior hashing) and uses it to look up what to do next.
> If a Query were indexed by the *post-observation* hash — the state
> you get *after* folding in this Query's own response — the walker
> couldn't look anything up without first knowing the response,
> which is exactly what it's asking for. That's chicken-and-egg. The
> old hash is what makes replay possible; the observation turns it
> into the new hash, which then becomes the old hash for the *next*
> Query.
>
> This applies to every table that produces a next observation from
> a lookup: `Ask` (edge from cur), `AmbientAsk` (chain-advance from
> fromFactSet), and every requestHash construction whose `from` field
> is the walker's pre-observation state (Env request payloads and
> cb-apply `from` fields alike). `Terminal` doesn't fit this pattern
> — a Terminal is the *end* of the chain and produces a Result, not a
> next observation, so it's queried at the cur the walker *lands*
> at after all observations for that queryHash.
>
> Practical check for every call-site: if you're writing
> `stateHashAt(subject, argAncestry, history, history.size())` at
> Query-key time, you're using the new hash — reverse it to
> `history[0..step)` where `step` is the state **before** this
> Query's own observation folds in.

### Walk from ∅: `decisionGraph.walk(queryHash, dispatch)`

Walks the chain from ∅, one Ask at a time. At each `(queryHash, cur)`:

1. `getAsks(queryHash, cur)` for outgoing Asks. If empty: miss.
2. For each Ask's RequestSet: compute
   `usefulDispatch(requestSet, dispatchedSoFar)`. Dispatch the
   useful Requests (via `dispatch` callback — memoised in
   `responseFor`), XOR-fold their `H_element` into a candidate
   `nextCur`.
3. Validate: `hasAnyEdge(queryHash, nextCur)`? That is, is there some
   `Ask(queryHash, nextCur, *)` or `Terminal(queryHash, nextCur, *)` row? If yes,
   advance `cur = nextCur` and continue. If no, this branch of the
   recording isn't reachable from the current env — try the next
   outgoing Ask.
4. If `Terminal(queryHash, cur)` exists, return that Result.

The existence check is per-`queryHash` rather than per-FactSet — what
matters is that *this* Query reached this position in some recorded
trace, not just that some other Query happened to land at the same
FactSet hash.

### Replay-object methods

`TracingReplayObject::maybeGetAttr("foo")` etc. each go through
`lookupResult<queryHash, R>` which calls `walk(QueryGetAttr{"foo",
parentTriePosition})`. The Merkle parent's `queryHash` flows into
the child Query's hash, so the cache key reflects "getAttr foo on
*this specific* recorded Result," not just "getAttr foo on
anything." If `walk` misses, the replay object lazily constructs its
inner counterpart and forwards the call. Methods the cache can't
model (`getStringWithoutContext`, `getPath`, `defeatCache`) always
fall through to inner.

`apply(fn, arg)` is the awkward case: when `fn` or `arg` came from a
prior cache boundary and no live Object is around to probe. The
Ambient message pairing handles this — inner-owned callback args
are proxied by `ReplayCallbackArg` (see the vocab's
[Callback arg objects](./tracing-eval-cache-vocabulary.md#callback-arg-objects)),
and their
recorded responses are served from the `InnerValueResponse` table.
`TracingReplayEvaluator::dispatchAmbientQuery` is the per-tag
bridge; details live in
[`tracing-eval-cache-primop.md`](./tracing-eval-cache-primop.md).

**Nondeterminism.** If the environment genuinely produces different
Responses to the same Request, the FactSet hashes for the two
recordings diverge naturally (different XOR inputs → different
hashes). The data model can store both — multiple Terminals exist
for the same queryHash at different factSets — but which one a walk lands
at depends on what the live env returns at dispatch time. The cache
makes no attempt to detect or arbitrate genuine nondeterminism; it's
a model-level concern that remains open.

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

The recording path and the replay path use the *same* writer; it's
the canonical sink for both. A successful replay still feeds the
writer (so its `envFactSet`, `sessionRequestsTrie`, etc. stay in
sync) — important if a later miss falls through to inner and
produces a fresh `record()`.

`builtins.cache` lives in `src/libexpr/primops/cache.cc`. It creates
a nested evaluator stack (`TracingReplayEvaluator → TracingEvaluator
→ Interpreter`) sharing the outer `TracingDecisionGraph` and
persisting its recordings to the same SQLite file. Full primop
design in
[`tracing-eval-cache-primop.md`](./tracing-eval-cache-primop.md).

## Concurrency and durability

Single-writer-process model — a `nix eval` invocation holds the
SQLite connection for its lifetime. Multiple concurrent `nix`
processes can read+write the same DB; SQLite WAL mode handles the
coexistence, but the in-process caches (`responseFor`,
`dispatchedTrie`, etc.) are per-process and don't synchronise.
That's fine: every persistent write is `INSERT OR IGNORE` on a
hash-derived key, so concurrent recorders either land identical
rows (no conflict) or land genuinely different rows (no conflict).

`SQLite::isCache()` sets `PRAGMA synchronous = OFF` and `PRAGMA
journal_mode = WAL`. The cache is non-durable in the
power-loss sense — a crash mid-recording can lose recent rows. For an
eval cache that's the right trade.

## Failure modes and fall-through

There are four categories of cache "miss" and the system handles
them transparently:

1. **queryHash never recorded.** `getAsks(queryHash, ∅)` is empty. Fall through to
   inner. The recording layer captures the fresh evaluation as a new
   `(queryHash, factSet, result)`.
2. **queryHash recorded, but the live env differs.** Walk dispatches and the
   XOR of live response hashes lands at a `cur` not present in any
   recorded chain. `hasAnyEdge` returns false at the divergent
   position; walk gives up. Fall through.
3. **Fast path lookup miss.** `dispatchedTrie.diff` produces a delta;
   we compute candidateCur; `Terminal(queryHash, candidateCur)` is absent.
   Fall through to slow walk (which may itself hit or miss).
4. **TracingReplayObject can't model the call** (e.g.
   `getStringWithoutContext`, `getPath`). Fall through to inner via
   `ensureInner()`.

The fall-through is always to the inner evaluator, which produces a
correct answer regardless. The cost is just the missed cache benefit.

## Performance goals

- **No linear search, anywhere.** Every lookup on recorded data
  goes through hashed keys — `getAsks(queryHash, cur)`,
  `getTerminal(queryHash, cur)`, RequestSet-trie node lookup,
  request-pool lookup. Cold and warm alike.
- **No unbounded backtracking.** The walker consumes recorded
  Facts against the live environment; on a Fact mismatch it fails
  cleanly at the divergent position rather than searching for an
  alternative interpretation. Missed states have no recorded content
  at their key; the miss is the answer.
- **Session-cumulative work proportional to observed change**, not
  to the total recorded state. The writer's incremental
  `sessionRequestsTrie` and `envFactSetHash` avoid re-hashing the
  growing FactSet per recording. The walker's `dispatchedTrie` fast
  path avoids re-walking the chain from ∅ per Query — subsequent
  Queries in the same session pay for the delta from the previous
  walk.
- **Structural storage sharing.** RequestSets that overlap share
  their common trie subtrees automatically via node-hash equality.
  Storage grows with the count of unique Requests, not with the
  number of Queries times the average RequestSet size.

Perf sweeps that check these hold across scaling regimes are in
`tests/perf/tracing-cache/`.

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
- Edge insert/get/remove, per-queryHash isolation
- `record()` + `walk()` end-to-end including divergent responses,
  Patricia split, edge-following on supersets, multi-element
  RequestSets

Test-only APIs:

- `insertFactSet(members)` — caller-side construction of a FactSet
  by value. Production uses `installFactSet` plus the incrementally
  maintained `envFactSetHash` in `TracingWriter`.
- `removeAsks(...)` — used by Patricia split internally, and by
  unit tests directly.

Performance harness under `tests/perf/tracing-cache/`:

- `nixpkgs-validate.sh` — end-to-end correctness sweep across
  commits, attribute variants, and file-edit replay churn.
- `scaling-threshold.sh` — K=1..1000 cold/warm/db sweep on
  recent nixpkgs.
- `scale.sh` — K=1k..10k push toward `nix-env -qa` scale.

## Open work

- **Cross-session amortisation and post-Patricia-split divergence
  handling** — passive-replay-before-insert (skip records whose
  factSet is a redundant superset of an existing Terminal) and a
  distance-to-any-R navigation heuristic. The K² motivations are
  gone; the semantics remain valid future work.
- **Eviction / compaction**: none. The DB grows with the workload.
  At 41 MB per 10k recorded attrs, tolerable for a while.
- **Wiring `nix-env -qa`** through the cache. Currently bypasses
  `EvalCommand`.

## Source map

- `src/libexpr/include/nix/expr/tracing-decision-graph.hh` — public
  API (schema, atom pool methods, `record`/`walk`/`TrieBuilder`)
- `src/libexpr/tracing-decision-graph.cc` — implementation
- `src/libexpr/include/nix/expr/tracing-writer.hh` — `TracingWriter`
  with its incremental state and the `logResponse` / `logResult`
  hot path
- `src/libexpr/tracing-environment.cc` — `TracingEnvironment` and
  `TracingSourceAccessor` (file-read capture)
- `src/libexpr/tracing-replay-evaluator.cc` — `walk` including the
  trie-diff fast path
- `src/libexpr/tracing-replay-object.cc` — per-method lookup &
  fall-through to inner
- `src/libcmd/command.cc` — wiring into `EvalCommand`
- `src/libexpr/primops/cache.cc` — the `builtins.cache` primop
- `src/libexpr/expr-from-object.cc` — `ExprFromObject`,
  `OuterResolver`, `makeCachedFnPrimOp` / `makeAmbientFnPrimOp`
- `src/libexpr/outer-object.cc` — `OuterObject` (outer-owned value
  the inner probes via Env)
- `src/libexpr/subject-id.cc` — Subject variants, state hash /
  argAncestry / evolution machinery
- `tests/perf/tracing-cache/` — perf scripts and validation sweeps
- `tests/functional/builtins-cache.sh` — `builtins.cache` functional tests
