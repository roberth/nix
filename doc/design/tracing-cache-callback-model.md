# Callback tracking model

Design of covariant callback tracking: the `SelectorCallbackApply`
Selector alternative, per-firing observation accumulation, sibling
discrimination, sampling per WHNF-producing probe. Companion to
[`tracing-eval-cache.md`](./tracing-eval-cache.md) (base cache
model),
[`tracing-eval-cache-vocabulary.md`](./tracing-eval-cache-vocabulary.md)
(term glossary), and
[`tracing-eval-cache-primop.md`](./tracing-eval-cache-primop.md)
(builtins.cache primop).

Historical note: the doc was written in the Q-evolution era
(task #110) with a Model / Code / Gap scaffold that flagged
transitional machinery. Q evolution and the subject-identity
machinery retired under #183; the doc has since been trimmed to
the callback-tracking model itself. Sections that documented
retired mechanisms (Q evolution, argAncestry composition,
SuppressApplyBoundary) are gone; §11 preserves the "removed
machinery" list for old-comment archaeology.

## 1. What the eval cache does

Cache the output of the interpreter under the assumption that its
internal state is opaque (state creep — any past observation may
affect any future result). The cache records the observations an
evaluation actually made and, on warm replay, validates each
observation against the current environment. On the first divergent
response, replay stops and yields no answer for the divergent case.

Two evaluators are involved when `builtins.cache` is on the table:

- **Outer** — the evaluator that called `builtins.cache`.
- **Inner** — the evaluator inside the cached function body (an
  isolated Nix expression per `import`).

User rule (2026-07-21): "outer" refers strictly to the outer
evaluator, never to "the caller of a callback inside the inner."

## 2. Primitives

Vocabulary specific to callback tracking (base definitions in
[`tracing-eval-cache-vocabulary.md`](./tracing-eval-cache-vocabulary.md)):

- **Selector** — one alternative of the `SelectorVariant` eDSL
  plus its serialised payload; content-hash is `selectorHash`
  (a.k.a. Q hash). Callback-related alternatives:
  `SelectorApply`, `SelectorCallbackApply`.
- **Fact** — one `(Request, Response)` pair.
- **Observation** — a Fact carrying attribution: the element hash
  plus the Q hash of the value the request was dispatched against.
- **cur** — a running XOR fold of Facts scoped to a cell's factset.
- **Ask edge** — `(selectorHash, cur) → requestSetHash`.
- **Terminal edge** — `(selectorHash, cur) → resultHash`. End of
  a chain.
- **Cell** — a topology node for a callback arg carrying
  positional depth, parent link, and folded observations.
- **contra-arg** — an inner-owned callback-arg value, seen by the
  outer while running an inner-supplied callback.

## 3. Q hash stability, and the one bounded exception

Selector Q hashes are stable per operation: a Selector's content
hash is a pure function of its payload, and the payload is fixed
at construction. No per-observation re-derivation, no `from`-field
rewriting after emission. (Historical: an earlier design evolved
Q hashes per observation; that mechanism retired under #183.
Task #178 completes the retirement by removing the stringly-typed
`from` field from Selector payloads once cell-based discrimination
is fully proven.)

The one bounded exception is **SelectorCallbackApply**. Its
`argObsSet` field is a CAS reference to the running observation
set at the moment of QCA emission. Distinct firings of the same
`fn` with distinct contra-arg observation patterns produce
distinct QCA Q hashes — content-addressed identity for the
firing, not session-cumulative evolution. See §7 for the
per-WHNF-probe sampling that defines "the moment of emission".

## 4. SelectorCallbackApply as a first-class Selector alternative

A function application doesn't "end" in the sense of observing
its complete irrelevance — references to it may still exist in
unevaluated parts of its result or in function closures it
returned. But it does return a WHNF result from its body first.

So the queries observed on a callback firing are:

1. The initial call: `SelectorCallbackApply(f, obs) → WHNF`.
2. Subsequent probes: `<q>(SelectorCallbackApply(f, obs')) → <r>`,
   where `obs' = obs` or some larger set (§7 explains "growing").

Sibling discrimination is trivial by construction: sibling A's
callback observes `getAttr("a")`, sibling B's observes
`getAttr("b")`, so `obs_a ≠ obs_b`, so
`SelectorCallbackApply(f, obs_a) ≠ SelectorCallbackApply(f, obs_b)`,
so distinct DB rows.

**Payload shape.**

```
SelectorCallbackApply {
  fn:          <Q hash of f>
  argObsSet:   <CAS content-hash of the sampled obs set>
}
```

`obs` is a CAS reference into the `ObservationSet` table, not
inlined — to keep large callback results efficient.

Result: `ResultWHNF`. Preferring a WHNF result reduces entropy in
the trace — otherwise a `SelectorCallbackApply(...)` would in
practice always be immediately followed by a getter probe on its
result.

Walker dispatch materialises a `ReplayCallbackArg` backed by the
referenced ObservationSet, invokes `fn->queryApply(replayArg)`
live, and returns the resulting WHNF.

## 5. `f` is arg-side, obs is contra-arg-side

A callback function is only given to us through an arg. All
observations on an arg are attributed to that arg's cell. `f`'s
identity flows from its arg-cell's factset like any other
arg-side value's does.

The **contra-arg** (the arg passed *to* the callback) is a
separate world: callback tracking doesn't inherently need
arg-side identity tracking. Contra-arg observations accumulate
PRIVATELY during the callback firing. No visibility into
arg-side tracking during that time. The two worlds meet only at
the sampling moment where `SelectorCallbackApply(f, obs)` is
emitted as an observation on the arg — the obs set folded into
`f`'s arg-cell's factset as one Fact.

The handoff seam is the writer's QCA-emission path
(`emitCallbackApplyForApplyResult`): it reaches into the enclosing
callback cell's `runningObsSet`, snapshots it into the CAS, and
emits QCA via `logOuterObservation` attributing to `f`. The seam
is load-bearing but the design didn't pin down where it should
best live in code.

## 6. Callback cell — per-application accumulator

A **callback application** is one invocation of `OuterApply::run`:
inner has issued an apply against an outer-side callable (an
`OuterObject` wrapping an outer function, or a `ReplayCallbackArg`
standing in for one at replay), and the outer-side handling of
that apply is a callback application. The callable's body
evaluates in the outer evaluator during the application; contra-arg
observations the body makes on the argument accumulate on the
application's cell. Older comments call this event a "callback
firing"; same thing.

Each callback application has one **cell** (an `ArgCell`) — the
per-application shared state. Every proxy participating in the
application (the callable's `OuterObject` / `ReplayCallbackArg`,
the contra-arg's `TracingCallbackArg`, the applyResult's
`TracingObject` marked `cbApplyOrigin`, and any nav descendants
of these) holds a reference to the same cell via its `argCell`
field. Observations one proxy records land where the others can
read them, without a writer-global lookup table. Cells are
**never closed** — references to the applyResult can persist in
unevaluated parts of downstream results, so more observations may
arrive at any time.

The cell holds two pieces of state relevant to producer-Selector
construction:

- **The callable's producer Selector** — captured at the moment
  the application began. Emitted as the `fn` field of this
  application's producer Selector (§7). Typically a
  `SelectorGetAttr` / `SelectorGetListElem` / `SelectorArg`
  navigation from the outer arg; may itself be a
  `SelectorCallbackApply` carrying its own obsSet snapshot when
  the callable came from a prior callback application, in which
  case that snapshot moment is captured here.
- **The running observation set** — contra-arg observations
  accumulated so far. Snapshotted into the ObservationSet CAS
  when a producer Selector is queried (§7).

Curried callback applications split their obsSets across the chain.
Arity-2 `cb a b` yields two `SelectorCallbackApply` layers, each
carrying the observations made during its own application — one
for `cb a`, another for `(cb a) b`. Mixed sequences work the same
way: a "library with context" pattern (outer supplies a callable
that returns an attrset of functions; the outer later retrieves
one function and applies it) produces a `SelectorCallbackApply →
SelectorGetAttr → SelectorCallbackApply` chain in the final
producer Selector. Each `SelectorCallbackApply` layer carries the
obsSet from its own application; intervening navigation Selectors
carry no obsSet.

## 6a. Probe

**Model** (user, 2026-07-22 — not yet in vocab).

A **Probe** is a single Value-level query issued by a consumer:
`whnf`, `maybeGetAttr("x")`, `getListElem(i)`, etc. Each probe
touches one Value at a time. `builtins.cache` is inherently
one-probe-at-a-time (it only ever accesses one Value on each side
of the boundary), so it can never generate a "sudden and deep"
query that reaches multiple layers in one shot. The CLI is
different — CLI callers (historically `AttrCursor`, now `Object`)
can reach deep in a single call, and `TracingObject` preserved
that capability.

Add "Probe" to the vocabulary. Doc-improvement, not a model
question.

## 7. Producer Selectors — how callback-produced values are identified

A callback-produced value's identity is its **producer Selector**
— a `SelectorCallbackApply` snapshotting the enclosing callback
cell's `runningObsSet` at the moment the identity is queried.
Distinct probes at distinct moments produce distinct producer
Selectors via distinct obsSet snapshots.

Composition is by nesting:

- **Getters compose via `from`.** A probe like `.whatever` on a
  callback-produced attrset yields
  `SelectorGetAttr{name="whatever", from=SelectorCallbackApply{...}}`.
  The getAttr is the outer Selector; the callback-produced
  parent's producer Selector is embedded as `from`. Its obsSet
  snapshots the runningObsSet at the moment `.whatever` is probed.

- **Curried callback applications compose via `fn`.** When a
  callback's applyResult is itself a function that the outer
  applies to a further arg, the further application yields
  `SelectorCallbackApply{fn=SelectorCallbackApply{...}, obs=obs_b}`
  — the outer `fn` field references the previous application's
  producer Selector.

- **Combined.** A getAttr on the applyResult of a curried callback
  application:
  `SelectorGetAttr{name, from=SelectorCallbackApply{fn=SelectorCallbackApply{...}, obs=...}}`.

The producer Selector's payload lives in the Requests pool (so
`resolveIdentity` can decode `from` / `fn` references at replay).
The producer Selector is not itself a Fact folded into any cell's
chain — the getAttr / apply Selector that references it is what
becomes the Fact.

