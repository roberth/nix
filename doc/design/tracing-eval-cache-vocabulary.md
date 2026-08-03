# Tracing eval cache — vocabulary

The dictionary of terms used by the tracing eval-cache design doc
and implementation. Each term is defined once; definitions depend
only on earlier terms.

Two **interaction models** describe how the tracing evaluator
can capture its relationship with another evaluator whose behavior
it wants to cache. Two **message pairings** — Query and Env —
realize those models in the code and storage. Callback-arg values
that cross the cache boundary from inner to outer are handled by a
first-class Selector alternative (`SelectorCallbackApply`) rather than a
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

- **Locally minimal** — a fact set is *locally minimal* at a Query
  if no new facts were recorded between the Query's structural
  parent and the Query itself. State creep contributed nothing
  between them.
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

**Probe** — one request-response pair in any message pairing.
Used as a general term when the specific pairing doesn't matter
or when the discussion spans multiple pairings.

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

The trace chain is what the writer produces at record time: an
ordered sequence of Ask edges keyed under `(selectorHash, cur_i)`
where `selectorHash` is stable per Query and `cur_i` folds in one
Ask's requestSet at a time.

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
during that lifetime — the session-root `ArgCell`'s facts
(env-level observations), `sessionRequestsTrie`, `responseFor`,
and `seenRequests` are all session-scoped state. `record()` at
any selectorHash reads and updates these fields.

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

## The Query message pairing

### Query payload types

**Selector** — the simple query language of operations the caller
can ask: evaluate a file, evaluate an expression, get an attribute,
get a list element, get function info, apply a function, apply a
callback function, refer to a positional callback arg. Non-leaf
alternatives carry the operation's parameters plus a `parent`
reference to the parent Selector — a Merkle chain rooted at a leaf.

**Query** — the caller's ask half of the Query/Result message pair.
Its payload is a Selector; identity is the hash of that payload.

**Result** — the value the evaluator returns for a Query.

**selectorHash** — SHA-256 of a Selector payload. Also the Query's
identity — the `Selectors` pool is keyed on it, and Ask / Terminal
rows use it as one of their keys.

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

**element hash** — `SHA-256(requestHash ++ responseHash)`. The
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

**Ask** — a row in `Ask(selectorHash, factSetHash) →
requestSetHash`. "At walker state `(selectorHash, cur)`, the next
step is to dispatch this RequestSet's Requests."

**Terminal** — a row in `Terminal(selectorHash, factSetHash) →
resultHash`. A recording that reached `(selectorHash, cur)` produced
this Result. The Terminal *points at* a `resultHash`; the Result
payload itself lives in the Results pool independently. Multiple
Terminals at the same `(selectorHash, cur)` are allowed — same
walker state, different Result — if recorded evaluations diverge
(nondeterminism policy is out of scope here). A Terminal ends a
walk.

**useful (dispatch)** — the subset of an Ask's RequestSet whose
Responses aren't already known at `cur`. The walker only
dispatches the useful subset; the rest is skipped as
already-known.

**hasAnyEdge** — a cheap existence check on
`(selectorHash, cur)`: does any Ask or Terminal row exist at that
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

**sessionCur** — the session-root cell's factset viewed as a
role of `cur`: the fold across all env-scope Facts folded in
this session.

**dispatch** — the walker's per-Request callback. Given a
Request, returns a Response by asking the live environment.

**walk(selectorHash, dispatch, ..., startCur)** — the walker's
top-level entry. Returns a `WalkHit` on a Terminal reach,
`nullopt` on miss.

