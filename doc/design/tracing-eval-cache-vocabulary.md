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

Every payload is identified by SHA-256 of its serialized bytes.
Hashes throughout the cache are always of payloads — structured
records of what was asked or answered — never of the Nix values a
payload might reference. Deep-hashing values would force
evaluation and defeat laziness; the cache observes probes
instead.

---

## The Query interaction

### 2. Payload types

**Query** — an operation the caller asks. `evalFile`, `getAttr`,
`getString`, `apply`, etc. The payload carries the operation, its
parameters, and a `from` field carrying the parent query's
identity (Merkle chain).

**Result** — the value the evaluator returns for a Query.

**queryHash** — SHA-256 of a Query payload. Its identity in the
Queries pool.

**resultHash** — SHA-256 of a Result payload. Its identity in the
Results pool.

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

**requestHash** / **responseHash** — SHA-256 of the respective
payloads.

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
resultHash`. A recording that reached `(Q, cur)` produced this
Result. The Terminal *points at* a `resultHash`; the Result
payload itself lives in the Results pool independently. Multiple
Terminals at the same `(Q, cur)` are allowed — same walker
state, different Result — if recorded evaluations diverge
(nondeterminism policy is out of scope here). A Terminal ends a
walk.

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
entry. Returns a `WalkHit` on a Terminal reach, `nullopt` on miss.

**WalkHit** — `{resultHash, terminalCur}`. `resultHash` is the
recorded Result the walk landed on; `terminalCur` is the `cur` at
that Terminal (usable as a child query's `startCur`).

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
node is keyed by SHA-256 of its payload bytes; identical subtrees
are shared across recordings via `INSERT OR IGNORE` on the node
hash.

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
one. Values cross the cache boundary in both directions; whichever
side owns a value, the other side is what probes it:

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
  proper** (§11).

Both wrappers carry the same `Query` / `Result` payload; what
distinguishes them is which side owns the value being queried.

Vocabulary that carries over unchanged from Query/Env:

- `Query`, `Result`, `queryHash`, `resultHash`.
- `Request`, `Response`, `Fact`, `element hash`, XOR-fold.
- `RequestSet`, `factSetHash`.

Ambient-specific payload types and edges are defined in §11. The
subject-identity machinery those Facts hang off — Subject, state
hash, argAncestry, callback-arg objects, cell navigation, the
subject-evolution fast-path — is a separate concern, defined
starting at §12. Storage for both lives in §18.

### 11. Ambient payload types and edges

**InnerValueRequest** / **InnerValueResponse** — the payload
types at the Ambient interaction. Payload is a `Query` / `Result`
(full evaluator surface); the wrapper tags the payload as being
about an inner-owned callback-arg value. Persisted responses live
in the `InnerValueResponse` table below. C++ wire wrappers are
`InnerValueRequestPayload` / `InnerValueResponsePayload` —
`Payload` suffix keeps the atom names free for the storage layer.

**AmbientAsk** — a row in `AmbientAsk(fromFactSet) →
(requestSet, toFactSet)`. Same shape as an Env Ask but keyed
on factSet alone (no `Q`) and storing the transition
explicitly as `toFactSet` — at replay the walker can't
dispatch an inner-owned value live, since it no longer exists.

**InnerValueResponse** — a persistent table
`(requestHash, contextHash) → payload` used by the replay walker
to serve probes into a reconstructed frozen image of a callback
arg. `contextHash` is the walker's Env-interaction `cur` at the
time the response was recorded, disambiguating same-request
observations under different outer contexts.

---

## Subject-identity machinery

Cached functions run many times. Two cases; both put multiple
values through the same syntactic slot, so position alone can't
tell the values apart — something else has to.

The simple case is the outer calling the inner directly:

```nix
builtins.cache { import = ./call-nixpkgs.nix; }
               { system = "x86_64-linux"; }
