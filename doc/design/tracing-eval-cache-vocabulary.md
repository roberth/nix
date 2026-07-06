# Tracing eval cache — vocabulary

The dictionary of terms used by the tracing eval-cache design doc
and implementation. Each term is defined once; definitions depend
only on earlier terms.

Three **interactions**, each between two participants. **Query**
and **Env** are always present and are defined first. **Ambient**
is specific to `builtins.cache` callbacks and is defined in §10
onward.

---

## 1. Interactions

Three interactions carry all the ask/answer pairs the cache
observes and replays. Each is one asker asking one askee. The
payload types vary — either the full evaluator `Query`/`Result`
surface, or a narrower per-participant protocol.

| Interaction | Asker → Askee | Payload types | Notes |
|---|---|---|---|
| **Query** | caller → evaluator | `Query` → `Result` | Full evaluator surface. |
| **Env** | inner-evaluator → its environment | varies (see below) | Environment can be filesystem, env vars, or outer evaluator. |
| **Ambient** | outer → inner-supplied local | `Query` → `Result` (wrapped) | Callback arg probes. |

Within Env, payload types depend on the participant:

- **filesystem** — `FileReadRequest` / `FileReadResponse`. Narrow
  protocol: the filesystem only speaks "read this file."
- **env vars** — `GetEnvRequest` / `GetEnvResponse`. Narrow
  protocol: only "get value of this var."
- **outer evaluator** (via `OuterObject`) — `Query` / `Result`
  payload inside an `OuterValueRequest` / `Response`
  wrapper. Full evaluator surface; the wrapper tags the payload
  as being about an outer-owned value.

At Ambient, the payload is a `Query` / `Result` inside an
`InnerValueRequest` / `Response` wrapper — same full evaluator
surface, wrapper tags the payload as being about an inner-owned
callback-arg value.

Every payload is content-addressed by SHA-256 of its serialized
bytes.

---

## The Query interaction

### 2. Payload types

**Query** — an operation the caller asks. `evalFile`, `getAttr`,
`getString`, `apply`, etc. The payload carries the operation, its
parameters, and a `from` field carrying the parent query's
identity (Merkle chain).

**Result** — the value the evaluator returns for a Query.

**queryHash** — content hash of a Query payload. Its identity in
the Queries pool.

**resultHash** — content hash of a Result payload. Its identity in
the Results pool.

---

## The Env interaction

### 3. Payload types

Three payload-type families at Env, one per environment participant.

**FileReadRequest** / **FileReadResponse** — filesystem protocol.
One question shape: "read this path." Response carries a content
hash and optionally the bytes read.

**GetEnvRequest** / **GetEnvResponse** — env-var protocol. One
question shape: "get value of this variable name."

**OuterValueRequest** / **OuterValueResponse** —
outer-evaluator protocol (via `OuterObject`; see §10). Full
evaluator surface: payload is a `Query` / `Result`, wrapped to
tag it as a query about an outer-owned value. Same evaluator
surface as the Query interaction — introduced here because the
wrapper is what the walker records and dispatches at Env.

**Request** / **Response** — the collective terms for payloads in
any of the three families above. The walker treats them uniformly,
hashing each into a `requestHash` / `responseHash` pair (see §5)
regardless of participant.

**requestHash** / **responseHash** — content hashes of the
respective payloads.

**Fact** — one `(Request, Response)` pair. The unit of "the
environment behaved this way at this moment"; the walker records
and dispatches Facts as indivisible.

**element hash** — `SHA-256(requestHash || responseHash)`. The
per-Fact contribution to XOR-fold hashes below.

### 4. Sets

**FactSet** — a set of Facts. Identified by an XOR-fold hash.
FactSet *members* are never persisted; the hash is the identity,
members are recomputed by the walker as needed.

**factSetHash** — the XOR-fold identity of a FactSet.

**RequestSet** — a set of Requests. Identified by SHA-256 of a
canonical serialization of the sorted, deduplicated member list.
Members are persisted in the `RequestSetNodes` trie.

**requestSetHash** — the RequestSet identity.

**XOR-fold** — the FactSet identity function: `H(S) = XOR over
element hashes of members`. Commutative, associative, self-inverse.
Extension `H(S ∪ {e})` is O(1) given `e ∉ S`.

### 5. Edges

**Ask** — a row in `Ask(queryHash, factSetHash) →
requestSetHash`. "At walker state `(Q, cur)`, the next step is
to dispatch this RequestSet's Requests."

