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
(Q, entry FactSet)
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

Implementation-wise this is two edge tables plus an entry-point
index:

- `Asks(queryHash, factSetHash, requestSetHash)` — outgoing
  RequestSet edge per `(Q, FactSet)` position.
- `Terminals(queryHash, factSetHash, resultHash)` — terminal Result
  edges per `(Q, FactSet)` position.
- `Entries(queryHash, entryFactSetHash)` — where to start traversal
  for a given `Q`.

None of these tables hold set content or Responses; they hold
references into the storage layer.

### Invariant: pairwise-disjoint outgoing RequestSet edges per (Q, FactSet)

After Patricia normalisation (below), the outgoing RequestSet edges
from a single `(Q, FactSet)` position are pairwise disjoint as sets.
Overlap is removed by factoring the shared prefix into a separate
edge. (Two different `Q`s at the same FactSet hash naturally have
independent edge sets — the invariant is per-position, not
per-FactSet-hash.)

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
dispatch sharing. It is distinct from the precondition intersection
Phase 2 will introduce, even though both use the same set-intersection
primitive.

When the two RequestSets are fully disjoint
(`requestSet_shared = ∅`), the split degenerates:
`FactSet_intermediate = FactSet`, the shared edge is a no-op, and we
fall back to FactSet carrying two parallel outgoing RequestSet edges.
In practice this is rare for real Nix evaluations, which routinely
share common-prefix Requests (builtins access, environment reads,
common imports).

## Operations

### Replay

Replay is **cache-driven**: no interpreter steps us through. The cache
navigates the graph independently and only falls back to the inner
evaluator on miss.

```
walk(Q, entryFactSet):
  factSet = entryFactSet
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

The recorder observes the interpreter as a black box. Per-event cost is
O(1): each observed `(Request, Response)` is appended to the in-flight
session's running Fact list. A RequestSet is closed when the recorder
has stable evidence that the batch is complete (e.g., the next response
wave arrives, or the Query finalisation triggers).

At Query finalisation:

```
record(Q, trace):
  factSet = Q's entry FactSet
  for each step (observedRequestSet, observedFacts) in trace:
    # observedFacts are the Facts the recorder saw at this step,
    # one per Request in observedRequestSet.
    existing = Asks(Q, factSet)
    case observedRequestSet vs existing:
      ─ exact match with some rs ∈ existing:
          # The RequestSet edge already exists. Compute the next FactSet
          # by storage-layer extension; storage adds it if novel.
          factSet' = storage.extend(factSet, observedFacts)
          factSet = factSet'
      ─ disjoint from every rs ∈ existing:
          insert Asks(Q, factSet, observedRequestSet)
          factSet' = storage.extend(factSet, observedFacts)
          factSet = factSet'
      ─ overlaps some rs ∈ existing:
          Patricia-split (above); retry this step
  insert Terminals(Q, factSet, Result R)
```

Patricia split mutates `Asks` for the affected `(Q, factSet)`
position and inserts new FactSet nodes via storage-layer extension;
it does not change `(Q, factSet)` itself, only its outgoing structure.

Storage operations are content-addressed `INSERT OR IGNORE`s, so
recurring nodes deduplicate. Per step cost is O(1) hash lookups, plus
O(|requestSet|) when a split occurs. Per-trace cost is therefore
O(trace length + splits · |requestSet|).

## What this does and does not solve

Phase 1 alone:

- ✓ Replay is navigated, not scanned. Hot path is O(trace length).
- ✓ Recording is incremental and O(events) total.
- ✓ Storage shares structure via content addressing and parent-pointer
  deltas.
- ✓ Patricia split keeps shape divergence cheap on the RequestSet
  axis.
- ✗ Preconditions can still be wider than necessary (over-approximation
  from concurrent in-flight Queries). A successful navigation
  terminates at a FactSet that includes Facts the Result did not
  actually depend on.
- ✗ The fan-out of next-FactSets from a single RequestSet grows with
  content edits: each unique combination of Responses produces a new
  next-FactSet hash in the storage layer, even if many of those
  next-FactSets ultimately lead to the same Result.

Both unaddressed items are the substance Phase 2 will tackle by
discovering, when two `(Q, R)` terminals exist with overlapping but
distinct preconditions, that the actually-required precondition is
their intersection. The structural foundation (content-addressed
FactSet nodes, parent-pointer delta, alternating graph) makes that
work cheap.

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

- Where does precondition intersection live in the operation order, and
  how does it interact with the per-(Q, R) terminal cluster?
- Inverted Fact index for the "find shortcuts whose precondition is a
  subset of current context" lookup that the cold fallback path needs.
- Eviction of subsumed terminal edges (edge-only; FactSet nodes remain
  for navigation).
- Recorder's RequestSet batching policy: per-event singletons vs.
  accumulated batches, and the trade-off against post-Phase-1 sharing.
