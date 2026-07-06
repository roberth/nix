# Tracing eval cache — vocabulary

The dictionary of terms used by the tracing eval-cache design doc
and implementation. Each term is defined once; definitions depend
only on earlier terms.

Three **interactions** — peer conversations between pairs of
participants. **Query** and **Env** are always present and are
defined first. **Ambient** is specific to `builtins.cache`
callbacks and is defined in §10 onward, once the Query/Env
machinery is in hand.

---

## 1. Interactions

Three interactions carry all the atomic conversations the cache
observes and replays. Each is one asker asking one askee; the
atomic conversation is "ask, receive answer." The atom *family*
depends on what protocol the askee speaks — either the full
evaluator `Query`/`Result` surface, or a narrower per-participant
protocol.

| Interaction | Asker → Askee | Atom family | Notes |
|---|---|---|---|
| **Query** | caller → evaluator | `Query` → `Result` | Full evaluator surface. |
| **Env** | inner-evaluator → its environment | varies (see below) | Environment can be filesystem, env vars, or outer evaluator. |
| **Ambient** | outer → inner-supplied local | `Query` → `Result` (wrapped) | Callback arg probes; the wrapper carries a direction tag. |

Within Env, atoms take the participant-specific form:

- **filesystem** — `FileReadRequest` / `FileReadResponse`. Narrow
  protocol: the filesystem only speaks "read this file."
- **env vars** — `GetEnvRequest` / `GetEnvResponse`. Narrow
  protocol: only "get value of this var."
- **outer evaluator** (via `AmbientObject`) — `Query` / `Result`
  payload inside an `AmbientOutgoingRequest` / `Response`
  wrapper. Full evaluator surface; the wrapper adds a
  "outgoing from inner" direction tag.

At Ambient, atoms are `Query` / `Result` payload inside an
`AmbientIncomingRequest` / `Response` wrapper — same full
evaluator surface, direction tag reads "incoming to inner."

Every atom is content-addressed by SHA-256 of its serialized
payload.

---

## The Query interaction

### 2. Atoms

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

### 3. Atoms

Three atom families at Env, one per environment participant.

**FileReadRequest** / **FileReadResponse** — filesystem protocol.
One question shape: "read this path." Response carries a content
hash and optionally the bytes read.

**GetEnvRequest** / **GetEnvResponse** — env-var protocol. One
question shape: "get value of this variable name."

**AmbientOutgoingRequest** / **AmbientOutgoingResponse** —
outer-evaluator protocol (via `AmbientObject`; see §10). Full
evaluator surface: payload is a `Query` / `Result`, wrapped with
a direction tag "outgoing from inner." Same evaluator surface as
the Query interaction — introduced here because the wrapper is
what the walker records and dispatches at Env.

**Request** / **Response** — the collective terms for atoms in
any of the three families above. The walker treats them
uniformly, hashing each into a `requestHash` / `responseHash`
pair (see §5) regardless of participant.

**requestHash** / **responseHash** — content hashes of the
respective payloads.