**Terminal** — a row in `Terminal(queryHash, factSetHash) →
resultHash`. "At walker state `(Q, cur)`, the recorded Result
for `Q` is this." A Terminal ends a walk.

**useful (dispatch)** — the subset of an Ask's RequestSet whose
Responses aren't already known at `cur`. The walker only
dispatches the useful subset; the rest is skipped as
already-known.

**hasAnyEdge** — a cheap existence check on `(Q, cur)`: does any
Ask or Terminal row exist at that key? Used by the walker to
reject branches that no recording ever passed through.

### 6. Walker state

**cur** — the walker's running factSetHash. Starts at ∅; advances
by XOR-folding each dispatched Fact's element hash. Every named
`*Cur` variable is a specific role of the same value.

**nextCur** — `cur` after XOR-folding one edge's `useful` Facts.

**startCur** — the `cur` the walk starts at. Defaults to ∅; child
queries can start at their parent's `terminalCur`.

**terminalCur** — the `cur` the walker lands at when committing a
Terminal.

**dispatch** — the walker's per-Request callback. Given a
Request, returns a Response by asking the live environment.

**walk(Q, dispatch, ..., startCur)** — the walker's top-level
entry. Returns a `WalkHit` (result + terminalCur) on a Terminal
reach, `nullopt` on miss.

### 7. Recording

**record(Q, factSet, result, ...)** — writes an `(Ask, ...,
Terminal)` chain into the decision graph for a completed
recording.

**Patricia split** — when a new recording's remaining Requests
partially overlap an existing Ask's `useful` Requests
(∅ ⊊ shared ⊊ useful), the existing Ask is split at the
overlap. Both tail Asks reuse the original RequestSets; only
the shared-prefix RequestSet is inserted anew, and dedups
against any other recording producing the same shared set.

### 8. RequestSet trie

**RequestSet trie** — the storage layout for RequestSets. Each
node is content-addressed by SHA-256 of its payload; identical
subtrees are shared across recordings via `INSERT OR IGNORE` on
the node hash.

**leaf node** — up to `TRIE_SPLIT_THRESHOLD` request hashes stored
inline.

**internal node** — sparse map from bucket index → child node hash.

**TRIE_RADIX_BITS** — 4. 16-way fanout. Bucket index at depth `d`
for hash `h` is the `TRIE_RADIX_BITS` bits of `h` starting at bit
`d * TRIE_RADIX_BITS` MSB-first.

**TRIE_SPLIT_THRESHOLD** — 16. A leaf that would exceed this size
splits into an internal node.

**TrieBuilder** — the in-memory helper that grows the trie one
request at a time, maintaining a lazily-computed root hash. Used
by the writer to hand `record()` a precomputed
`sessionRequestsRsHash`.

### 9. Storage tables (Query and Env only)

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

All are append-only via `INSERT OR IGNORE`; reads use prepared
statements with a per-hash in-process cache. `WITHOUT ROWID` on
edge tables collapses the data into the primary-key B-tree.

Notable absences: no `Responses` table (walk recomputes responses
from the live environment), no `FactSets` table (members are
recomputed).

---

## The Ambient interaction

### 10. What the Ambient interaction is

`builtins.cache` nests a cached inner evaluator inside an outer
one. Two categories of value cross the cache boundary and each
gets probed the other way around:

- **Outer-owned values** — Values the outer evaluator produced,
  passed to the inner as arguments. The inner reads them through
  `OuterObject`. Queries about these are `OuterValueRequest`
  and belong to the Env interaction (see §3) — the outer
  evaluator is one of the inner's environment participants.
- **Inner-owned callback-arg values** — Values the inner
  evaluator produced, that the outer receives when it invokes an
  inner-supplied callback. The outer's callback body reads them
  through the callback-arg objects (`TracingCallbackArg` /
  `ReplayCallbackArg`). Queries about these are
  `InnerValueRequest`s and belong to the **Ambient interaction
  proper**, which is what §§11–18 cover.

Both wrappers carry the same `Query` / `Result` payload; what
distinguishes them is which side owns the value being queried.

Vocabulary that carries over unchanged from Query/Env:

- `Query`, `Result`, `queryHash`, `resultHash`.
- `Request`, `Response`, `Fact`, `element hash`, XOR-fold.
- `RequestSet`, `factSetHash`.

Ambient-specific vocabulary is what §11 onward defines.

### 11. Ambient payload types and edges

**InnerValueRequest** / **InnerValueResponse** — the payload
types at the Ambient interaction. Payload is a `Query` / `Result`
(full evaluator surface); the wrapper tags the payload as being
about an inner-owned callback-arg value. When the walker records
or dispatches an ambient Fact it goes through this wrapper.