Idempotency: same runningObsSet content → same `argObsSet` hash →
same producer Selector identity → same DB row across probes that
sample equivalent snapshots.

Empty `runningObsSet` is a legitimate snapshot. A fully-lazy
callback application whose applyResult is used before any
contra-arg observations have fired has `obs = ∅` at that point;
the resulting producer Selector still needs to be in the Requests
pool so downstream getAttr / apply Selectors that reference it
can resolve their `from` / `fn`. Empty obs is not a reason to
suppress payload insertion.

Curried applications are the canonical example. Before a curried
callback application `f a b` can happen, the caller has to demand
the partial application `f a` — an ordinary WHNF probe whose
response is the `Function` type tag. (Functions are characterized
through observations made on them — `getFunctionInfo`, subsequent
applications — not represented in the trace; only FunctionInfo
reflection metadata appears when queried.) That probe's producer
Selector is `CallbackApply(f, obs=<snapshot at that moment>)`;
the response is `Function`. Later, applying the returned function
to `b` produces a further producer Selector
`CallbackApply(fn=<the previous producer>, obs=<later snapshot>)`
identifying the nested application; its response is whatever that
application evaluates to.

The enclosing cell's factset accumulates one Fact per probe the
caller drives, in caller-driven order — consistent with principle
7 (laziness end-to-end): the writer records what the caller
actually probed, never probes ahead. `f a`'s WHNF probe is one
Fact on the enclosing chain; a later `(f a) b` probe is another.
Neither Fact is manufactured by the cache — each corresponds to a
probe the ultimate caller issued.

