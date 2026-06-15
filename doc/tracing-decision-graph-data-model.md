# Trace Index Data Model (Decision Graph) — Phase 1

Successor to [`tracing-sets-index-data-model.md`](tracing-sets-index-data-model.md), the
flat-CBOR sets-based index currently shipped on this branch. This document
specifies the decision-graph model that the sets-based index was always
meant to be a stepping-stone toward. It covers Phase 1 only:
intersectionless recording and replay. Phase 2 (precondition intersection)
will be specified separately and grounded in this baseline.

## Motivation

The shipped sets-based index records each d=0 Query's preconditions as
flat content-addressed CBOR blobs and looks them up by scanning every
recorded Binding for the queryHash and filtering by Bloom. Two failure
modes follow:

- **`k` grows with content edits**, not just with new dependency shapes.
  Each `(queryHash, preconditionHash)` is a fresh row whenever any d>0
  response changes, even when the dependency *shape* (which queries the
  box asks) didn't. Repeated edits to source files produce a row per
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

Atomic content-addressed entities (each its own table or column,
keyed by content hash):

- **Fact** — a `(d>0 query, d>0 response)` pair. Hash:
  `SHA-256(queryHash || responseHash)`. The atomic unit of observed
  evaluation context.
- **d=0 Query** — a top-level query, identified by its `queryHash`
  (operation + params + inputHashes).
- **d=0 Response** — a recorded result, identified by its
  `responseHash` over the serialised payload.

Set pools with parent-pointer delta encoding per L7881:

- **QuerySet (QS) pool** — `(setHash, parentHash, extraQueries)`.
  Each set is stored as a delta from some existing parent set; the
  parent chain encodes structural sharing. `setHash` is canonical,
  so content-equivalent sets always have the same hash regardless of
  how they were built.
- **ResponseSet (RS) pool** — `(setHash, parentHash, extraFacts)`.
  Same scheme for sets of Facts.

The storage layer has no per-`Q` structure, no notion of edges, no
notion of "next". It is queried purely by content hash: "give me the
members of this set" or "is this set in the pool?".

## Decision graph layer

The decision graph is a relational structure over storage hashes.

Two node-typed positions, both identified by storage hashes:

- **RS positions** — identified by an RS storage hash. Represents
  "the box has observed exactly these Facts as context."
- **QS positions** — identified by a QS storage hash. Represents
  "the set of queries the box asks next, as a single transition."

Three edge types:

- **RS ──asks──▶ QS** — at this RS, the box's next set of asks is
  this QS. After Patricia normalisation, an RS has pairwise-disjoint
  outgoing QS edges (and almost always exactly one — see below).
- **QS ──tup──▶ RS** keyed by **response tuple** — given those
  responses to the asked queries, the resulting RS is the union of
  the source RS's facts and the new (query, response) facts. Multiple
  observed response tuples produce multiple keyed children of the
  same QS.
- **RS ──terminal──▶ Response** for a particular d=0 Query `Q` —
  this RS is a recorded precondition under which `Q`'s d=0 Response
  is the recorded one. A single RS may carry terminal edges for
  multiple `Q`s.

A trace through the graph for evaluating `Q` is therefore the
alternating chain:

```
(entry RS) ──asks──▶ QS ──tup──▶ RS ──asks──▶ QS ──tup──▶ ... ──terminal──▶ R
```

The decision graph is per-`Q` only in the sense of where terminal
edges attach. Every RS and QS position is shared across all `Q`s by
content addressing — when two unrelated `Q`s happen to see the same
intermediate context, they reference the same storage RS hash and
share the same position in the graph.

Implementation-wise this is three edge tables:

- `Asks(rs_hash, qs_hash)` — outgoing QS edges per RS.
- `Outcomes(qs_hash, response_tuple, next_rs_hash)` — keyed children
  per QS.
- `Terminals(rs_hash, q_hash, response_hash)` — terminal Response
  edges per RS per d=0 Query.

Plus an entry-point index `Entries(q_hash, entry_rs_hash)` to find
where to start traversal for a given `Q`. None of these tables hold
set content; they hold references into the storage layer.

### Invariant: pairwise-disjoint outgoing QS edges per RS

After Patricia normalisation (below), the outgoing QS edges from a
single RS are pairwise disjoint as sets. Overlap is removed by
factoring the shared prefix into a separate edge.

A consequence: in any deterministic, perfectly-attributed evaluation,
every RS has exactly one outgoing QS edge. Multiple outgoing edges
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
    if rs has a terminal edge for Q: return its Response
    outgoing = rs's outgoing QS edges
    if outgoing is empty: miss
    next_qs = pick from outgoing
    # Edge QS may include queries whose responses are already in rs
    # (the shared prefix from a previous Patricia split). Source those
    # from rs; dispatch only the rest.
    to_dispatch = next_qs \ {queries present in rs.facts}
    fresh = {}
    for q in to_dispatch:
        fresh[q] = walk(q, …)  # recursive d>0 lookup
    tup = canonical_tuple(next_qs, source: rs.facts ∪ fresh)
    rs' = lookup edge (next_qs → ?) keyed by tup
    if not found: miss
    rs = rs'
```

Cost: O(trace length) hash lookups plus the unavoidable cost of
recursively replaying each d>0 query. No scans, no probabilistic
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
arrives, or the d=0 finalisation triggers).

At d=0 finalisation:

```
record(Q, trace):
  rs = Q's entry RS
  for each step (observed_qs, observed_tuple) in trace:
    existing = rs's outgoing QS edges
    case observed_qs vs existing:
      ─ exact match with some e ∈ existing:
          rs' = lookup e's child keyed by observed_tuple
          if missing: insert (e → new RS, keyed by observed_tuple)
          rs = rs'
      ─ disjoint from every e ∈ existing:
          insert (rs → observed_qs → new RS, keyed by observed_tuple)
          rs = new RS
      ─ overlaps some e ∈ existing:
          Patricia-split (above); retry this step
  insert terminal: rs → Response R for Q
```

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
  from concurrent in-flight d=0 Queries). A successful navigation
  terminates at an RS that includes Facts the d=0 Response did not
  actually depend on.
- ✗ The QS→RS response-tuple fan-out grows with content edits: each
  unique combination of d>0 responses produces a new keyed child, even
  if many of them lead to the same d=0 Response.

Both unaddressed items are the substance Phase 2 will tackle by
discovering, when two `(Q, R)` terminals exist with overlapping but
distinct preconditions, that the actually-required precondition is
their intersection. The structural foundation (content-addressed RS
nodes, parent-pointer delta, alternating graph) makes that work cheap.

## Explicitly out of scope

- **`builtins.cache`** — explicit user-controlled cache scopes need
  separate theory.
- **d≥2 (ambient incoming events)** — handled by a different mechanism,
  not part of this model.
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
