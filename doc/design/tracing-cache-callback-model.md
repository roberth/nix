# Callback tracking model

Design of covariant callback tracking: the `SelectorCallbackApply`
Selector alternative, per-application observation accumulation,
sibling discrimination, producer Selectors and their composition.
Companion to
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
set at the moment the producer Selector is queried. Distinct
callback applications of the same `fn` with distinct contra-arg
observation patterns produce distinct Q hashes — content-addressed
identity for the application, not session-cumulative evolution.
See §7 for the per-probe sampling that defines the moment of
query.

## 4. SelectorCallbackApply as a first-class Selector alternative

A function application doesn't "end" in the sense of observing
its complete irrelevance — references to it may still exist in
unevaluated parts of its result or in function closures it
returned. But it does return a WHNF result from its body first.

So the queries observed on a callback application are:

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
privately during the callback application on the application's
own cell. The two worlds meet at the producer Selector: when a
`SelectorCallbackApply` is constructed for a probe on a
callback-produced value, it references `f`'s identity (arg-side)
as `fn` and the callback application's contra-arg observation set
(contra-arg-side) as `argObsSet`.

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

## 6a. Higher-order callback application (#217)

When outer's callback body applies a callback contra-arg to some
outer-supplied value (`g 5` inside `f = g: g 5` where `f` is outer's
callback and `(x: x+1)` was passed as contra-arg from inner), the
apply itself is a callback application scoped inside the enclosing
firing — an "inside" context per §7's terminology. Its cell is
parented to the enclosing firing's cell, its identity is a
compositional `SelectorCallbackApply`, and its argObsSet captures
the inner-lambda-body's probes on the outer-supplied value.

**Implementation locus.** `TracingCallbackArg::queryApply`
(`tracing-callback-arg.cc`) is the cold-side recording site.
Object-level dispatch avoids re-entering
`TracingCallbackArg::toValueOrProxy` through `Interpreter::apply`'s
`fn->toValueOrProxy` call — that would recurse without bound.
`TracingCallbackArg::toValueOrProxy` returns a thin primop whose
impl just wraps args[0] as an `InterpreterObject`, invokes
`self->queryApply(argObj)`, and materialises the Value via
`ExprFromObject`. The recording (layer-2 cell + callbackState +
snapshot into `ObservationSet` CAS + record the compositional SCA on
the enclosing cell) lives inside `queryApply`.

**Warm-side symmetry.** `ReplayCallbackArg::queryApply` mirrors
this: iterate `obsSetResponses` for `SelectorCallbackApply` entries
whose fn matches this proxy's producer, replay each candidate's
argObsSet probes on the live arg, and on all-match materialise a
child `ReplayCallbackArg` representing the applyResult.
Nested SCA probes recurse via `argObj->queryApply(nestedRca)`.

**Deferred cases.** Multi-arity currying (`f 1 2 3`) and
attribute-nav-then-apply on a callback-produced attrset (`t.double
f`) are not yet fully handled. The first apply routes through
`TCA::queryApply` correctly; subsequent applies route through
`<cached-fn>` primop → `TE::apply`'s non-fnIsTlo branch → plain
`SelectorApply`, which breaks warm's compositional chain. Fixing
would require wrapping `TCA::queryApply`'s applyResult so subsequent
applies also route through `queryApply`, or teaching
`makeCachedFnPrimOp` to detect callback-context and route accordingly.

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
per-application tracking because they can't vary during the
inner's lifetime.

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