**WalkHit** — `{resultHash, terminalCur}`. `resultHash` is the
recorded Result the walk landed on; `terminalCur` is the `cur` at
that Terminal (usable as a child query's `startCur`).

### Recording

**record(selectorHash, factSet, result, ...)** — writes an
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
Selectors (selectorHash   BLOB PRIMARY KEY, payload BLOB)
Results (resultHash  BLOB PRIMARY KEY, payload BLOB)
RequestSetNodes(nodeHash BLOB PRIMARY KEY, payload BLOB) WITHOUT ROWID
Ask     (selectorHash BLOB, factSetHash BLOB, requestSetHash BLOB,
         PRIMARY KEY (selectorHash, factSetHash, requestSetHash)) WITHOUT ROWID
Terminal(selectorHash BLOB, factSetHash BLOB, resultHash BLOB,
         PRIMARY KEY (selectorHash, factSetHash, resultHash))     WITHOUT ROWID
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
  `ReplayCallbackArg`). Queries about the callback application
  use `SelectorCallbackApply` (a first-class Selector alternative,
  see the callback-tracking model doc); the observation set the
  callback body accumulates on its contra-arg is folded into that
  query's identity.

### Call hierarchy across the boundary

Functions are referentially transparent black boxes — we can't
inspect a function's body or clone it, so a function stays loaded
in whichever evaluator originally loaded it, and any application
of it resolves against that side. Values (both functions and
arguments) carry no persistent cross-boundary identity: when a
value from one side ends up participating in the other side's
apply, it lazily copies over and behaves as a native value there.

Same-side applies (function and argument both loaded on the same
side) happen inside that side's black box and don't concern the
cache. The applies the cache observes are the ones where function
and argument originate on opposite sides. Their **direction** is
determined by the function's home — the apply resolves where the
function lives; the argument travels there.

The applies form an alternating hierarchy by depth, with argument
ownership flipping level by level:

- **regular call** — function owned by inner, argument owned by
  outer. Outer initiates the application.
- **callback** — function owned by outer, argument owned by inner.
  Inner initiates the application, typically invoking an outer
  library function from inside its own body with inner-produced
  data. **Curried callbacks** (multiple inner arguments threaded
  through the same outer function in sequence) live at this level
  too — same directionality, different arity. Some literature
  calls curried callbacks higher-order; in this vocabulary they
  aren't, because ownership hasn't alternated.
- **higher-order callback** — function owned by inner, argument
  owned by outer, one alternation deeper than a callback. Outer
  applies an inner-loaded function it received as a contra-arg
  from a prior callback, to a fresh outer-constructed argument.
- **even higher-order callbacks** — the alternation continues at
  each further level. Rare in practice; mentioned for completeness.

Each level past a regular call is nested inside the previous level's
application; its enclosing cell (see the callback-tracking doc §6,
and §6a for the higher-order case) is what composes it with its
context.

The mechanism that ties observations to structural identities —
Selector chains, callback-arg objects, cell navigation — is
defined in the next section.

---

## Arg-side identification: Selector chains

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