## 7a. Sibling cb-apply discrimination

The canonical mechanism is **obsSet in QCA selectorHash**.
Sibling A's callback observes different contra-arg attributes
than sibling B's, so obs sets differ, so QCA queryHashes differ,
so distinct DB rows. Discrimination is at the query-identity
level. No cur-trajectory divergence required.

Other mechanisms mentioned in older sources — cur-trajectory
divergence, SelectorApply slot differences — were pre-#110
workarounds for the fixed-Q collision problem. Under
obsSet-in-QCA they're redundant. Only obsSet in QCA is
load-bearing today.

## 8. Matching-until-divergence

Two evaluation events whose observations match up to some point
are characterized identically at that point — same Q hashes at
every referenced value, same requestHashes on any observation
emitted there. They are characterized distinctly only once their
observations diverge.

Consequences:

- Cold and warm see identical observations at corresponding
  moments up to divergence.
- At any Ask edge, if warm's live dispatch produces the same
  response as cold's recorded response, walker's cur advances the
  same way.
- The first divergent response ends warm's applicability. No
  silent wrong hit — the divergent elementHash lands warm at a
  cur cold has no row for; walker misses cleanly.

Guaranteed by construction, not by explicit checks. Cold's
recording is deterministic given its observation sequence;
warm's per-walk observation sequence matches cold's up to
divergence; therefore hashes match up to divergence.

