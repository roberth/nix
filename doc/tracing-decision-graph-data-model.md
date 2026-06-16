# Trace Index Data Model (Decision Graph) — Phase 1

Successor to [`tracing-sets-index-data-model.md`](tracing-sets-index-data-model.md), the
flat-CBOR sets-based index currently shipped on this branch. This document
specifies the decision-graph model that the sets-based index was always
meant to be a stepping-stone toward. It covers Phase 1 only:
intersectionless recording and replay. Phase 2 (precondition intersection)
will be specified separately and grounded in this baseline.

## Motivation

The shipped sets-based index records each Query's preconditions as
flat content-addressed CBOR blobs and looks them up by scanning every
recorded Binding for the queryHash and filtering by Bloom. Two failure
modes follow:

- **`k` grows with content edits**, not just with new dependency shapes.
  Each `(queryHash, preconditionHash)` is a fresh row whenever any
  Response changes, even when the dependency *shape* (which Requests
  the box makes) didn't. Repeated edits to source files produce a row per
  content combination. The dominant per-queryHash bucket grows
  unboundedly.
- **Lookup is a candidate scan, not navigation**. The Bloom prefilter is
  load-bearing — without it the per-query cost is `O(k · |P|)`. With
  it, it's still `O(k)` candidate checks, with the prefilter doing
  probabilistic rejection. This compensates for, rather than addresses,
  the lack of a structural index.

Neither problem is fixable in the shipped storage model without adding
batch maintenance, which the design conversation explicitly rejected.
The replacement is a navigated decision graph with structural sharing
in storage and no scanning at lookup.

## Two layers

The model has two layers that should not be conflated:

1. **Storage layer** — content-addressed pools of sets and atoms. No
   graph structure; no notion of "what comes next". A flat pool of
   canonical identifiers.
2. **Decision graph layer** — a relational structure built on top of
   the storage layer using storage hashes as identifiers. Encodes the
   box's observed behaviour as edges between storage hashes.

Edges in the decision graph reference storage hashes; they do not own
or duplicate the sets themselves. Multiple positions in the decision
graph reference the same storage hash whenever the underlying set is
identical, by content addressing.

## Storage layer

Two distinct roles, kept separate at the schema level:

- **Query / Result** — the thing the cache exists to answer; the
  outside of the black box. A Query has its own `queryHash`
  (operation + params + inputHashes); a Result is a recorded answer
  with its own `resultHash`.
- **Request / Response** — what the box asks of the environment
  during evaluation. A Request has its own `requestHash`; a
  Response has its own `responseHash`. Pairs of `(Request,
  Response)` form Facts.

Query/Result and Request/Response don't share tables or hash spaces;
they're different roles.

Atomic content-addressed entities:

- **Query** — `(queryHash, payload)`.
- **Result** — `(resultHash, payload)`.
- **Request** — `(requestHash, payload)`.
- **Response** — `(responseHash, payload)`.
- **Fact** — a `(requestHash, responseHash)` pair. Hash:
  `SHA-256(requestHash || responseHash)`. The atomic unit of
  observed evaluation context.

Set pools with parent-pointer delta encoding per L7881:

- **RequestSet pool** — `(setHash, parentHash, extraRequests)`.
  A set of Requests. Each new set is a delta from some existing
  parent set; the parent chain encodes structural sharing.
- **FactSet pool** — `(setHash, parentHash, extraFacts)`.
  A set of Facts.

The `setHash` is canonical: content-equivalent sets always have the
same hash regardless of how they were built. Multiple parent-pointer
decompositions of the same `setHash` may coexist (whichever parent
the recorder picked).

Set extension is the storage-layer operation that the decision
graph relies on for transitions: given a FactSet hash and a set of
new Facts, produce the new FactSet hash. The choice of set-hashing
scheme (canonical sorted-Merkle vs. additive/xor) is a storage-layer
design knob that affects the cost of extension but not the
decision-graph semantics — recorded here only as a flag, not a
decision.

The storage layer has no per-`Q` structure, no notion of edges, no
notion of "next". It is queried purely by content hash, or by the
set-extension operation.

## Decision graph layer

The decision graph is a relational structure over storage hashes.

Within the evaluation of a specific Query `Q`, the box's next ask
is determined by `(Q, current FactSet)`: the *same* FactSet reached
during two different `Q` evaluations may have entirely different
next-asks, because the Query itself drives what's needed. So
decision-graph positions on the asks side are pairs:

- **`(Q, FactSet)` positions** — identified by
  `(queryHash, factSetHash)`. Represents "while evaluating `Q`, the
  box has observed exactly these Facts."
- **RequestSet positions** — identified by a RequestSet storage hash
  alone. Represents "this set of Requests the box asks next."
  RequestSet positions are Q-free because the RequestSet → next-FactSet
  transition is a pure structural union and doesn't depend on which
  `Q` is in flight; multiple `Q`s that happen to dispatch the same
  RequestSet share the same downstream structure.

Two edge types:

- **`(Q, FactSet) ──asks──▶ RequestSet`** — while evaluating `Q`
  from this FactSet, the box's next set of Requests is this
  RequestSet. After Patricia normalisation, a `(Q, FactSet)`
  position has pairwise-disjoint outgoing RequestSet edges (and
  almost always exactly one — see below).
- **`(Q, FactSet) ──terminal──▶ Result`** — this FactSet is a
  recorded precondition under which `Q`'s recorded Result is the
  named one.

There is no third edge type for "RequestSet to next FactSet". The
transition is implicit: after dispatching the RequestSet and
observing Responses, the new Facts are computed (pairing each
Request with its Response), and the next FactSet is the storage-layer
extension of the source FactSet by those new Facts. The next-FactSet
hash is computable from the source FactSet hash and the new Facts
via the storage layer's set-extension operation. The decision graph
never names a tuple of Responses because the Responses are transient
data used to compute Facts; they live in the dispatch path at
replay/recording time, not in the schema.

A trace through the graph for evaluating `Q` is therefore the
chain:

```
(Q, ∅)
   │ asks
   ▼
   RequestSet
   │  (dispatch RequestSet's Requests; observe Responses;
   │   form new Facts; extend source FactSet in storage layer)
   ▼
(Q, FactSet')
   │ asks
   ▼
   RequestSet
   │  ...
   ▼
(Q, FactSet_final) ── terminal ──▶ Result R
```

`Q` carries through every position; `FactSet` accumulates Facts;
RequestSets appear as edge labels.

Storage-level sharing is automatic at every layer:

- **FactSet hashes** are shared across all `Q`s by content
  addressing. Two unrelated `Q`s whose evaluations pass through the
  same Fact set reference the same FactSet hash, even though they
  occupy distinct `(Q, FactSet)` positions in the decision graph.
- **RequestSet hashes** are shared across all `Q`s. When two `Q`s'
  Asks edges point at the same RequestSet, the RequestSet itself is
  one stored node.
- **Fact hashes** are shared across all FactSets that contain them,
  via the FactSet pool's parent-pointer delta encoding.

Implementation-wise this is two edge tables:

- `Asks(queryHash, factSetHash, requestSetHash)` — outgoing
  RequestSet edge per `(Q, FactSet)` position.
- `Terminals(queryHash, factSetHash, resultHash)` — terminal Result
  edges per `(Q, FactSet)` position.

The entry FactSet for every `Q` is the empty FactSet `∅`. A walk
always starts at `(Q, ∅)`; an entry-point index is unnecessary. Any
Facts the box implicitly carried into `Q`'s evaluation from earlier
session state are captured along `Q`'s tree as the box re-observes
them; if the box silently reused a Fact without re-observing it, the
recording wouldn't see it, but that's a question for whatever
mechanism would expose such reuse (out of scope here).

Neither table holds set content or Responses; they hold references
into the storage layer.

### Invariant: pairwise-disjoint *useful dispatches* per (Q, FactSet)

After Patricia normalisation (below), the outgoing RequestSet edges
from a single `(Q, FactSet)` position have pairwise-disjoint **useful
dispatches**:

```
usefulDispatch(edge, FactSet) = edge.requestSet \ requestsOf(FactSet.facts)
```

