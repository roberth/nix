# Tracing eval cache — vocabulary

The dictionary of terms used by the tracing eval-cache design doc
and implementation. Each term is defined once; definitions depend
only on earlier terms.

Two **interaction models** describe how the tracing evaluator
can capture its relationship with another evaluator whose behavior
it wants to cache. Two **message pairings** — Query and Env —
realize those models in the code and storage. Callback-arg values
that cross the cache boundary from inner to outer are handled by a
first-class Query variant (`QueryCallbackApply`) rather than a
separate message pairing.

Above both models sits a property of the black-box interpreter
model itself: state creep.

---

## State creep

**State creep** is the phenomenon that in the black-box
interpreter model, any observation must be assumed to affect every
subsequent result. The evaluator's internal state is opaque; the
cache has no way to prove which subset of past observations a
given result actually depended on, so it conservatively treats
every preceding observation as a precondition of every subsequent
one.

State creep is over-approximation — the cache key widens beyond
the strict minimum — but never invalidates incorrectly. Both
interaction models are designed around it.

*In development, untested but promising* — qualifiers useful when
reasoning about state creep on the write side:

- **Locally minimal** — a fact set / state hash is *locally
  minimal* at a Query if no new facts or observations were recorded
  between the Query's structural parent and the Query itself. State
  creep contributed nothing between them.
- **Ancestrally minimal** — locally minimal along parents
  transitively. Weaker than session-minimal (see next); probably
  not what you actually want.
- **Session-minimal** — no extraneous requests recorded anywhere in
  the session up to this Query. Strongest of the three.

Ancestrally minimal is not session-minimal in the general case. A
CLI-driven evaluator can load file after file (or expression after
expression) within one session. Each file's evaluation may be
ancestrally minimal down its own tree, but only the *first* file's
tree can also be session-minimal — the second file's evaluation
starts against a session cur that already folded the first file's
facts. In `builtins.cache` we currently have one starting
expression per `cache` call and one session per `cache` call, so
ancestrally-minimal and session-minimal coincide by construction —
but that's an assumption specific to the primop, not a general
property.

---

## Interaction models

Both models describe *interaction* between two evaluators; they
differ in *how* the trace observes the other evaluator. They
correspond to the input-addressing / content-addressing distinction
on the Nix build side.

**input tracing** — analogous to input-addressing. The trace
records the *other* evaluator's environment interactions (file
reads, env vars, and anything else the other evaluator reads to
produce its outputs). By tracing what the other evaluator depends
on, the trace doesn't need to record the values passing across the
boundary between them: same environment inputs guarantee same
behavior, so a matching input trace is sufficient to reuse the
recorded outputs.

**content tracing** — analogous to content-addressing. The trace
records only the information that crosses the evaluator-evaluator
boundary — the values and probes exchanged between the two — and
stays unaware of the other evaluator's own inputs. By tracing the
content of what crosses, the trace can reuse cached results
whenever the same content crosses again, without any assumption
about the other evaluator's environment. Content crossing a
boundary reads as symmetric between the two sides; the directional
bookkeeping (which side records, which side dispatches) only shows
up in the mechanism below.

The classification is per-boundary, not per-evaluator. The same
evaluator can sit on the input-tracing side of one boundary and
the content-tracing side of another. `builtins.cache`'s inner
evaluator is content-tracing in relation to its outer; if it
invokes a further nested `builtins.cache`, it sits on the
input-tracing side of that deeper boundary.

---

## Message pairings

Three message pairings carry all the ask/answer pairs the cache
observes and replays. Each is one asker asking one askee. The
payload types vary — either the full evaluator `Query`/`Result`
surface, or a narrower per-participant protocol.

| Message pairing | Asker → Askee | Payload types | Notes |
|---|---|---|---|
| **Query** | caller → evaluator | `Query` → `Result` | Full evaluator surface. |
| **Env** | inner-evaluator → its environment | varies (see below) | Environment can be filesystem, env vars, or outer evaluator. |

Input tracing is realized by Query + Env. Content tracing applies
to the interaction with the outer evaluator during callbacks —
modelled as simple Query-level probes with no Env of its own.
These probes are embedded into the input-traced layer's Env
messages via the `OuterValueRequest` / `OuterValueResponse`
variants.

Within Env, payload types depend on the participant:

- **filesystem** — `FileReadRequest` / `FileReadResponse`. Narrow
  protocol: the filesystem only speaks "read this file."