## 9. Sub-Q composition

A sub-Q is a Q whose evaluation is required to answer another Q
— a semantic relationship (nesting of Nix expressions), not an
operational stack construct. Under the current model, sub-Q
composition rides on the cell chain: a child Q's evaluation runs
against a child cell whose parent link resolves to the parent
Q's cell, so the parent's factset composition (`factSetHash()`
walks the parent chain) folds in the sub-Q's completion
implicitly.

The design intent of a **composite observation** at parent's
chain — one Fact per sub-Q completion, request = sub-Q's
selectorHash, response = sub-Q's resultHash — remains a
recording-shape option worth revisiting. Status: partial; see
`doc/status.md` (B2) for the current state.

## 10. "Ambient" — retired, term available for repurposing

The Ambient message pairing dissolves entirely: there are two
message pairings, Query and Env. QCA is a Selector alternative;
contra-arg observations are Facts on Env; the cell mechanism
(§5-6) is implementation detail, not a distinct message-pairing
layer.

**Term reservation.** "Ambient" is a good word for an
inner-global value — an outer argument bound immediately at the
`builtins.cache` call (as opposed to at a callback application)
and 1:1 with the inner evaluator. Such values don't need any
per-firing tracking because they can't vary during the inner's
lifetime.

Reserving the term only. Not defining the concept formally,
since there's no implementation or design driver for it yet.
Naming without a use case risks the term drifting; the note
here is to prevent the freed word from being reused for
something unrelated in the meantime.

## 11. Removed machinery (for reading old comments)

The design got here by explicitly deleting several prior
mechanisms. Terms to be suspicious of in old comments / commit
messages / stale doc sections:

- **AmbientAsks table + `advanceChainAndAppendFact` +
  `withAmbientAsksValidation`** — replaced by `runningObsSet`
  accumulation. Task #109.
- **`InnerValueResponse` table + `contextHash`** — replaced by
  obsSet CAS.