Whole-set labels may still overlap at Requests already in `FactSet`
(this is exactly what Patricia split produces: tail edges keep
whole-set references that include the factored shared prefix, but
that prefix is in the intermediate FactSet's facts). The parts that
would actually be dispatched at this position — what we call the
useful dispatch — are pairwise disjoint. (Two different `Q`s at the
same FactSet hash naturally have independent edge sets — the
invariant is per-position, not per-FactSet-hash.)

This is the standard Patricia "children distinguishable at branch
point" invariant, generalised from single-character discriminators
to sets of Requests. Stating it on whole labels would be too strong
and would in fact be violated by Patricia split's own output;
stating it on useful dispatches matches both the split's output and
what replay needs to navigate unambiguously.

A consequence: in any deterministic, perfectly-attributed evaluation,
every `(Q, FactSet)` position has exactly one outgoing RequestSet
edge. Multiple outgoing edges means either the box is
nondeterministic at this precondition (rare in real Nix) or the
precondition over-approximates — the recorder attributed Facts to
this FactSet that weren't actually dependencies, and the divergence
is driven by something not represented here. The latter is the
dominant case in practice and is exactly the symptom Phase 2 will
collapse.

### Patricia split

When a recording extends the graph at FactSet with an observed
next-RequestSet that overlaps but does not equal an existing
outgoing edge's RequestSet, the overlapping edge is split:

```
Before:                              After:
   FactSet                              FactSet
   │                                    │
  requestSet_existing                   requestSet_shared
        │                               │  (new content-addressed node =
        ▼                               │   requestSet_existing ∩ requestSet_new)
  FactSet_existing                      ▼
                                        FactSet_intermediate
                                       ╱                    ╲
                                requestSet_existing       requestSet_new
                                       ▼                       ▼
                                FactSet_existing           FactSet_new
```

`FactSet_intermediate` is content-addressed by `FactSet ∪ (Facts
formed from requestSet_shared and the observed Responses)`. If the
two traces agreed on the Responses to the shared Requests, they
converge on the same `FactSet_intermediate` automatically by
content addressing; if they disagreed, they end up at different
`FactSet_intermediate` nodes (the ordinary content-addressed
FactSet fork).

**Edge labels are whole-set references, not computed deltas.** Both
tail edges keep references to the *original* RequestSet nodes
(`requestSet_existing` and `requestSet_new`) that the recorder
observed — the same nodes that the RequestSet pool already stores by
content hash. Only `requestSet_shared` is a freshly inserted
RequestSet node, and it deduplicates against any other recording
that happened to produce the same intersection. This means a split
inserts at most one new RequestSet node regardless of how often the
same tail RequestSets appear elsewhere in the graph.

A consequence is that an edge's RequestSet may contain Requests
whose Responses are already in the current FactSet at that edge's
source — the Requests asked along the way to that FactSet. Replay
dispatches only the difference: `edge.requestSet \ {Requests already
in current FactSet}`, sourcing the already-observed Responses from
the current FactSet and dispatching only the rest. Content-addressed
FactSet dedup keeps the next-FactSet identity correct regardless.

The split is *set intersection used structurally* for storage and
dispatch sharing, distinct from any precondition-narrowing logic
Phase 2 may add on top.

When the two RequestSets are fully disjoint
(`requestSet_shared = ∅`), the split degenerates:
`FactSet_intermediate = FactSet`, the shared edge is a no-op, and we
fall back to FactSet carrying two parallel outgoing RequestSet edges.
In practice this is rare for real Nix evaluations, which routinely
share common-prefix Requests (environment reads, common imports,
shared source-file lookups).

**When Patricia split fires.** Set canonicity (RequestSet is a
set, not a sequence) absorbs reordering: two recordings of the
same `Q` that ask the same Requests in different orders produce
identical Asks labels and dedupe via content addressing. Different
Responses to the same Requests produce identical labels too — the
divergence lands cleanly on the FactSet axis at the next position
rather than as overlapping labels at the current one. What does
fire Patricia is **path divergence**: source or data changes that
cause the box to ask a different *set* of Requests for the same
`Q`. For example, if `a.nix` once imported `b.nix` and `c.nix` and
later imports `d.nix` instead, two recordings of
`Q = Import a.nix` have overlapping but non-equal RequestSets at
`(Q, ∅)` — `{read a.nix, read b.nix, read c.nix}` versus
`{read a.nix, read d.nix}` — and Patricia factors out the shared
`{read a.nix}` prefix so future replays can dispatch the common
Request once and branch on its Response. This is the common case
the cache is built to handle gracefully.

## Operations

### Replay

Replay is **cache-driven**: no interpreter steps us through. The cache
navigates the graph independently and only falls back to the inner
evaluator on miss.

```
walk(Q):
  factSet = ∅
  loop:
    if Terminals has (Q, factSet, R): return R
    outgoing = Asks(Q, factSet)
    if outgoing is empty: miss
    nextRequestSet = pick from outgoing
    # Edge RequestSet may include Requests whose Responses are already
    # in factSet (the shared prefix from a previous Patricia split).
    # Source those from factSet; dispatch only the rest.
    toDispatch = nextRequestSet \ {Requests present in factSet.facts}
    fresh = {}
    for req in toDispatch:
        fresh[req] = walk_request(req, …)  # recursive Request lookup
    newFacts = {Fact(req, fresh[req])           for req in toDispatch}
             ∪ {Fact(req, factSet.responseFor(req)) for req in nextRequestSet \ toDispatch}
    factSet' = storage.extend(factSet, newFacts)
    if factSet' is not in the FactSet pool: miss
    factSet = factSet'
```

Cost: O(trace length) hash lookups plus the unavoidable cost of
recursively replaying each Request. No scans, no probabilistic
filters. The `nextRequestSet \ factSet.facts` subtraction is the price
of whole-set edge labels — paid in O(|nextRequestSet| + |factSet|) per
step, which is dominated by the dispatch work itself.

The `pick from outgoing` step has only one choice in the common case
(single outgoing edge after Patricia normalisation). When multiple
edges are present, the choice is a knob:

- **Single-edge dispatch**: pick one, follow it; on miss, optionally
  retry the next. Cheapest per step but loses parallelism across
  branches.
- **Parallel dispatch**: dispatch the union of all outgoing
  RequestSets; only one branch's next-FactSet will exist in storage,
  and we follow it. Wasted work on the others, but the results are
  cached.

Either is correct. The choice is local to replay and changeable at
runtime; storage is invariant to it.

### Recording

The recorder observes the box's event stream and maintains a single
global `factSet` that grows monotonically as Responses arrive. There
is no per-`Q` state and no muxing: when a Result is produced for
some `Q`, the recorder samples the current global `factSet` and
pairs it with `Q`. This mirrors the natural and necessary
over-approximation of the black-box model — the recorder can't tell
which events `Q` "caused" and which were unrelated in-flight work,
and the model is built to absorb that ambiguity.

```
state: factSet  # one global accumulating set, starts at ∅

on_event(e):
  case e:
    ResponseReceived(req, resp):
      factSet = factSet ∪ {Fact(req, resp)}
    ResultProduced(Q, result):
      record(Q, factSet, result)
```

Per-event cost: O(1). All work for `Q`'s decision-graph contribution
happens at `ResultProduced` time, via `record`, which integrates the
new `factSet` into `Q`'s Patricia tree.

```
record(Q, factSet, result):
  cur = ∅
  while cur ≠ factSet:
    remaining = requestsOf(factSet) \ requestsOf(cur)
    existing = Asks(Q, cur)
    # Eagerly Patricia-split every outgoing edge whose USEFUL
    # DISPATCH partially overlaps `remaining`. After this pass every
    # outgoing edge's useful dispatch at (Q, cur) is either fully
    # inside `remaining` or fully disjoint from it.
    for e in existing:
      usefulE = e.requestSet \ requestsOf(cur.facts)
      shared = usefulE ∩ remaining
      if ∅ ⊊ shared ⊊ usefulE:
        patricia_split(Q, cur, e, shared)
    # Pick an outgoing edge whose useful dispatch ⊆ remaining (after
    # the splits, every overlapping edge's useful dispatch is fully
    # inside remaining).
    existing = Asks(Q, cur)
    e = find e in existing where (e.requestSet \ requestsOf(cur.facts)) ⊆ remaining
                              and (e.requestSet \ requestsOf(cur.facts)) ≠ ∅,
        or None
    if e is None:
      # No followable edge; add a fresh one covering what's left.
      insert Asks(Q, cur, remaining)
      cur = factSet
    else:
      cur = storage.extend(cur, factsForRequests(e.requestSet, factSet))
  insert Terminals(Q, factSet, result)
```

After the eager split pass each iteration, the useful-dispatch
invariant holds *and* every outgoing edge's useful dispatch is either
entirely inside or entirely outside `remaining`. Advancing through
any inside edge consumes part of `remaining`; the loop reduces
`remaining` strictly until `cur` reaches `factSet`.

Per `record` cost: O(|factSet|) iterations in the worst case, each
O(|existing|) split candidates checked plus an `INSERT OR IGNORE` or
a follow; Patricia split adds O(|requestSet|) when triggered. Per
event: O(1).

**Volume risk on fan-out and a possible mitigation.** If many
recordings introduce different next-Request shapes at the same
`(Q, cur)`, the number of outgoing edges grows and the per-iteration
overlap scan becomes O(|existing|). A reverse index — Request → set
of edges at `(Q, cur)` whose useful dispatch contains that Request —
reduces the overlap search to O(|remaining|) lookups. The index has
one entry per Request per edge, maintained on insert/split. Flagged
as an optimisation; the basic algorithm above doesn't depend on it.

## What Phase 1 covers

- Navigated replay: O(trace length) hash lookups + the unavoidable
  cost of recursively replaying each Request. No candidate scans, no
  probabilistic prefilters.
- Incremental recording: O(events) total, with O(|requestSet|) per
  Patricia split when one is triggered.
- Structural sharing: content-addressed RequestSet and FactSet
  pools with parent-pointer delta encoding. Shared prefixes across
  unrelated `Q`s collapse automatically.
- Patricia split keeps shape divergence cheap on the RequestSet
  axis without introducing speculation at lookup.

## The Phase 1 limitation Phase 2 addresses

Phase 1 hits only **exact-response paths**: at every step the actual
Responses must extend the source FactSet to a next-FactSet hash that
already exists in storage. A source edit that flips a Response
diverts the path to a previously-unseen FactSet hash and Phase 1
sees that as a miss, even if the flipped Response was irrelevant to
the eventual Result.

Phase 1 also has no notion of trace length. Recordings made under
over-approximated context (`nix eval .#a .#b` lands a wide factSet
in `Q_a`'s recording, including `Q_b`'s asks) end up storing wider
traces than necessary, and Phase 1 can't tell that a leaner later
recording of the same Q would be preferable.

## Phase 2 sketch

Two mostly-independent pieces, both incremental, both preserving
"no batch maintenance".

### Recording: passive-replay-before-insert

At `ResultProduced(Q, R)`, before calling `record(Q, factSet, R)`,
do a **passive replay** from `(Q, ∅)` using only the Facts already
in `factSet` — no new Request dispatch. Walk as far as the existing
graph allows:

- **Reach a Terminal for `R`**: existing graph already produces
  this Result for some subset of our Facts. Skip `record`; we'd
  only be adding a redundant superset Terminal.
- **Reach a Terminal for a different `R'`**: model-level ambiguity
  (genuine nondeterminism, or stale wider precondition that no
  longer holds). Policy question — store both, invalidate existing,
  etc. Deferred.
- **Stuck at some `(Q, cur)` with no followable edge** (the
  existing chain requires Facts we don't have): fall through to
  normal `record` from `cur`. The new recording integrates as a
  sibling path; pre-existing wider Terminals coexist until evicted.

What this suppresses: future recordings whose `factSet` is a
superset of an already-reachable Terminal's precondition. What it
doesn't shrink: pre-existing wider Terminals (lazy eviction is a
separate piece, deferred).

### Replay: distance-to-any-R as navigation heuristic

Per `(Q, cur)` node, store one number: the minimum number of Facts
on the shortest known path from `cur` to any downstream Terminal
for `Q`. Per node, not per-`R` — that map would be heavy and would
have to be maintained on every split.

At multi-outgoing-edge positions, replay's `pick from outgoing`
consults this distance: pick the edge whose target has the smallest
`|usefulDispatch(edge)| + dist(target)`. The chosen edge is most
likely to reach a hit with the fewest Request dispatches.

Maintenance:

- **Terminal insert at `(Q, factSet)`**: `dist := 0`. Walk back
  along the Asks chain; at each ancestor,
  `dist(ancestor) := min(dist(ancestor),
                         |usefulDispatch(edge_to_child)| + dist(child))`.
  Stop when no update is needed. O(path length).
- **Patricia split**: at split time the new intermediate's distance
  is set from its single known outgoing tail (the existing branch):
  `dist(intermediate) := |usefulDispatch(tail_existing)| + dist(factSet_existing)`.
  The new branch's contribution arrives later when the recording
  completes and the Terminal-insert back-walk passes through the
  intermediate, lowering its distance and propagating up. The
  split itself doesn't change `dist(cur)` — it inserts an
  intermediate hop whose total cost equals the original edge's
  cost; only future improvements propagate upward.
- **Eviction** (deferred): a node's distance may *increase* when a
  Terminal disappears, requiring re-derivation from surviving
  children.

Correctness in a deterministic-box model: any Terminal we reach
gives a correct Result; the heuristic only affects how many
Requests get dispatched en route, not the answer. With genuine
nondeterminism the heuristic might serve a different Terminal than
another policy would, but that's a model-level ambiguity.

Where it doesn't help: single-outgoing-edge positions (the common
Patricia-normalised case). It earns its keep at path-divergence
forks (`a.nix → b/c` vs `d`).