**Fact** — one `(Request, Response)` pair. The atomic unit of
"the environment behaved this way at this moment."

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
element hashes of members`. Commutative, associative,
self-inverse, identity-with-zero. Extension `H(S ∪ {e})` is O(1)
given the disjointness invariant `e ∉ S`.

### 5. Edges

**Asks edge** — a row in `Asks(queryHash, factSetHash) →
requestSetHash`. "At walker state `(Q, cur)`, the next step is to
dispatch this RequestSet's Requests."

**Terminal** — a row in `Terminals(queryHash, factSetHash) →
resultHash`. "At walker state `(Q, cur)`, the recorded Result for
`Q` is this." A Terminal ends a walk.

**useful (dispatch)** — the subset of an edge's RequestSet whose
Responses aren't already known at `cur`. The walker only
dispatches the useful subset; the rest is skipped as already-known.

**hasAnyEdge** — a cheap existence check on `(Q, cur)`: does any
Asks or Terminal row exist at that key? Used by the walker to
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

**record(Q, factSet, result, ...)** — writes an `(Asks, ...,
Terminal)` chain into the decision graph for a completed
recording.

**Patricia split** — when a new recording's remaining Requests
partially overlap an existing edge's `useful` Requests
(∅ ⊊ shared ⊊ useful), the existing edge is split at the
overlap. Both tail edges reuse the original RequestSets; only the
shared-prefix RequestSet is inserted anew, and dedups against any
other recording producing the same shared set.

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
Asks     (queryHash BLOB, factSetHash BLOB, requestSetHash BLOB,
          PRIMARY KEY (queryHash, factSetHash, requestSetHash)) WITHOUT ROWID
Terminals(queryHash BLOB, factSetHash BLOB, resultHash BLOB,
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
one. Two directions cross this cache boundary and both are
"ambient" — happening at the outer/inner interface:

- **Outgoing (inner → outer)** happens whenever the inner
  interpretation reads a value the outer passed in as an
  argument. The inner asks that outer-supplied value questions
  through `AmbientObject`. This direction is recorded as part of
  the Env interaction (see §3, `AmbientOutgoingRequest/Response`)
  — the outer evaluator counts as one of the inner's
  environment participants.
- **Incoming (outer → inner)** happens whenever the outer
  invokes an inner-supplied callback with an argument that
  originated inside the inner. The outer's callback body then
  probes the inner-supplied argument. This direction is the
  **Ambient interaction proper** and is what §§10–18 cover.

At the wire level both directions wrap the same `Query` /
`Result` payload; the direction tag is what distinguishes
`AmbientOutgoingRequest` from `AmbientIncomingRequest`.

Vocabulary that carries over unchanged from Query/Env:

- `Query`, `Result`, `queryHash`, `resultHash`.
- `Request`, `Response`, `Fact`, `element hash`, XOR-fold.
- `RequestSet`, `factSetHash`.

Ambient-specific vocabulary is what §11 onward defines.

### 11. Ambient atoms and edges

**AmbientIncomingRequest** / **AmbientIncomingResponse** — the
atom family at the Ambient interaction. Payload is a `Query` /
`Result` (full evaluator surface); the wrapper adds a direction
tag "incoming to inner." When the walker records or dispatches an
ambient atom it goes through this wrapper.

**Ambient Asks edge** — a row in `AmbientAsks(fromFactSet) →
(requestSet, toFactSet)`. Same shape as an Env Asks edge but
keyed on factSet alone (no `Q`) and storing the transition
explicitly as `toFactSet` — at replay the walker can't reproduce
an ambient transition by live dispatch, because the "environment"
being asked was an inner-supplied value that no longer exists.

**LocalResponses** — a persistent map
`(requestHash, contextHash) → responsePayload` used by the
replay walker to serve probes into a reconstructed frozen image
of a callback arg. `contextHash` is the walker's Env-interaction
`cur` at the time the response was recorded, disambiguating
same-request observations under different outer contexts.

### 12. Subject identity

Values that cross a cb-apply boundary need a stable structural
name so recording and replay agree on identity even when the
value's content evolves through observations. That name is a
**Subject**.

**Subject** — the algebraic form of an identifier. Four variants:

- **PositionalSeed{depth}** — a callback arg at a static
  apply-stack depth (reverse De Bruijn).
- **DerivedSubject{parent, kind, name/index}** — a value reached
  by `getAttr`/`getListElem` on a parent Subject.
- **ApplyResultSubject{fn, arg}** — the result of applying one
  Subject to another.
- **PostulatedIdempotentRead{hash}** — a subject whose source is
  postulated to be re-readable idempotently (file, expression
  string, literal); the carried hash identifies the source.

Subjects are **stable** — same structural shape, same Subject.
The Subject is the algebraic form of an **argId**.

**argId** — a subject's stable identity. Two representations of
the same identity: the Subject value (algebraic) and its atomic
SHA-256 hash (`argIdHash`). Under the naming discipline of the
codebase, `Id`-suffixed names promise this invariance across
observations, ancestry, and invocations.

**argIdHash** — the hash-form representation of an argId. Local
variables typed `Hash` that hold a subject's atomic base hash
use this suffix.

### 13. State hash — the subject's evolving identity

**state hash** — the value a subject characterizes to at a walk
position. Combines the subject's Subject value, the enclosing
argAncestry, and any observations already folded in. State hashes
evolve as observations accumulate; they are situational, not
stable ids.

**stateHashAt(argId, argAncestry, walk, k)** — the state hash of
an argument-level subject at the precondition of edge `k` in the
walk. Traps on `DerivedSubject` — derived values don't have their
own observations; use `subjectHashAt` for a universal accessor.

**stateHashAfter(argId, argAncestry, walk)** — `stateHashAt` at
`k = walk.size()`.

**stateHashConverged(argId, argAncestry, observations)** — the
grouping-invariant fixed point of the state hash over an
unordered observation set. Depends only on the *set* of
observations, not on how they were partitioned into edges. Used
by the replay walker as a fallback when the walk-order navigation
misses.

**subjectHashAt(subject, argAncestry, walk, k)** — universal
dispatch accessor. For arg-level Subjects returns
`stateHashAt(...)`. For `DerivedSubject` returns the producer
QueryGetAttr's queryHash — the Queries-pool key of the query that
would produce the derived value at walk position k. Different
values identified (subject state vs. query payload) unified in
one accessor for caller convenience.

**fromStateHashOf(query)** — reads the `from` field of a query
payload and returns it as a Hash. Every observation a subject
emits carries `stateHashAt(...)` at the emission time in this
field.

### 14. argAncestry — the enclosing scope of a subject

**argAncestry** — a Hash. The XOR-fold of enclosing callback
args' state hashes at the moment the innermost callback was
entered. Not a lexical scope — `let` bindings and other lexical
constructs don't cross the cache boundary; only callback
arguments do.

An argAncestry is itself a state hash (situational), not a stable
id. Its value depends on what observations have flowed into the
outer arg states before entry.

**inheritedScope / callArgAncestry** — argAncestry stored on
specific objects: `argAncestry` on `AmbientObject` / callback-arg
proxies is the ancestry inherited from the enclosing scope;
`callArgAncestry` on `AmbientResolver` is the ancestry of the
cache call itself, sampled at cb-apply fire time.

**combineArgAncestries(fnArgAncestry, argArgAncestry)** — the
non-commutative combinator producing the argAncestry inside an
apply-result callback body. Non-commutative because `f a` ≠ `a f`
(cf. `flip apply`); computed as
`SHA-256("apply-argAncestry:" || fnHex || ":" || argHex)` rather
than XOR.

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

**AmbientObject** — outer-evaluator-side proxy for an
inner-supplied value in the callback body. Distinct from
TracingCallbackArg (which is the *writer's* view of the arg);
AmbientObject is what the *outer evaluator* sees when it holds
the arg during the callback body.

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

**SubjectEvolutionEdges** — a persistent table storing
`(argIdHash, curBefore, obs.from, obs.elem) → curAfter` — one row
per single-observation fold step. Populated by the writer via a
callback during `stateHashAtStamping`; consumed by the replay
walker to navigate a subject's evolution in O(1) per step.

**stateHashAtStamping(...)** — the writer-side variant of
`stateHashAt` that emits `EvolutionStep` records to the callback
as it folds. Structurally equivalent to `stateHashAt`; used only
at record time.

**subjectHashAfter / structuralIdAfter** — see §13; the `*After`
convention applies uniformly at `k = walk.size()`.

### 18. Ambient storage tables (additions)

Extends the base schema (§9):

```
LocalResponses(requestHash BLOB, contextHash BLOB, payload BLOB,
               PRIMARY KEY (requestHash, contextHash)) WITHOUT ROWID
AmbientAsks(fromFactSetHash BLOB, requestSetHash BLOB, toFactSetHash BLOB,
            PRIMARY KEY (fromFactSetHash, requestSetHash)) WITHOUT ROWID
SubjectEvolutionEdges(argIdHash BLOB, curBefore BLOB, obsFromHash BLOB,
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

Characterizations (of a state or payload) may be represented as
**data** (a `walk` vector of Edges, the members of a FactSet,
`observations`) or as **hashes** (`stateHash`, `factSetHash`,
`argAncestry`, `queryHash`). The rule about `Id` is a stability
promise; the `Hash` suffix is just a type marker.

## Appendix B: what this dictionary does not cover

- `builtins.cache` primop wiring — see `tracing-eval-cache-primop.md`.
- The design rationale (why XOR-fold, why Patricia split, why
  subject-evolution edges) — see `tracing-eval-cache.md`.
- Historical vocabulary and the transitions from it — see git
  history and the (throwaway) `tracing-eval-cache-vocabulary-cleanup.md`
  plan.
