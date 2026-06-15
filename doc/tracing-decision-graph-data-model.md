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
   the storage layer using set hashes as identifiers. Encodes the
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

Query/Result and Request/Response don't share tables or hash spaces
— they're different roles. (The previous model conflated them in a
single shared table to make the algorithm generalise cleanly to
`builtins.cache`-style `d=2`; we revisit that when builtins.cache
comes into scope. For now, separate.)

Atomic content-addressed entities:

- **Query** — `(queryHash, payload)`.
- **Result** — `(resultHash, payload)`.
- **Request** — `(requestHash, payload)`.
- **Response** — `(responseHash, payload)`.
- **Fact** — a `(requestHash, responseHash)` pair. Hash:
  `SHA-256(requestHash || responseHash)`. The atomic unit of
  observed evaluation context.

Set pools with parent-pointer delta encoding per L7881:

- **QuerySet (QS) pool** — `(setHash, parentHash, extraRequests)`.
  A set of Requests. (Historical name from when the d=1 entities
  were called queries; members are Requests under the current
  naming.) Each new set is a delta from some existing parent set;
  the parent chain encodes structural sharing.
- **ResponseSet (RS) pool** — `(setHash, parentHash, extraFacts)`.
  A set of Facts. (Historical name; each member Fact pairs a
  Request with the Response the box observed.)

The `setHash` is canonical: content-equivalent sets always have the
same hash regardless of how they were built. Multiple parent-pointer
decompositions of the same `setHash` may coexist (whichever parent
the recorder picked).

Set extension is the storage-layer operation that the decision
graph relies on for transitions: given an RS hash and a set of new
Facts, produce the new RS hash. The choice of set-hashing scheme
(canonical sorted-Merkle vs. additive/xor) is a storage-layer
design knob that affects the cost of extension but not the
decision-graph semantics — recorded here only as a flag, not a
decision.

The storage layer has no per-`Q` structure, no notion of edges, no
notion of "next". It is queried purely by content hash, or by the
set-extension operation.

## Decision graph layer

The decision graph is a relational structure over storage hashes.

Within the evaluation of a specific Query `Q`, the box's next ask
is determined by `(Q, current RS)`: the *same* RS reached during
two different `Q` evaluations may have entirely different next-asks,
because the Query itself drives what's needed. So decision-graph
positions on the asks side are pairs:

- **`(Q, RS)` positions** — identified by `(queryHash, rsHash)`.
  Represents "while evaluating `Q`, the box has observed exactly
  these Facts."
- **QS positions** — identified by a QS storage hash alone.
  Represents "this set of Requests the box asks next." QS
  positions are Q-free because the QS → next-RS transition is a
  pure structural union and doesn't depend on which `Q` is in
  flight; multiple `Q`s that happen to dispatch the same QS share
  the same downstream structure.

Two edge types:

- **`(Q, RS) ──asks──▶ QS`** — while evaluating `Q` from this RS,
  the box's next set of Requests is this QS. After Patricia
  normalisation, a `(Q, RS)` position has pairwise-disjoint
  outgoing QS edges (and almost always exactly one — see below).
- **`(Q, RS) ──terminal──▶ Result`** — this RS is a recorded
  precondition under which `Q`'s recorded Result is the named one.

There is no third edge type for "QS to next RS". The transition is
implicit: after dispatching the QS and observing Responses, the
new Facts are computed (pairing each Request in QS with its
Response), and the next RS is the storage-layer extension of the
source RS by those new Facts. The next-RS hash is computable from
the source RS hash and the new Facts via the storage layer's
set-extension operation. The decision graph never names a tuple of
Responses because the Responses are transient data used to compute
Facts; they live in the dispatch path at replay/recording time, not
in the schema.

A trace through the graph for evaluating `Q` is therefore the
chain:

```
(Q, entry RS)
   │ asks
   ▼
   QS
   │  (dispatch QS's Requests; observe Responses;
   │   form new Facts; extend source RS in storage layer)
   ▼
(Q, RS')
   │ asks
   ▼
   QS
   │  ...
   ▼
(Q, RS_final) ── terminal ──▶ Result R
```

`Q` carries through every position; `RS` accumulates Facts; QSes
appear as edge labels.

Storage-level sharing is automatic at every layer:

- **RS hashes** are shared across all `Q`s by content addressing.
  Two unrelated `Q`s whose evaluations pass through the same Fact
  set reference the same RS hash, even though they occupy distinct
  `(Q, RS)` positions in the decision graph.