- **env vars** — `GetEnvRequest` / `GetEnvResponse`. Narrow
  protocol: only "get value of this var."
- **outer evaluator** (via `OuterObject`) — `Query` / `Result`
  payload inside an `OuterValueRequest` / `Response`
  wrapper. Full evaluator surface; the wrapper tags the payload
  as being about an outer-owned value.

Every payload is identified by SHA-256 of its serialized bytes.
Hashes throughout the cache are always of payloads — structured
records of what was asked or answered — never of the Nix values a
payload might reference. Deep-hashing values would force
evaluation and defeat laziness; the cache observes probes
instead.

---

## Trace

A **trace** is the log of interactions across the Query and Env
message pairings during one evaluation: Query pairs at the top
(user ↔ evaluator) and Env pairs required to answer each Query
(evaluator ↔ environment). Env pairs sit under the Query pair
that provoked them.

Traces are what the cache records and replays. A recording is the
persisted form of a writer's trace up to a Result; replay builds
its own trace live from its dispatches and looks for matches
against recorded ones.

### Trace chain

The **trace chain** of one Query is that Query's own Ask-edge
sequence from the terminal factSet of the prior Query in the
trace to this Query's Terminal. Each Query in a trace produces
one trace chain.

The trace chain is what the writer's Design principle 5 flush
produces at record time: an ordered sequence of Ask edges keyed
under `(queryHash_i, cur_i)` where `queryHash_i` may evolve per
edge (Q evolution) and `cur_i` folds in one Ask's requestSet at
a time.

### Landing chain

A **landing chain** is a set of extra Ask insertions the writer
lays down that lead *to* a valid trace but aren't themselves part
of any trace. They exist so a walker starting from ∅ (or an
anchor) can reach a Query's entry point without following the
exact sequence that recorded the target trace.

Landing-chain-ness is analytical, not walker-runtime. The walker
just sees Ask rows; whether a given row is trace-chain content or
a landing-chain feeder is a design-level distinction we use to
reason about coverage. Removing all landing chains still permits
lockstep replay of any recorded trace — landing chains are what
enable *non-lockstep* replay from a walker that isn't already at
the trace's entry point (see
[`tracing-eval-cache.md`](./tracing-eval-cache.md)'s replay
strategies section).

Tributaries analogy: the trace chain is the river; landing chains
are tributaries that feed into it. Both are valid Ask nodes; only
the river is the trace.

A **structural chain** is a concrete form of landing chain whose
entrypoint is a structural parent's terminalCur. Writer-inserted
structural Asks let a trace-discovering walker at the parent Q's
Terminal reach a child Q's entry point cheaply, without following
the child's cumulative recording path from ∅. See
[`tracing-eval-cache.md`](./tracing-eval-cache.md)'s open work
section on structural-Ask insert cost for the recording-side
consequences.

---

## Session

A **session** is the lifetime of one `TracingWriter` and the
evaluator stack that shares it. The writer accumulates the trace
during that lifetime — `envFactSet`, `envFactSetHash`,
`sessionRequestsTrie`, `responseFor` are all session-scoped state.
`record()` at any queryHash reads and updates these fields; the
walker consults them for session-cumulative bookkeeping.

Not process-scoped: one CLI invocation contains multiple sessions.
The outer `EvalCommand` opens one; each `builtins.cache { ... }`
invocation opens one for its inner evaluator stack, shared across
every application of the returned `<cached-fn>` (so sibling cached
calls of the same `cached` share a session). Nested
`builtins.cache` inside a cached body opens a further session for
its own inner. Sessions are disjoint — a session's writer state
never spans another session's.

Qualifiers built on this: **session-cumulative** state is the value
of a session-scoped field at a moment in the session's lifetime.
**Within-session drift** is divergence in that state (e.g., between
walker's per-walk view and writer's cumulative view) that surfaces
inside one session's boundary. **Cross-session amortisation** is
sharing content-addressed atoms across recordings from disjoint
sessions via the DB index.

---

## Per-Q-chain

Under Q evolution (see
[`tracing-cache-callback-model.md`](./tracing-cache-callback-model.md)
§3), a Query's `queryHash` is not stable across its own
evaluation: `Query`'s payload has a `from` field carrying some
Subject's state hash, and as observations dispatched during the
Query's evaluation fold into that Subject, `from` evolves and
`queryHash` advances through a chain `Q_0 → Q_1 → … → Q_N`.
One evaluator activation of a Query — one `ActiveQuery` frame on
the writer's stack, one walk-local Q context on the walker —
tracks exactly this chain from Q_0 through Q_N.