**AmbientAsk** — a row in `AmbientAsk(fromFactSet) →
(requestSet, toFactSet)`. Same shape as an Env Ask but keyed
on factSet alone (no `Q`) and storing the transition
explicitly as `toFactSet` — at replay the walker can't
dispatch an inner-owned value live, since it no longer exists.

**LocalResponses** — a persistent map
`(requestHash, contextHash) → responsePayload` used by the
replay walker to serve probes into a reconstructed frozen image
of a callback arg. `contextHash` is the walker's Env-interaction
`cur` at the time the response was recorded, disambiguating
same-request observations under different outer contexts.

### 12. Subject identity

Values inside a callback body need a stable name — the value's
content changes as **observations** accumulate, but its identity
should stay pinned. That name is a **Subject**.

**Observation** — a Fact viewed through the subject-identity
lens. Just `(fromHash, elementHash)`:
- `fromHash` — the state hash the subject had when it emitted
  this observation.
- `elementHash` — `SHA-256(requestHash || responseHash)`, same
  as the Fact's contribution to the XOR-fold.

Every Fact yields one Observation per subject that emitted it.

**ObservationSet** — a batch of Observations that share a
precondition state; the walker's fold at each step consumes one
ObservationSet at a time. XOR-folding the member `elementHash`es
yields the delta by which the FactSet's hash changes when this
set is consumed — mathematically the same operation as
`XOR-fold` in §4, but scoped to one step. `struct
ObservationSet { std::vector<Observation> observations; }` in
`subject-id.hh`. A **walk** (§13) is a sequence of
ObservationSets.

**Subject** — a structural identifier for a value. Four variants:

- **PositionalSeed{depth}** — a callback arg at a static
  apply-stack depth (reverse De Bruijn).
- **DerivedSubject{parent, kind, name/index}** — a value reached
  by `getAttr`/`getListElem` on a parent Subject.
- **ApplyResultSubject{fn, arg}** — the result of applying one
  Subject to another.
- **PostulatedIdempotentRead{hash}** — a subject whose source can
  be re-read as if content-addressed (file, expression string,
  literal); the carried hash identifies the source.

Same structural shape → same Subject. A Subject is the value form
of an **argId**.

**argId** — a subject's stable identity, in either form: the
Subject value or its SHA-256 hash (`argIdHash`). `Id`-suffixed
names don't depend on observations, ancestry, or invocations.

**argIdHash** — an argId in `Hash` form. Local variables that
hold a subject's base hash use this suffix.

### 13. State hash — the subject's evolving identity

**state hash** — a subject's identity at a walk position:
combines the Subject, the enclosing argAncestry, and the
observations folded in so far. Evolves as observations accumulate;
situational, not stable.

**stateHashAt(argId, argAncestry, walk, k)** — the state hash of
an arg-level subject at the precondition of edge `k`. Traps on
`DerivedSubject` — derived values have no own observations to
fold; use `subjectHashAt` instead.

**stateHashAfter(argId, argAncestry, walk)** — `stateHashAt` at
`k = walk.size()`.

**stateHashConverged(argId, argAncestry, observations)** — state
hash computed over an unordered observation set: same result
regardless of how observations were grouped into edges. Used by
the replay walker as a fallback when walk-order navigation
misses.

**subjectHashAt(subject, argAncestry, walk, k)** — returns a
hash for any Subject variant. Arg-level: `stateHashAt(...)`.
Derived: the producer QueryGetAttr's queryHash — the Queries-pool
key of the query that would produce the derived value at k. Two
different kinds of hash unified in one call.

**fromStateHashOf(query)** — reads the `from` field of a query
and returns it as a `Hash`. Every observation a subject emits
carries `stateHashAt(...)` at the emission time in this field.

### 14. argAncestry — the enclosing scope of a subject

**argAncestry** — a `Hash`: the XOR-fold of enclosing callback
args' state hashes at the moment the innermost callback was
entered. Not a lexical scope — `let` bindings and other lexical
constructs don't cross the cache boundary; only callback
arguments do. Itself a state hash (situational): its value
depends on what observations have flowed into the outer arg
states before entry.

**callArgAncestry** — an `argAncestry` stored on the
`OuterResolver`, sampled at cb-apply fire time. Distinct from
the `argAncestry` field on callback-arg proxies, which is the
enclosing scope's argAncestry inherited by children.