- **QS hashes** are shared across all `Q`s. When two `Q`s' Asks
  edges point at the same QS, the QS itself is one stored node.
- **Fact hashes** are shared across all RSes that contain them, via
  the RS pool's parent-pointer delta encoding.

Implementation-wise this is two edge tables plus an entry-point
index:

- `Asks(queryHash, rsHash, qsHash)` — outgoing QS edge per
  `(Q, RS)` position.
- `Terminals(queryHash, rsHash, resultHash)` — terminal Result
  edges per `(Q, RS)` position.
- `Entries(queryHash, entryRsHash)` — where to start traversal for
  a given `Q`.

None of these tables hold set content or Responses; they hold
references into the storage layer.

### Invariant: pairwise-disjoint outgoing QS edges per (Q, RS)

After Patricia normalisation (below), the outgoing QS edges from a
single `(Q, RS)` position are pairwise disjoint as sets. Overlap is
removed by factoring the shared prefix into a separate edge. (Two
different `Q`s at the same RS hash naturally have independent edge
sets — the invariant is per-position, not per-RS-hash.)

A consequence: in any deterministic, perfectly-attributed evaluation,
every `(Q, RS)` position has exactly one outgoing QS edge. Multiple outgoing edges
means either the box is nondeterministic at this precondition (rare in
real Nix) or the precondition over-approximates — the recorder
attributed Facts to this RS that weren't actually dependencies, and the
divergence is driven by something not represented here. The latter is
the dominant case in practice and is exactly the symptom Phase 2 will
collapse.

### Patricia split

When a recording extends the graph at RS with an observed next-QS that
overlaps but does not equal an existing outgoing edge's QS, the
overlapping edge is split:

```
Before:                       After:
   RS                            RS
   |                             |
  QS_existing ─▶ RS_existing    QS_shared  (new content-addressed
                                 │          node = QS_existing ∩ QS_new)
                                 ▼
                                RS_intermediate
                               ╱              ╲
                          QS_existing       QS_new
                              ▼                ▼
                       RS_existing          RS_new
```

`RS_intermediate` is content-addressed by `RS ∪ (responses to
QS_shared)`. If the two traces agreed on the responses to the shared
queries, they converge on the same `RS_intermediate` automatically by
content addressing; if they disagreed, they end up at different
`RS_intermediate` nodes keyed on the differing response tuples (this is
the ordinary QS→RS keyed-child mechanism).

**Edge labels are whole-set references, not computed deltas.** Both
tail edges keep references to the *original* QuerySet nodes
(`QS_existing` and `QS_new`) that the recorder observed — the same
nodes that the QuerySets table already stores by content hash. Only
`QS_shared` is a freshly inserted QuerySet node, and it deduplicates
against any other recording that happened to produce the same
intersection. This means a split inserts at most one new QuerySet node
regardless of how often the same tail QSes appear elsewhere in the
graph.

A consequence is that an edge's QS may contain queries whose responses
are already in the current RS at that edge's source — the queries
asked along the way to that RS. Replay dispatches only the difference:
`edge.QS \ {queries already in current RS}`, sourcing the
already-observed responses from current RS and dispatching only the
new ones. The response tuple keyed on the edge still covers all of
`edge.QS` (already-observed plus freshly dispatched), so QS→RS keyed
children are well-defined and content-addressed RS dedup still works.

The split is *set intersection used structurally* for storage and
dispatch sharing. It is distinct from the precondition intersection
Phase 2 will introduce, even though both use the same set-intersection
primitive.

When the two QSes are fully disjoint (`QS_shared = ∅`), the split
degenerates: `RS_intermediate = RS`, the shared edge is a no-op, and we
fall back to RS carrying two parallel outgoing QS edges. In practice
this is rare for real Nix evaluations, which routinely share common
prefix queries (builtins access, environment reads, common imports).

## Operations

### Replay

Replay is **cache-driven**: no interpreter steps us through. The cache
navigates the graph independently and only falls back to the inner
evaluator on miss.

```
walk(Q, entryRS):
  rs = entryRS
  loop:
    if Terminals has (Q, rs, R): return R
    outgoing = Asks(Q, rs)  # the outgoing QS edges for this (Q, rs)
    if outgoing is empty: miss
    next_qs = pick from outgoing
    # Edge QS may include Requests whose Responses are already in
    # rs (the shared prefix from a previous Patricia split). Source
    # those from rs; dispatch only the rest.
    to_dispatch = next_qs \ {Requests present in rs.facts}
    fresh = {}
    for req in to_dispatch:
        fresh[req] = walk_request(req, …)  # recursive Request lookup
    new_facts = {Fact(req, fresh[req]) for req in to_dispatch}
              ∪ {Fact(req, rs.responseFor(req)) for req in next_qs \ to_dispatch}
    rs' = storage.extend(rs, new_facts)  # next_rs_hash by set extension
    if rs' is not in the RS pool: miss
    rs = rs'
```