**Per-Q-chain** state is the value of a field scoped to one such
frame's whole Q_0..Q_N chain, from when the frame is pushed
until it pops at `logResult`. Distinct from:

- **Session-scoped** (above) — spans all Queries in one
  `TracingWriter`'s lifetime.
- **Walk-local** — spans one call to the walker's `walk()`.
  A `walk()` call carries one Query's evaluation, but "walk-local"
  emphasizes the call scope, whereas "per-Q-chain" emphasizes the
  Q_0..Q_N chain that call corresponds to. On the writer they
  coincide within one `ActiveQuery` frame; on the walker
  "walk-local" is the more common phrasing because a walk may
  begin at trace-continuing state and fall through to
  trace-discovering.

Per-Q-chain scoping is what the writer's
`ActiveQuery::perQEnvWalk` and the walker's `recomputeQ`-reading
`perQEnvWalk` use for Q evolution's re-derivation — each Q's own
chain of observations, not session-cumulative and not folded
across Queries.

**Per-Q** appears in prose as a looser shorthand for the same
concept when the context makes Q-evolution unambiguous. Prefer
**per-Q-chain** where precision matters.

---

## The Query message pairing

### Query payload types

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

## The Env message pairing

### Env payload types

Three payload-type families at Env, one per environment participant.

**FileReadRequest** / **FileReadResponse** — filesystem protocol.
One question shape: "read this path." Response carries a content
hash and optionally the bytes read.

**GetEnvRequest** / **GetEnvResponse** — env-var protocol. One
question shape: "get value of this variable name."