**combineArgAncestries(fnArgAncestry, argArgAncestry)** —
produces the argAncestry inside an apply-result callback body.
Non-commutative because `f a` ≠ `a f` (cf. `flip apply`);
computed as `SHA-256("apply-argAncestry:" || fnHex || ":" ||
argHex)` rather than XOR.

### 15. Callback arg objects

At each cb-apply boundary the cache tracks the inner-supplied
argument through the outer's probes. Three related object types:

**TracingCallbackArg** — writer-side wrapper. Wraps the
inner-supplied value at the boundary; records the outer's probes
on it as Ambient-interaction Facts.

**ReplayCallbackArg** — replay-side counterpart. Frozen image
reconstructed from `LocalResponses`. Serves the outer's probes
from recorded data; throws an ambient-interaction divergence exception
if the outer's probes don't match what was recorded.

**OuterObject** — the outer evaluator's view of the callback
arg while running the callback body. Peer to `TracingCallbackArg`
(writer view) and `ReplayCallbackArg` (replay view); all three
wrap the same underlying arg from different sides.

### 16. Cell navigation

Values inside a callback body form a proxy chain — an
apply-result is derived from an arg, an attribute is derived from
a value, and so on. The chain is tracked structurally, orthogonal
to state hashes.

**ArgCell** — a scope-graph node carrying `(depth, parent,
liveObject)`. Depth is the reverse-De-Bruijn index of the
callback arg the cell was created for; parent is the next-outer
cell; liveObject is the wrapped Object. Cells are pure topology —
no hashes are stored on them.

**argCell** — a field on writer- and replay-side Object wrappers
holding a `shared_ptr<const ArgCell>`. Children inherit the
parent's cell for navigation.

**withArgCell(...)** — setter to attach a cell to a proxy.

**effectiveArgCell(obj)** / **getProxyArgCell()** — return the
proxy's cell, or null for non-proxy Objects.

### 17. Subject-evolution fast-path

The state hash of a subject at some walk position is a pure
function of `(subject, argAncestry, walk, k)`, but naive
evaluation is O(walk.size()) per query. The subject-evolution
fast-path caches the individual fold-step transitions.

**SubjectEvolutionEdge** — a persistent table storing
`(argIdHash, curBefore, obs.from, obs.elem) → curAfter` — one row
per single-observation fold step. Populated by the writer via a
callback during `stateHashAtStamping`; consumed by the replay
walker to navigate a subject's evolution in O(1) per step.

**stateHashAtStamping(...)** — the writer-side variant of
`stateHashAt` that emits `EvolutionStep` records to the callback
as it folds. Structurally equivalent to `stateHashAt`; used only
at record time.


### 18. Ambient storage tables (additions)

Extends the base schema (§9):

```
LocalResponses(requestHash BLOB, contextHash BLOB, payload BLOB,
               PRIMARY KEY (requestHash, contextHash)) WITHOUT ROWID
AmbientAsk(fromFactSetHash BLOB, requestSetHash BLOB, toFactSetHash BLOB,
           PRIMARY KEY (fromFactSetHash, requestSetHash)) WITHOUT ROWID
SubjectEvolutionEdge(argIdHash BLOB, curBefore BLOB, obsFromHash BLOB,
                     obsElemHash BLOB, curAfter BLOB,
                     PRIMARY KEY (argIdHash, curBefore, obsFromHash, obsElemHash))
                     WITHOUT ROWID
```

Same `INSERT OR IGNORE` discipline. Same per-hash in-process
caches.

---

## Appendix A: naming rules

Two rules the vocabulary above obeys:

1. **`Id` marks stable identity.** A `*Id` name promises the value
   does not depend on observations, walks, argAncestry, or
   invocations. `argId`, `argIdHash`, `PositionalArgId` (proposed
   for `PositionalSeed`) — all stable. `stateHash`, `argAncestry`,
   `factSetHash` — situational, therefore never `*Id`.

2. **`Hash` is neutral.** It says only "the value is a `Hash`."
   Distinctive prefixes clarify what the hash is *of* — `queryHash`
   of a query payload, `resultHash` of a result, `stateHash` of a
   subject's state at a walk position.

## Appendix B: what this dictionary does not cover

- `builtins.cache` primop wiring — see `tracing-eval-cache-primop.md`.
- The design rationale (why XOR-fold, why Patricia split, why
  subject-evolution edges) — see `tracing-eval-cache.md`.
- Historical vocabulary and the transitions from it — see git
  history and the (throwaway) `tracing-eval-cache-vocabulary-cleanup.md`
  plan.