Cost: O(trace length) hash lookups plus the unavoidable cost of
recursively replaying each Request. No scans, no probabilistic
filters. The `next_qs \ rs.facts` subtraction is the price of
whole-set edge labels — paid in O(|next_qs| + |rs|) per step, which is
dominated by the dispatch work itself.

The `pick from outgoing` step has only one choice in the
common case (single outgoing edge after Patricia normalisation).
When multiple edges are present, the choice is a knob:

- **Single-edge dispatch**: pick one, follow it; on miss, optionally
  retry the next. Cheapest per step but loses parallelism across
  branches.
- **Parallel dispatch**: dispatch the union of all outgoing QSes; at
  most one branch's response-tuple key matches and we follow it.
  Wasted work on the others, but the results are cached.

Either is correct. The choice is local to replay and changeable at
runtime; storage is invariant to it.

### Recording

The recorder observes the interpreter as a black box. Per-event cost is
O(1): each observed `(q, r)` is appended to the in-flight session's
running Fact list. A QuerySet is closed when the recorder has stable
evidence that the batch is complete (e.g., the next response wave
arrives, or the Query finalisation triggers).

At Query finalisation:

```
record(Q, trace):
  rs = Q's entry RS
  for each step (observed_qs, observed_facts) in trace:
    # observed_facts are the Facts the recorder saw at this step,
    # one per Request in observed_qs.
    existing = Asks(Q, rs)  # outgoing QS edges at this (Q, rs)
    case observed_qs vs existing:
      ─ exact match with some qs ∈ existing:
          # The QS edge already exists. Compute the next RS by
          # storage-layer extension; storage adds it if novel.
          rs' = storage.extend(rs, observed_facts)
          rs = rs'
      ─ disjoint from every qs ∈ existing:
          insert Asks(Q, rs, observed_qs)
          rs' = storage.extend(rs, observed_facts)
          rs = rs'
      ─ overlaps some qs ∈ existing:
          Patricia-split (below); retry this step
  insert Terminals(Q, rs, Result R)
```

Patricia split mutates `Asks` for the affected `(Q, rs)` position
and inserts new RS nodes via storage-layer extension; it does not
change `(Q, rs)` itself, only its outgoing structure.

Storage operations are content-addressed `INSERT OR IGNORE`s, so
recurring nodes deduplicate. Per step cost is O(1) hash lookups, plus
O(|qs|) when a split occurs. Per-trace cost is therefore
O(trace length + splits · |qs|).

## What this does and does not solve

Phase 1 alone:

- ✓ Replay is navigated, not scanned. Hot path is O(trace length).
- ✓ Recording is incremental and O(events) total.
- ✓ Storage shares structure via content addressing and parent-pointer
  deltas.
- ✓ Patricia split keeps shape divergence cheap on the QS axis.
- ✗ Preconditions can still be wider than necessary (over-approximation
  from concurrent in-flight Queries). A successful navigation
  terminates at an RS that includes Facts the Result did not
  actually depend on.
- ✗ The fan-out of next-RSes from a single QS grows with content edits:
  each unique combination of Responses produces a new next-RS hash in
  the storage layer, even if many of those next-RSes ultimately lead
  to the same Result.

Both unaddressed items are the substance Phase 2 will tackle by
discovering, when two `(Q, R)` terminals exist with overlapping but
distinct preconditions, that the actually-required precondition is
their intersection. The structural foundation (content-addressed RS
nodes, parent-pointer delta, alternating graph) makes that work cheap.

## Explicitly out of scope

- **`builtins.cache`** — explicit user-controlled cache scopes need
  separate theory.
- **Ambient incoming events (the model's "next layer up")** — handled
  by a different mechanism, not part of this model. Generalising the
  decision graph to that layer is the eventual home of
  `builtins.cache`-style scoping; the previous shared-table convention
  in the storage layer was set up to make that generalisation easier
  and may be reintroduced when that work begins.
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
- Eviction of subsumed terminal edges (edge-only; RS nodes remain for
  navigation).
- Recorder's QS batching policy: per-event singletons vs. accumulated
  batches, and the trade-off against post-Phase-1 sharing.