**OuterValueRequest** / **OuterValueResponse** —
outer-evaluator protocol (via `OuterObject`; see
[Values crossing the cache boundary](#values-crossing-the-cache-boundary)).
Full evaluator surface: payload is a `Query` / `Result`, wrapped
to tag it as a query about an outer-owned value. Same evaluator
surface as the Query message pairing — introduced here because
the wrapper is what the walker records and dispatches at Env.

**Request** / **Response** — the collective terms for payloads in
any of the three families above. The walker treats them uniformly,
hashing each into a `requestHash` / `responseHash` pair (see [Edges](#edges))
regardless of participant.

**requestHash** / **responseHash** — SHA-256 of the respective
payloads.

**Fact** — one `(Request, Response)` pair. The unit of "the
environment behaved this way at this moment"; the walker records
and dispatches Facts as indivisible.

**element hash** — `SHA-256(requestHash || responseHash)`. The
per-Fact contribution to XOR-fold hashes below.

### Sets

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
Extension against a known-disjoint element is a single in-place XOR.

### Edges

**Ask** — a row in `Ask(queryHash, factSetHash) →
requestSetHash`. "At walker state `(queryHash, cur)`, the next
step is to dispatch this RequestSet's Requests."

**Terminal** — a row in `Terminal(queryHash, factSetHash) →
resultHash`. A recording that reached `(queryHash, cur)` produced
this Result. The Terminal *points at* a `resultHash`; the Result
payload itself lives in the Results pool independently. Multiple
Terminals at the same `(queryHash, cur)` are allowed — same
walker state, different Result — if recorded evaluations diverge
(nondeterminism policy is out of scope here). A Terminal ends a
walk.

**useful (dispatch)** — the subset of an Ask's RequestSet whose
Responses aren't already known at `cur`. The walker only
dispatches the useful subset; the rest is skipped as
already-known.

**hasAnyEdge** — a cheap existence check on
`(queryHash, cur)`: does any Ask or Terminal row exist at that
key? Used by the walker to reject branches that no recording ever
passed through.

### Walker state

**cur** — the walker's running factSetHash. Starts at ∅; advances
by XOR-folding each dispatched Fact's element hash. Every named
`*Cur` variable is a specific role of the same value.

**nextCur** — `cur` after XOR-folding one edge's `useful` Facts.

**startCur** — the `cur` the walk starts at. Defaults to ∅; child
queries can start at their parent's `terminalCur`.

**terminalCur** — the `cur` the walker lands at when committing a
Terminal.

**sessionCur** — the writer's `envFactSetHash` viewed as a role of
`cur`: the session-cumulative fold across all Facts the writer has
folded in this session. Corresponds on the walker side to the
running state after every dispatch the walker's session has done.
Named as a role because it's the same value as the writer's
`envFactSetHash`; distinct name marks the walker-side role.

**dispatch** — the walker's per-Request callback. Given a
Request, returns a Response by asking the live environment.

**walk(queryHash, dispatch, ..., startCur)** — the walker's
top-level entry. Returns a `WalkHit` on a Terminal reach,
`nullopt` on miss.

**WalkHit** — `{resultHash, terminalCur}`. `resultHash` is the
recorded Result the walk landed on; `terminalCur` is the `cur` at
that Terminal (usable as a child query's `startCur`).

### Recording

**record(queryHash, factSet, result, ...)** — writes an
`(Ask, ..., Terminal)` chain into the decision graph for a
completed recording.

**Patricia split** — when a new recording's remaining Requests
partially overlap an existing Ask's `useful` Requests
(∅ ⊊ shared ⊊ useful), the existing Ask is split at the
overlap. Both tail Asks reuse the original RequestSets; only
the shared-prefix RequestSet is inserted anew, and dedups
against any other recording producing the same shared set.

### RequestSet trie

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

### Storage tables (Query and Env only)

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

## Values crossing the cache boundary

`builtins.cache` nests a cached inner evaluator inside an outer
one. Values cross the cache in both directions; whichever side
owns a value, the other side is what probes it:

- **Outer-owned values** — Values the outer evaluator produced,
  passed to the inner as arguments. The inner reads them through
  `OuterObject`. Queries about these are `OuterValueRequest`s and
  belong to the Env message pairing (see [Env payload types](#env-payload-types)) — the outer
  evaluator is one of the inner's environment participants.
- **Inner-owned callback-arg values** — Values the inner
  evaluator produced, that the outer receives when it invokes an
  inner-supplied callback. The outer's callback body reads them
  through the callback-arg objects (`TracingCallbackArg` /
  `ReplayCallbackArg`). Queries about the callback firing use
  `QueryCallbackApply` (a first-class Query variant, see the
  callback-tracking model doc); the observation set the callback
  body accumulates on its contra-arg is folded into that query's
  identity.

The subject-identity machinery that ties observations to structural
identities — Subject, state hash, argAncestry, callback-arg
objects, cell navigation — is defined in the next section.

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
responses against the recording to confirm the hit.

The subject-identity machinery below characterises which
observation belongs to which value on the *arg side* — outer-
owned values the inner probes, apply-result values including a
callback firing's `fn` subject, and structural navigation
(getAttr/getListElem) between them. It fixes a **Subject** as
the value's structural name and evolves a **state hash** as
observations accumulate; `from` fields on request payloads
reference these state hashes so distinct arg-side values produce
distinct request hashes.

The other case — inner-owned callback-arg values, seen by the
outer when it runs an inner-supplied callback — is handled
differently. At replay the inner isn't running; its closures are
gone; the arg no longer exists to be probed. Rather than name
contra-arg values via subject identity, the eval-cache stores
the observations the outer made on them by value in an
`ObservationSet`, referenced from the enclosing
`QueryCallbackApply` request (see the callback-tracking model
doc). Subject / state-hash machinery below applies to arg-side
identification; it does not identify contra-arg values.

### Subject

A **Subject** is the structural name for a value in a trace — an
outer-owned value the inner probes, an apply-result value
(including a callback firing's arg-side `fn` subject), or a
derivation of either. It stays fixed while the value's content
varies across observations; a state hash tracks the
characterization built up by those observations.

Callback-arg (contra-arg) values are handled separately: instead
of being named by a Subject, the observations the outer made on
them are stored by value in an `ObservationSet` (referenced from
a `QueryCallbackApply` request; see the callback-tracking model
doc). The Subject / state-hash machinery below does not apply to
contra-arg identification.

**Observation** — a Fact viewed through the subject-identity
lens. Just `(fromHash, elementHash)`:
- `fromHash` — the state hash at the Subject when this Fact was
  emitted.
- `elementHash` — `SHA-256(requestHash || responseHash)`, same
  as the Fact's contribution to the XOR-fold.

Every Fact about an arg-side value yields one Observation per
subject that emitted it. Facts about a contra-arg value are not
projected through the subject-identity lens — they carry no
`fromHash` because the enclosing `QueryCallbackApply` request
already fixes the contra-arg position; contra-arg observations
travel by content-hashed value inside the associated
`ObservationSet`.

**ObservationSet** — a batch of Observations that share a
precondition state; the walker's fold at each step consumes one
ObservationSet at a time. XOR-folding the member `elementHash`es
yields the delta by which the FactSet's hash changes when this
set is consumed — mathematically the same operation as
`XOR-fold` in [Sets](#sets), but scoped to one step. `struct
ObservationSet { std::vector<Observation> observations; }` in
`subject-id.hh`. A **history** is a sequence of
ObservationSets.

**Subject** — a structural name for a value. Four variants:

- **Arg{depth}** — a positional name for the arg slot of a
  callback apply at reverse De Bruijn depth `depth`. Purely
  positional: no state hash evolves against it, and it is never
  the origin of a `fromHash` on a Fact — its role is composing
  `ApplyResultSubject{fn, arg=Arg{d+1}}` for a callback firing's
  return value.
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

### State hash — situational characterization

**state hash** — the situational characterization at a Subject
at a history position: SHA-256 of a serialization combining the
Subject, the enclosing argAncestry, and the observations folded
in so far. Evolves as observations accumulate; situational, not
stable.

**stateHashAt(subject, argAncestry, history, step)** — the state
hash of an arg-level subject before step `step` folds in. Traps
on `DerivedSubject` — derived values have no own observations to
fold; their key is a producer query hash, not a state hash.

**stateHashAfter(subject, argAncestry, history)** — `stateHashAt`
at `step = history.size()`.

**stateHashConverged(subject, argAncestry, observations)** — state
hash computed over an unordered observation set: same result
regardless of how observations were grouped into edges. Used by
the replay walker as a fallback when step-by-step navigation
misses.

**producerQueryHashAt(derivedSubject, argAncestry, history,
step)** — the queryHash of the `QueryGetAttr` /
`QueryGetListElem` that would produce this derived value from
its parent chain at step `step`. Not a state hash (derived
values don't have one); it's a payload hash serving as the
Queries-pool key.

**producerQueryHashAfter(derivedSubject, argAncestry, history)** —
`producerQueryHashAt` at `step = history.size()`.

**stateHashAtSubject(subject, argAncestry, history, step)** —
polymorphic dispatcher. For `DerivedSubject`, delegates to
`producerQueryHashAt`; every other variant delegates to
`stateHashAt`. Kept as a convenience for callers holding a
Subject of unknown variant.

**stateHashAfterSubject(subject, argAncestry, history)** —
`stateHashAtSubject` at `step = history.size()`.

**fromStateHashOf(query)** — reads the `from` field of a query
and returns it as a `Hash`. Every observation a subject emits
carries `stateHashAt(...)` at the emission time in this field.

### argAncestry

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

### Callback arg objects

At each cb-apply boundary the cache tracks the inner-supplied
argument through the outer's probes. Three related object types:

**TracingCallbackArg** — writer-side wrapper. Wraps the
inner-supplied value at the outer/inner interface; records the
outer's probes on it into the enclosing `CallbackCell`'s running
observation set.

**ReplayCallbackArg** — replay-side counterpart. Frozen image
reconstructed from a recorded `QueryCallbackApply`'s referenced
observation set. Serves the outer's probes from recorded data;
throws a divergence exception if the outer's probes don't match
what was recorded.

**OuterObject** — the outer evaluator's view of the callback
arg while running the callback body. Peer to `TracingCallbackArg`
(writer view) and `ReplayCallbackArg` (replay view); all three
wrap the same underlying arg from different sides.

### Cell navigation

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

### Storage tables (callback-arg observation set)

Extends the base schema
([Storage tables (Query and Env only)](#storage-tables-query-and-env-only))
with a single content-addressed pool for callback-arg observation
sets referenced by `QueryCallbackApply` requests:

```
ObservationSet(setHash BLOB PRIMARY KEY, payload BLOB)
```

The payload is a canonical serialisation of the set's members
(`(queryHash, respHash)` tuples); `setHash` is the SHA-256 of
that payload. Referenced by hash from `QueryCallbackApply`
request payloads. Same `INSERT OR IGNORE` discipline. Same
per-hash in-process caches.

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
- The design rationale (why XOR-fold, why Patricia split) — see
  `tracing-eval-cache.md`.
- Historical vocabulary and the transitions from it — see git
  history.