### Together

Recording-side stops adding redundant supersets going forward.
Replay-side picks shortest-known path among outgoing edges. Over
time, the cache accumulates shorter chains for each Q as new
recordings prove they're shorter; the heuristic exploits them at
lookup. Eviction of stale wider Terminals once a strictly-shorter
one exists is a third, deferred piece.

## Explicitly out of scope

- **`builtins.cache`** — explicit user-controlled cache scopes need
  separate theory.
- **Ambient incoming events (the model's "next layer up")** — handled
  by a different mechanism, not part of this model. Generalising the
  decision graph to that layer is the eventual home of
  `builtins.cache`-style scoping.
- **Replay-time nondeterminism handling** — a model-layer concern. If
  the box truly produces multiple Responses under the same precondition,
  the data structure can store both but the choice of which to serve is
  a policy question.
- **Batch maintenance / compaction** — none. Every operation maintains
  the structure as it goes.

## Open questions left to Phase 2

- Eviction policy for pre-existing wider Terminals once a strictly
  shorter Terminal for the same `(Q, R)` exists. Includes the
  distance-recomputation problem on eviction (a node's distance may
  increase, requiring re-derivation from surviving children).
- Conflicting-Result Terminals encountered during passive replay —
  whether to invalidate, store both, or escalate as a policy
  question.
- Whether to add precondition-intersection learning on top of
  passive-replay-before-insert, for cases where no single session
  produces a minimal trace and the cache stays stuck at the
  wider-than-necessary first recording.

## Implementation notes (Phase 1 as shipped)

Diversions from the storage-layer sketches above, all driven by
profiling at K=1..10000 nixpkgs-attr workloads:

- **FactSet hash: XOR over per-element hashes, not canonical-sort
  Merkle.** Set extension is O(1) (`H(S ∪ {e}) = H(S) ⊕ H_e(e)`
  given `e ∉ S`); insertion is order-independent so canonical;
  algebraically weaker than SHA-Merkle, fine for an internal
  cache. FactSets are not persisted at all — Asks/Terminals are
  keyed by hash but walk reconstructs `cur` and `curRequests`
  incrementally per call.
- **RequestSet pool: hash-prefix trie of content-addressed nodes**,
  not parent-pointer delta encoding. 16-way fanout, leaves split
  past `TRIE_SPLIT_THRESHOLD = 16`. Two RequestSets that overlap
  share their subtrees automatically via content addressing of
  node payloads.
- **Response payloads not persisted.** Walk dispatch recomputes
  from the live environment; only the hash participates in cur
  extension. Request payloads stay (walk needs the path to dispatch).
- **`TracingWriter` maintains the v13FactSet's running hash,
  `seenRequests`, `responseFor`, and an incremental
  `TrieBuilder` for allRequests.** Each `logResponse` is O(log N)
  for the trie insert; each `logResult` is O(1) for the cached
  factSet hash + trie root hash, plus persisting any new trie
  nodes since the last `logResult`. `record()`'s fast-path
  overload takes the precomputed RS hash and jumps `cur` straight
  to `factSetHash` for fresh-Q whole-remaining edges, skipping
  `insertRequestSet` entirely.

What this buys, end-to-end on a K=10000 nixpkgs-attr sweep:

| metric              |  starting | shipped Phase 1 |
|---------------------|----------:|----------------:|
| Cold-record at K=10000 | ~3+ hours (extrapolated) | 277s (17s overhead) |
| Per-attr record cost | super-linear in K | constant ~1.7 ms/attr |
| Warm-replay at K=10000 | unmeasurable | 329s (still O(K²) — Phase 2 target) |
| DB size at K=10000   | extrapolated GBs | 41 MB |

The warm-replay K² is structural to Phase 1's over-approximation
model and not addressable by code-level optimisation — see the
Phase 2 sketch above. The cold-record K² that the prior bullet
also expected ("O(events) total, with O(|requestSet|) per
Patricia split") needed the incremental `TrieBuilder` to actually
deliver, since `insertRequestSet` over the global allRequests
would otherwise re-sort and re-trie the whole growing factSet on
every `record()`.