```

Two invocations with different args land at the same syntactic
slot. Position alone can't tell them apart — a slot name is
just a slot name — and the inner sees each arg as an opaque
`OuterObject` with no way to look inside. Discrimination lives
in *observations*: the inner probes the arg, the outer's
answers carry the value's content one probe at a time, and
different args produce different Facts. Different Fact chains
→ different walks → different Terminals. At replay the walker
dispatches recorded probes back to the outer live and matches
responses against the recording to confirm the hit. The Ambient
machinery in §§12–17 doesn't fire here — it's for the callback
case, where inner-owned values can't be re-probed live.

Callbacks are the case §§12–17 exist for. When the inner
supplies a callback and the outer runs it — `(cb 10) + (cb 20)`,
or nesting inside another cached call whose enclosing
argAncestry differs — the callback arg is inner-owned. At replay
the inner isn't running; its closures are gone; the arg no
longer exists to be probed, so probes have to be served from
storage (`InnerValueResponse`) instead of dispatched live.
`Arg{depth=1}` names the slot; per-invocation
distinction has to come from what the outer *did* with the arg —
the observations it made. Every invocation shares the same
Subject (immutable by construction; independent of history,
argAncestry, invocation); each still produces distinct Facts. The machinery
below closes that gap: §12 fixes the Subject; §§13–14
characterize its state per-invocation via state hash and
argAncestry (with `callArgAncestry` sampled at each cb-apply,
disambiguated in storage by `InnerValueResponse.contextHash`);
§§15–16 wire this through the Object graph; §17 caches
step-by-step transitions. Storage lives in §18.

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
`subject-id.hh`. A **history** is a sequence of
ObservationSets.

**Subject** — a structural identifier for a value. Four variants:

- **Arg{depth}** — a callback arg at a static apply-stack depth
  (reverse De Bruijn).
- **DerivedSubject{parent, kind, name/index}** — a value reached
  by `getAttr`/`getListElem` on a parent Subject.
- **ApplyResultSubject{fn, arg}** — the result of applying one
  Subject to another.
- **PostulatedIdempotentRead{hash}** — a subject whose source
  (file, expression string, literal) is re-read on demand at
  replay; the `hash` is of the source bytes. "Postulated"
  because we assume re-reading the source yields the same value
  — we never verify by inspecting the value.

Same structural shape → same Subject. Subject values are
immutable — a Subject is stable by construction, independent of
history, argAncestry, or invocation.

**subjectHash** — SHA-256 of a Subject payload. Also stable by
construction. Used as a Merkle key when a Subject is referenced
by hash.

### 13. State hash — the subject's evolving identity

**state hash** — a subject's identity at a history position:
combines the Subject, the enclosing argAncestry, and the
observations folded in so far. Evolves as observations accumulate;
situational, not stable.

**stateHashAt(subject, argAncestry, history, step)** — the state
hash of an arg-level subject before step `step` folds in. Traps
on `DerivedSubject` — derived values have no own observations to
fold; use `subjectHashAt` instead.

**stateHashAfter(subject, argAncestry, history)** — `stateHashAt`
at `step = history.size()`.

**stateHashConverged(subject, argAncestry, observations)** — state
hash computed over an unordered observation set: same result
regardless of how observations were grouped into edges. Used by
the replay walker as a fallback when step-by-step navigation
misses.

**subjectHashAt(subject, argAncestry, history, step)** — returns
a hash for any Subject variant. Arg-level: `stateHashAt(...)`.
Derived: the producer QueryGetAttr's queryHash — the Queries-pool
key of the query that would produce the derived value at step
`step`. Two different kinds of hash unified in one call.

**fromStateHashOf(query)** — reads the `from` field of a query
and returns it as a `Hash`. Every observation a subject emits
carries `stateHashAt(...)` at the emission time in this field.

### 14. argAncestry

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
reconstructed from `InnerValueResponse` rows. Serves the outer's
probes from recorded data; throws an ambient-interaction
divergence exception if the outer's probes don't match what was
recorded.

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
holding a `shared_ptr<const ArgCell>`.

**withArgCell(...)** — setter to attach a cell to a proxy.

**effectiveArgCell(obj)** / **getProxyArgCell()** — return the
proxy's cell, or null for non-proxy Objects.

### 17. Subject-evolution fast-path

The state hash of a subject at some history position is a pure
function of `(subject, argAncestry, history, step)`, but naive
evaluation is O(history.size()) per query. The subject-evolution
fast-path caches the individual fold-step transitions.

**SubjectEvolutionEdge** — a persistent table (schema in §18)
with one row per single-observation fold step. Populated by the
writer via a callback during `stateHashAtStamping`; consumed by
the replay walker to navigate a subject's evolution in O(1) per
step.

**EvolutionStep** — one row's worth of data as a struct in
`subject-id.hh`: `curBefore` (subject's state hash before this
observation folds in), the observation's `fromHash` and
`elementHash`, and `curAfter` (state hash after). Emitted by
`stateHashAtStamping` to a callback; the writer's callback
persists each into `SubjectEvolutionEdge`.

**stateHashAtStamping(...)** — the writer-side variant of
`stateHashAt` that emits an `EvolutionStep` for each fold step
via a callback. Structurally equivalent to `stateHashAt`; used
only at record time.


### 18. Storage tables (Ambient and subject-id additions)

Extends the base schema (§9). Ambient (§11) contributes the first
two rows; the subject-evolution fast-path (§17) contributes the
third.

```
InnerValueResponse(requestHash BLOB, contextHash BLOB, payload BLOB,
                   PRIMARY KEY (requestHash, contextHash)) WITHOUT ROWID
AmbientAsk(fromFactSetHash BLOB, requestSetHash BLOB, toFactSetHash BLOB,
           PRIMARY KEY (fromFactSetHash, requestSetHash)) WITHOUT ROWID
SubjectEvolutionEdge(subjectHash BLOB, curHash BLOB, obsFromHash BLOB,
                     obsElementHash BLOB, nextCurHash BLOB,
                     PRIMARY KEY (subjectHash, curHash, obsFromHash, obsElementHash))
                     WITHOUT ROWID
```

Same `INSERT OR IGNORE` discipline. Same per-hash in-process
caches.

---

## Appendix A: naming rules

Two rules the vocabulary above obeys:

1. **Stable vs situational is carried by the type name, not by a
   suffix.** `Subject` and `subjectHash` are stable by
   construction — an immutable algebraic value and its hash.
   `stateHash*`, `argAncestry`, `callArgAncestry`, `factSetHash`
   are situational — their values track observations, ancestry,
   invocations. No `Id` marker is required or used.

2. **`Hash` is neutral.** It says only "the value is a `Hash`."
   Distinctive prefixes clarify what the hash is *of* — `queryHash`
   of a query payload, `resultHash` of a result, `subjectHash` of
   a Subject payload, `stateHash` of characterizing observations
   at a Subject.

## Appendix B: what this dictionary does not cover

- `builtins.cache` primop wiring — see `tracing-eval-cache-primop.md`.
- The design rationale (why XOR-fold, why Patricia split, why
  subject-evolution edges) — see `tracing-eval-cache.md`.
- Historical vocabulary and the transitions from it — see git
  history and the (throwaway) `tracing-eval-cache-vocabulary-cleanup.md`
  plan.