**Arg-side value** — an outer-owned value the inner probes, an
apply-result of one, or a structural derivation of either.
Identified by the Selector chain that produced it. Chains
compose (each Selector embeds its parent's Q hash) so the
chain's Q hash is stable per operation and IS the value's
identity.

**Contra-arg value** — an inner-owned callback-arg value, seen
by the outer while running an inner-supplied callback.
Identified not by a Selector chain but by the observations the
outer made on it, carried by value inside the enclosing
`SelectorCallbackApply` request's `argObsSet`. See the
callback-tracking model doc.

**Bounded Q evolution.** Q hashes are stable per operation
throughout the design, with one exception:
`SelectorCallbackApply.argObsSet` embeds the running observation
set at the moment the producer Selector is queried, so distinct
callback applications of the same fn with distinct contra-arg
observation patterns produce distinct CallbackApply Q hashes.
This is content-addressed identity for the application — a
temporary identity tied to one query moment; see
[`tracing-cache-callback-model.md`](./tracing-cache-callback-model.md)
§7 — not session-cumulative evolution.

### Observation and ObservationSet

**Observation** — a scoped Fact. Facts are the pure
`(Request, Response)` pairing; Observations add the arg-side scope
that says which cell / callback firing the Fact belongs to, so the
walker can route it to the right factset.

**ObservationSet** — a batch of Observations sharing a
precondition state; the walker's fold consumes one per step.

**history** — a sequence of ObservationSets.

### Callback arg objects

At each cb-apply boundary the cache tracks the inner-supplied
argument through the outer's probes. Three related object types:

**TracingCallbackArg** — writer-side wrapper. Wraps the
inner-supplied value at the outer/inner interface; records the
outer's probes on it into the enclosing `CallbackCell`'s running
observation set. Its `queryApply` method (#217) also handles the
higher-order case — outer applying the wrapped contra-arg to some
outer-supplied value records a compositional
`SelectorCallbackApply` on the enclosing runningObsSet with the
inner-lambda-body's probes on the outer-arg captured as argObsSet.

**ReplayCallbackArg** — replay-side counterpart. Frozen image
reconstructed from a recorded `SelectorCallbackApply`'s referenced
observation set. Serves the outer's probes from recorded data;
throws a divergence exception if the outer's probes don't match
what was recorded. Its `queryApply` mirrors TCA's higher-order
recording — iterates recorded SCA entries, replays argObsSet
probes on live arg (recursively for nested SCA queries), returns
a child RCA representing the applyResult.

**OuterObject** — the outer evaluator's view of the callback
arg while running the callback body. Peer to `TracingCallbackArg`
(writer view) and `ReplayCallbackArg` (replay view); all three
wrap the same underlying arg from different sides.

### Cell navigation

**Cell** — a topology node for a callback arg, carrying a
positional depth (reverse-De-Bruijn), a parent link, and the
observations folded through this position.

**Session-root cell** — the writer's outermost cell; env-scope
facts land on it, and every other cell descends from it.

**Cell factset** — the XOR-fold of a cell's own observations
with its ancestor cells' factsets. Composed on demand.

### Storage tables (callback-arg observation set)

Extends the base schema
([Storage tables (Query and Env only)](#storage-tables-query-and-env-only))
with a single content-addressed pool for callback-arg observation
sets referenced by `SelectorCallbackApply` requests:

```
ObservationSet(setHash BLOB PRIMARY KEY, payload BLOB)
```

The payload is a canonical serialisation of the set's members
(`(selectorHash, respHash)` tuples); `setHash` is the SHA-256 of
that payload. Referenced by hash from `SelectorCallbackApply`
request payloads. Same `INSERT OR IGNORE` discipline. Same
per-hash in-process caches.

---

## Appendix A: naming rules

Rules the vocabulary above obeys:

1. **Stable vs situational is carried by the type name, not by a
   suffix.** `Selector` and `selectorHash` are stable by
   construction — an immutable algebraic value and its hash.
   `factSetHash` is situational — its value tracks folded
   observations. No `Id` marker is required or used.

2. **`Hash` is neutral.** It says only "the value is a `Hash`."
   Distinctive prefixes clarify what the hash is *of* — `selectorHash`
   of a query payload, `resultHash` of a result, `factSetHash` of
   a set of Facts.

3. **"outer" / "inner" (unqualified) refers to the primop cache
   boundary**, not to the evaluator-wrapper stack. The primop
   relation lives in the value heap: outer is the caller that
   invoked `builtins.cache { ... }`, inner is the boundary being
   cached, and values cross between them by heap reference. The
   evaluator-wrapper stack (TracingReplayEvaluator wraps
   TracingEvaluator wraps Interpreter) is a *different* relation —
   layers of interception around one Evaluator instance. When
   discussing the wrapper stack, qualify: "the wrapped Interpreter",
   "the fallback evaluator", "the recording layer" — never bare
   "inner" or "outer".

## Appendix B: what this dictionary does not cover

- `builtins.cache` primop wiring — see `tracing-eval-cache-primop.md`.
- The design rationale (why XOR-fold, why Patricia split) — see
  `tracing-eval-cache.md`.
- Historical vocabulary and the transitions from it — see git
  history.