- **`localArg sidecar`** — replaced by QCA's explicit
  `argAncestry`/`argDepth` payload fields (both dropped from
  QCA payload under #183).
- **`SubjectEvolutionEdge` table** — replaced by local
  `obs.fromHash == cur` filter.
- **`params.callbackApply` slot on outer probes** (task #103's
  MVP) — replaced by first-class `SelectorCallbackApply` (task #110).
- **`OuterApply::run`'s emission of an outer-scope observation +
  applyContext bridge push** (task #108) — replaced by QCA
  compositional Q evolution.
- **`contextHash` / `boundaryContext` / `outerContext` on
  `ReplayCallbackArg`** — obsolete under obsSet CAS.
- **`chainCursor` + `<replay-local-lambda>` primop's XOR-advance**
  — dead once `dispatchApplyLive` was ripped.
- **Subject, subjectId, state hash, argAncestry, and Q evolution**
  — the arg-side subject-identity machinery + per-observation Q
  hash re-derivation retired under #183. Identity is now the
  content hash of the Selector chain that produced the value,
  stable per operation.
- **`SuppressApplyBoundary`** — the guard that prevented
  walker-triggered phantom callback cells retired under #184.
- **`ApplyContext`** — a shared observation-accumulator struct
  once threaded through TracingObject/TracingReplayObject via
  `applyContext` fields, retired as pure write-only bookkeeping.
- **The word "boundary"** — legacy. If a comment or symbol name
  uses it, be suspicious.

## 12. Retained naming quirks

- **`dispatchQueryRequest`** (walker method name) — despite the
  name, all its branches dispatch first-class Selector
  alternatives. Rename pending.

## 13. Nested callback composition (curried/higher-order case)

`TracingCallbackApplyResult` — WRONG in the curried/nested case.

Scope of this bug: **when a callback-originated value is itself
applied later** (curried application, or a returned closure being
applied). NOT the ordinary "contra-arg observations flow into
cell.runningObsSet, then out via emitCallbackApplyForApplyResult
as a QCA on the arg" routing (§4.2/§5/§6) — that channel is
correct and load-bearing.

Example (user): `cb = k: v: k == v`; `c1 = cb "a"`; `c2 = c1 "b"`;
`c3 = c1 "a"`. Should the observations from `c2` and `c3`'s
applications route BACK into `c1`'s cell? Certainly not — it would
be ambiguous / contradictory (two different applications of `c1`
writing to the same cell).

**Correct design.** Each application (including of returned
closures) has its OWN cell to track its own observational state.
The query shape falls out naturally as nested QCAs:

```
QCA( QCA(cb, obs_a) , obs_b )
```

`c1` is `QCA(cb, obs_a)`, then applying `c1` to `"b"` produces
`QCA(QCA(cb, obs_a), obs_b)`. No cross-cell routing needed. The
obsSet content-hash goes directly in each QCA.

The current code "has something quirky that it copied from state
hashes, but there's no need for that. Just put the observation
set hash in the QCA" (user).

**B8** — `TracingCallbackApplyResult`'s cross-application routing
into the enclosing cell is wrong; refactor so nested applications
compose via nested QCA structure with per-application cells.

**Walker-side dispatch of nested QCA** (user, 2026-07-22 —
"Recursive Q resolution"). To dispatch `QCA(QCA(cb, obs_a),
obs_b)`, the walker recursively invokes `lookup` on the inner
`QCA(cb, obs_a)` to resolve `f`'s current identity, materialises
the outer obsSet as a `ReplayCallbackArg`, then invokes the
resolved callable live. Symmetric to how flat QCA dispatches
today, extended to nested case. Requires walker to recognise
QCA-in-fn-slot vs a leaf Q hash.

Historical note: earlier framings of this design placed the
"nested QCAs" style alongside a separate "state hash evolution"
style for arg-side identity, suggesting two composition idioms
coexisting. Under #183 the arg-side state-hash mechanism
retired in favour of Selector-chain identity, which is
inductive-style already (each Selector composes by embedding
its parent's Q hash). Both callback and arg-side composition
are now inductive.
