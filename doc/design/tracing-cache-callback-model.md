# Callback tracking model — QueryCallbackApply and Q evolution

2026-07-22. Task #110 model as of commit `9d6da5a58` (branch
`eval-cache-v13-primop`).

Each section separates **Model** (design intent, cited to user
prompts or older docs that are still valid), **Code** (what the
implementation actually does, cited by file:line), and **Gap**
(where they differ). When a claim appears in the pre-existing
`tracing-eval-cache-*.md` docs but is stale under task #110, the
gap section notes it.

**This doc's positioning** (user, 2026-07-22): the existing
`tracing-eval-cache-subject-identity.md` still provides a
foundation; surgical removal of outdated parts is the target, not
wholesale replacement. This callback-model draft is transitional
— its content should feed into targeted edits to subj (and to
vocab / main where they overlap) rather than persist as a
permanent parallel doc.

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

**Model.** Common vocabulary. Definitions from
`tracing-eval-cache-vocabulary.md` still hold except where task #110
revises them.

- **Query (Q)** — a `QueryVariant` value plus its serialised
  payload; content-hash is `queryHash`. Q types include
  `QueryGetWHNF`, `QueryGetAttr`, `QueryGetListElem`,
  `QueryImport`, `QueryCallbackApply`.
- **Result** — the value the evaluator returns for a Query.
  Content-hash `resultHash`.
- **Fact** — one `(Request, Response)` pair. Element-hash is
  `SHA-256(requestHash || responseHash)` — the fold contribution.
- **Observation** — a Fact viewed through subject-identity: a
  `(fromHash, elementHash)` pair where `fromHash` is the referenced
  Subject's state hash at the moment of the Fact. Same physical
  event as a Fact, different projection.
- **cur (factSet hash)** — a running XOR fold of observations. The
  scope of "running" is where the model has been revised (see §3).
- **Subject** — a value's structural name (`Arg{depth}`,
  `DerivedSubject{parent, kind, name/index}`,
  `ApplyResultSubject{fn, arg}`, `PostulatedIdempotentRead{hash}`).
  Immutable — same shape → same Subject.
- **State hash** — SHA-256 of a serialization combining Subject,
  argAncestry, and the observations folded so far. Situational, not
  stable.
- **Ask edge** — `(queryHash, cur) → requestSetHash`. Written by
  the writer at cold, consumed by the walker at warm.
- **Terminal edge** — `(queryHash, cur) → resultHash`. End of a
  chain.
- **argAncestry** — an XOR-fold over enclosing callback args'
  state hashes at the moment the innermost callback was entered
  (vocab §argAncestry). Non-lexical: only callback arguments
  contribute, not `let` bindings. Composition details in §9.

**Code.**

- Query types in `trace-types.hh`. `QueryCallbackApply` declared
  there with `DECLARE_QUERY_RESULT(QueryCallbackApply, ResultWHNF)`.
- Fact = `(Request, Response)` in `tracing-decision-graph.hh`.
- Observation = `struct Observation { Hash fromHash; Hash
  elementHash; }` in `tracing-decision-graph.hh` line 132.
- Subject variants in `subject-id.hh`.
- Ask + Terminal tables in `tracing-decision-graph.cc` (SQL schema).

**Gap.**

- Vocab defines Ask/Terminal with `queryHash` as a stable key
  throughout a walk. Task #110 makes `queryHash` evolve per-edge.
  What the row keys concretely represent is now "queryHash at the
  edge's moment" — see §3. Question Q7 in the questions file.
- Vocab's `cur` definition is scope-ambiguous between session and
  per-Q. Task #110 introduces per-Q scoping explicitly. Question Q8.

## 3. Q evolution

**Model** (user, 2026-07-21):

> The next Ask is at (Q_after, curAfter). Also a Terminal would be
> at (Q_after, curAfter).

Q's payload carries a `from` field: a state hash of some referenced
subject. As observations dispatched during Q's walk fold into that
subject's state, the subject's state hash evolves → Q's `from`
evolves → Q's queryHash advances. A chain is
`Ask(Q_0, cur_0) → Ask(Q_1, cur_1) → … → Terminal(Q_N, cur_N)`
where both Q and cur evolve in lockstep.

Not every Q evolves. A Q whose `from`-subject doesn't participate
in observations dispatched during its walk keeps a constant `from`;
its queryHash stays fixed. That's a special case, not a separate
code path.

Recording is single-trace and in-place (user, 2026-07-21):

> For writing, I think tracking state evolution in-place could work.
> Reason: writing with smaller hashes tends to be a misrepresentation
> of the preconditions of the result Terminal. Larger or different
> doesn't really make sense either.

Replay is 0-many traces, per-candidate:

> The ability to *accept* smaller preconditions lives on the
> *walker* side, because it deals with not one but 0-many traces,
> each having their own sets of preconditions.

And the ordering constraint:

> Before accepting the new Q, flush into an Ask so that the Request
> that gives rise to the state hash change can also be dispatched
> by the walker.

**Code — writer** (`tracing-writer.cc:145-214`).

For each observation via `logOuterObservation`:

1. **Insert Ask at (Q_before, cur_before)** at `line 162-165`:
   ```
   if (!activeQueryStack.empty()) {
       auto & innermost = activeQueryStack.back();
       decisionGraph->insertAsk(innermost.currentQ,
                                prevQFactSetHash, requestSetHash);
   }
   ```
   The Ask edge uses `currentQ` — which is Q_before-fold — as key.
2. **Fold** at `line 166-174`: append to session `envWalk`; append
   to innermost active Q's `perQEnvWalk`.
3. **Re-derive** at `line 188-213`:
   ```
   auto newState = stateHashAt(
       *aq.fromSubject, aq.fromSubjectArgAncestry,
       aq.perQEnvWalk, aq.perQEnvWalk.size());
   if (newState != aq.fromSubjectLastState) {
       // rewrite payload's `from` field, re-hash → new currentQ
   }
   ```
   The re-derivation uses per-Q `perQEnvWalk`, not session envWalk.

At `logResult` (`tracing-writer.hh:713-777`): insert
`Terminal(finalQ, finalCur, resultHash)` and pop the ActiveQuery
frame.

**Code — walker** (`tracing-decision-graph.cc` +
`tracing-replay-evaluator.cc`).

`TracingDecisionGraph::walk` accepts an optional `recomputeQ`
callback. `TracingReplayEvaluator::walk` builds one at
`tracing-replay-evaluator.cc:281-298`:

```
recomputeQ = [payloadTemplate, fromSubject,
              fromSubjectArgAncestry, perQEnvWalk](const Hash & preFoldQ) {
    auto newState = stateHashAt(
        *fromSubject, fromSubjectArgAncestry,
        *perQEnvWalk, perQEnvWalk->size());
    // rewrite payload's `from`, re-hash → new Q
};
```

`perQEnvWalk` is a walk-local `shared_ptr<vector<ObservationSet>>`
(line 64), populated by `commitEdge` alongside session `envWalk`
(line 107-109).

**Gap 1** (task #110 vs old vocab). Vocab treats `queryHash` as a
fixed row key; the code re-derives it per-edge. The row schema
still uses `queryHash` as a column name, but the value at
insertion time is Q's currentQ at that moment. Doc update needed;
schema unchanged.

**Gap 2** (fast-path residue in perQEnvWalk). Fast-path miss
rolls back session `envWalk`/`envCur`/`committedEdgeFingerprints`
(lines 335-342). But `perQEnvWalk` is declared once at function
scope and shared between fast and slow paths — failed fast-path
commits leave residue in `perQEnvWalk`, and slow path's Q evolution
folds them in. This is bug **B5** in the status file. Question Q27.

**Gap 3** (file/env reads and Q evolution). `closeAsksEdge`'s
finalize (`tracing-writer.cc:246-266`) inserts an Ask under
innermost Q but pushes an EMPTY `ObservationSet` to `envWalk` and
does NOT push to `perQEnvWalk`. Doc-side: correct — file/env reads
carry no `fromHash`, so they can't fold into any subject's state.
Skipping the recompute is right, not a gap. Called out to head off
"is this a Q-evolution miss?" confusion.

## 4. QueryCallbackApply as a first-class Query variant

**Model** (task #110 verbatim, user 2026-07-21):

> I have had to remind previous sessions that "a function
> application does not end" in the sense of observing its complete
> irrelevance, because references to it may still exist in
> unevaluated parts of its result or in function closures it
> returned. But that doesn't mean it doesn't return a WHNF result
> from its body first.
>
> So the queries we observe are basically:
> 1. The initial call, QueryCallbackApply(f, obs) -> WHNF
> 2. Subsequent observations, <q>(QueryCallbackApply(f, obs')) -> <r>,
>    where obs' = obs or some larger set

And (2026-07-21):

> All I said just now is about *callbacks* and their *contra-arg*.
> A *call* and its *arg* is still subject to the more complex state
> hash tracking ("from"/"fromStateHashes").
> - A callback function is only given to us through an *arg*.
> - All observations on an *arg* are subject to state hash tracking.
> - A QueryCallbackApply observation is a regular observation, just
>   like getting the WHNF of an attribute, etc.
> - Conclusion: callback tracking does *not inherently need* state
>   hash tracking, but *ends up* as part of a state hash *outside*
>   of its area of responsibility.

Sibling discrimination is trivial by construction: sibling A's
callback observes `getAttr("a")`, sibling B's observes
`getAttr("b")`, so `obs_a ≠ obs_b`, so
`QueryCallbackApply(f, obs_a) ≠ QueryCallbackApply(f, obs_b)`,
so distinct DB rows.

**Model — payload shape** (user, Q2 answer 2026-07-21: "choose (a)").

```
QueryCallbackApply {
  fn:          <state hash of f, at f's evolved state when sampled>
  argObsSet:   <CAS content-hash of the sampled obs set>
  argAncestry: <arg's argAncestry hash>
  argDepth:    <arg's depth in nested cache calls>
}
```

`obs` is a CAS reference into the `ObservationSet` table, not
inlined — "to keep large callback results efficient" (user).

Result: `ResultWHNF` (user, 2026-07-22: "prefer a WHNF result,
because that reduces entropy in the trace. Otherwise the
(QueryCallbackApply ...) would have to be immediately followed by
QueryGetWHNF(QueryCallbackApply ...) in practice").

**Code.**

`trace-types.hh`: `QueryCallbackApply` variant with
`DECLARE_QUERY_RESULT(QueryCallbackApply, ResultWHNF)` (was
`ResultType` pre-C3, commit `5f763022d`).

Writer emission: `TracingWriter::emitCallbackApplyForApplyResult`
(`tracing-writer.hh:277-311`). Called from `TracingObject::whnf()`
(`tracing-object.cc:167`) after `computeWHNFFromObject` returns.

Walker dispatch: `TracingReplayEvaluator::dispatchAmbientQuery`
has a `tag == "callbackApply"` branch that extracts
fn/argObsSet/argAncestry/argDepth, materialises a
`ReplayCallbackArg` backed by the referenced ObservationSet,
invokes `fnObj->queryApply(replayArg)` live, returns `ResultWHNF`.

**Gap** (branch name legacy). The walker's method is
`dispatchAmbientQuery` — "Ambient" is legacy nomenclature (task
#109 removed the Ambient message pairing but kept the method name).
Callers are all task-#110 QCA dispatches. Question Q9.

## 5. `f` is arg-side, obs is contra-arg-side

**Model** (user, 2026-07-21).

A callback function is only given to us through an arg. All
observations on an arg are subject to state hash tracking. `f`'s
state hash evolves as any other arg-side value's does.

The **contra-arg** (the arg passed *to* the callback) is a
separate world:

> Callback tracking does not inherently need state hash tracking,
> but ends up as part of a state hash outside of its area of
> responsibility.

Contra-arg observations accumulate PRIVATELY in a cell during the
callback firing. No visibility into arg-side tracking during that
time. The two worlds meet only at the sampling moment where
`QueryCallbackApply(f, obs)` is emitted as an observation on the
arg.

The handoff is meta-level (user, 2026-07-21: "there's some
abstraction involved in that handoff, but I don't know off the top
of my head where that indirection lands in terms of code").

**Code.**

Handoff seam: `TracingWriter::emitCallbackApplyForApplyResult`
(`tracing-writer.hh:277-311`) reaches into the cell's
`runningObsSet`, snapshots into the CAS, emits QCA via
`logOuterObservation` with `*ar->fn` (arg-side subject for `f`) as
the attribution subject. Question Q25 flags this seam as
"reasonable but genuinely open" — the model didn't pin down
where the code seam should live.

## 6. CallbackCell — writer-side firing accumulator

**Model.** For each in-flight callback firing (an application of
an inner-side function whose body is being evaluated), the writer
keeps state that lets it (a) route contra-arg observations to the
right accumulator, (b) snapshot the accumulator into a QCA payload
at sampling moments. Cells are **never closed** — references to
the applyResult can persist in unevaluated parts of downstream
results, so more observations may arrive at any time.

**Code** (`tracing-writer.hh` around `CallbackCell` struct):

```
CallbackCell {
  applyId          // unique id for this firing
  fnStateHashHex   // f's initial state hash (cell lookup key)
  argAncestryHex   // contra-arg's argAncestry
  argDepth         // contra-arg's depth
  runningObsSet    // observations made on the contra-arg so far
}
```

Populated by `logCallbackObservation` (writer method,
`tracing-writer.hh:498`) as `TracingCallbackArg` methods are
invoked. `emitCallbackApplyForApplyResult` looks up cells by
`cell.fnStateHashHex == fnInitialHex` (loop at
`tracing-writer.hh:289-292`), takes the most recent match (reverse
iteration), snapshots `runningObsSet` into the CAS, emits QCA.

**Gap.**

The cell struct went through significant slimming in commit
`52840203e` (13 fields → 6). Prior fields — `applyRequestHash`,
`insertionIndex`, `envCurAtOpen`, `outerEnvCurAtOpen`, `contextCur`,
`facts`, `finalized`, `cumulativeFactSet`, `factHash`, `pos`,
`lastProcessedCount`, `runningObsHistory` — all supported the
"boundary lifecycle" framing that's now retired. Old doc
references to `contextCur`, `facts`, `finalized` are stale.

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

## 7. Sampling — when QueryCallbackApply gets emitted

**Model** (user, 2026-07-22 refinement).

**Per WHNF-producing probe on a callback-originated value.** Each
probe that produces a WHNF emits its own QCA. Not "one per
firing" — "firing" is under-defined. Concretely: if the callback
returns an attrset, then:

- The probe that computes the applyResult's WHNF emits QCA-1 for
  that WHNF.
- A subsequent probe that computes an attribute's WHNF emits
  QCA-2 for the attribute's WHNF.

Each QCA carries the runningObsSet as observed at THAT probe's
sample moment. Between samples, the cell's runningObsSet may have
grown as more contra-arg observations arrived.

Idempotency: same runningObsSet content → same `argObsSet` hash →
same queryHash → same DB row.

**"Firing" is the wrong word** (user, 2026-07-22). The scoping
unit is the WHNF-producing probe, not the callback body's initial
invocation. Vocabulary refinement pending.

**Closures are first-class WHNFs.** A partial application like
`cb "a"` in `cb = k: v: k == v` is fully lazy — no contra-arg
observations happen during the application itself. Force to WHNF
yields the closure `k` bound, `v` unbound. That force triggers
a QCA emission with `obs = ∅` (nothing was observed) and
`result = <function WHNF>` (user, 2026-07-22: "That first
function is fully lazy, so we naturally emit a QCA with empty
obs and result: function. Function should be part of WHNF. I
think this all just works out. Good thing functions are first-
class values").

The nested composition then works cleanly: `QCA-A(cb, obs=∅) →
function` is a normal QCA row like any other; later, when
`c2 = c1 "b"` is probed, `QCA-B(fn=QCA-A, obs=obs_b) → <result>`
references QCA-A by queryHash. No special-casing for closures.

**Code.**

Emission triggers in `TracingObject::whnf()`
(`tracing-object.cc:144-174`), gated on `applyResultSubject` being
set:

```
auto whnfResult = computeWHNFFromObject(*inner);
if (applyResultSubject)
    writer.emitCallbackApplyForApplyResult(*applyResultSubject,
                                            applyArgAncestry,
                                            whnfResult);
```

**Gap 1** (structural probes bypass emission).

`TracingObject::maybeGetAttr` (`tracing-object.cc:113-142`) calls
`inner->maybeGetAttr(name)` directly WITHOUT forcing WHNF first
and does NOT call `emitCallbackApplyForApplyResult`. Same for
`getListElem`. Consequence: if a caller does `getAttr("whatever")`
on an applyResult before ever calling `whnf()` on it, no QCA is
emitted; the getAttr's Q hash doesn't compose through
`QueryCallbackApply`.

**Model** (user, 2026-07-22): structural probes on an applyResult
should implicitly force `whnf()` first — that path already emits
the QCA. Rationale: `builtins.cache` is inherently one-probe-at-a-
time (see §6a), so it can never trigger a "sudden deep" probe.
CLI callers (which historically CAN reach deep, via `AttrCursor` /
`Object`) rely on the deeper probe being expressible as a whnf
followed by a getAttr — which requires the whnf leg to have been
emitted.

Fix direction: `maybeGetAttr` and `getListElem` on TracingObject
call `whnf()` first when `applyResultSubject` is set. Related to
B4, B7 in the status file.

**Gap 2** (B7 — contra-arg observations not reaching cells).

Diagnostic finding (2026-07-22): for
`cb-sibling-discrimination-via-observation`, cold has 4
`createCallbackCell` calls but 0 `logCallbackObservation` calls.
Cells stay empty → `emitCallbackApplyForApplyResult` early-returns
because `cell.argAncestryHex.empty()` → no QCA in DB. Root cause
requires tracing where the callback body's parameter access
actually resolves; likely the `ExprFromObject` bridge unwraps back
to raw `argObj` rather than `TracingCallbackArg`.

## 7a. Foundational Principle 9 under writer/replay asymmetry

**Model** (user, 2026-07-22).

Foundational Principle 9 (subj doc) — "a Result's factSet hash is
cumulative over the writer's session up to its `logResult`" —
**stands**. The apparent tension with task #110's per-Q basis
resolves through the writer/replay asymmetry:

- **On recording** (single trace, in-place): Principle 9 holds as
  written. The writer's cumulative session state is the
  precondition of every Terminal it records.
- **On replay** (0-many traces): the walker queries multiple
  traces from the DB. Principle 9's cumulative property applies
  to each individual trace among the many. The walker doesn't
  reproduce cold's exact session cumulative — it reproduces one
  trace's chain from its own start (∅ or a parent-anchored
  factSetHash).

"0-many" is the key framing. Recording is 1-trace; replay is
0-many, with each candidate having its own cumulative property
scoped to itself.

Per-Q envWalk isn't a revision of Principle 9 — it's the
walker-side scoping that lets 0-many candidate traces each
maintain their own cumulative property.

## 7b. Sibling cb-apply discrimination

**Model** (user, 2026-07-22).

The canonical mechanism is **obsSet in QCA queryHash**. Sibling
A's callback observes different contra-arg attributes than sibling
B's, so obs sets differ, so QCA queryHashes differ, so distinct
DB rows. Discrimination is at the query-identity level. No cur
trajectory divergence required.

Other mechanisms mentioned in older sources — cur-trajectory
divergence (subj §Matching until divergence, case 2), QueryApply
slot differences (subj §Design principle 8 corollary) — were
pre-#110 workarounds for the fixed-Q collision problem. Under Q
evolution + obsSet-in-QCA they're redundant. Only obsSet in QCA
is load-bearing today.

## 8. Matching-until-divergence

**Model** (`subj §Matching until divergence`, still valid; user
implicitly assumes throughout).

> Two evaluation events whose observations match up to some point
> are characterized identically at that point — same state hashes
> at every referenced Subject, and therefore same requestHashes on
> any observation emitted there. They are characterized distinctly
> only once their observations diverge.

Consequences:

- Cold and warm see identical observations at corresponding
  moments up to divergence.
- At any Ask edge, if warm's live dispatch produces the same
  response as cold's recorded response, walker's cur advances the
  same way.
- Under Q evolution, Q also advances identically on both sides
  through the shared prefix.
- The first divergent response ends warm's applicability. No silent
  wrong hit — divergent elementHash lands warm at a cur cold has
  no row for; walker misses cleanly.

**Code.** Guaranteed by construction, not by explicit checks. The
correctness argument is: cold's recording is deterministic given
its observation sequence; warm's per-walk observation sequence
matches cold's up to divergence; therefore hashes match up to
divergence.

**Gap** (writer/walker basis alignment).

Both sides must compute state hashes on the same basis. Task #110
introduced per-Q `perQEnvWalk` on the writer; task #110 fix B1
aligned the walker to also use walk-local `perQEnvWalk` for
`recomputeQ`. The session-cumulative `envWalk` on both sides is
used for other bookkeeping but not for Q evolution.

## 9. argAncestry composition

**Model — current design shape.**

- `argAncestry` (vocab §argAncestry) — XOR-fold of enclosing
  callback args' state hashes at the moment the innermost callback
  was entered.
- `callArgAncestry` — an `argAncestry` stored on the
  `OuterResolver`, sampled at cb-apply fire time. Contributed
  per-cached-call from source identifier hashes and XOR-folded with
  enclosing calls' contributions (primop §Architecture step 5).
- `combineArgAncestries(fnArgAncestry, argArgAncestry)` — produces
  the argAncestry INSIDE an apply-result callback body.
  **Non-commutative**: `SHA-256("apply-argAncestry:" || fnHex || ":"
  || argHex)`.

Two different operations for two different composition moments —
XOR for enclosing scopes, non-commutative combine for the specific
`f a` apply-result site.

**Model — open** (user, 2026-07-22).

The XOR compositions in this area are worth re-examining. Rule of
thumb: "use XOR at one easily controlled layer, then seal it by
hashing before letting it be XORed at another layer." Under that
rule, XOR at the enclosing-scope layer is fine only if
`callArgAncestry` contributions never get XOR-folded again
downstream without a hash seal in between.

An earlier iteration (pre-redesign, ~hundreds of commits ago)
intentionally exploited XOR cancellation. That's likely not the
case today, but the design hasn't been re-audited under the
current shape. Open to re-evaluation if there's a specific benefit;
otherwise conservative default is Merkle composition (hash-seal
between XOR layers) to prevent accidental cancellation.

**Code.**

- `combineArgAncestries` in `subject-id.cc`.
- `callArgAncestry` seeded in `primops/cache.cc` (primop step 5):
  `hashString("cache-import:" | "cache-expr:" || <source id>)`
  XOR-folded with `state.inheritedCallArgAncestry`.
- Propagated via `setAmbientResolverCallArgAncestry` and
  `innerState->inheritedCallArgAncestry`.
- Used in QCA payload: `qca.argAncestry = cell.argAncestryHex`
  (`tracing-writer.hh:302`).

The `subj §Foundational principles` doc's XOR-cancellation audit
(§Technical requirements → Component G, "sound today, under
SHA-256 entropy. Fragility lives in the PostulatedIdempotentRead
wrapping path") notes single-layer nesting is fine but flags
deeper nesting as needing Merkle composition. That audit stands.

**Gap.**

Vocab describes XOR at the enclosing-scope layer as unambiguous,
but the user has flagged this as worth re-examining. Not a bug in
current use, but the doc should encode the "hash-seal between
layers" discipline explicitly rather than treating XOR as a
default composition operator.

## 10. Trace-continuing / trace-discovering under Q evolution

The main doc's §Replay strategies carries the trace-continuing /
trace-discovering vocabulary and the axis decomposition (Axis A =
starting state, Axis B = tracking scope). This section covers
only the Q-evolution-specific interactions.

Both trace-continuing and trace-discovering rely on the same
per-Ask Q re-derivation. `TracingDecisionGraph::walk` accepts a
`recomputeQ` callback that re-derives Q from the walker's
walk-local per-Q chain observations (see §3.2). Q evolution's
within-Q basis is independent of the between-Q tracking scope —
the axes are orthogonal here. A trace-continuing walker and a
trace-discovering walker both walk one Q's trace chain through
the same Q-evolution loop; they differ in how they arrived at
that Q's entry and in how their state carries across to the next
Q.

## 11. Sub-Q composition

**Model.**

A sub-Q is a Q whose evaluation is required to answer another Q. A
semantic relationship — nesting of Nix expressions — not an
operational LIFO stack construct (user 2026-07-21: "the LIFO
nesting framing sounds laborious"), though the writer uses a LIFO
`activeQueryStack` to encode the current chain.

Under the corrected model:

- Each observation attributes to exactly one Q: the innermost
  active on the writer's stack.
- When a sub-Q completes, its `logResult` inserts a Terminal.
- The parent Q should observe the sub-Q's completion as ONE
  composite observation — one entry in parent's `perQEnvWalk`
  whose request is the sub-Q's queryHash and whose response is the
  sub-Q's resultHash.

This last part is **planned, not yet implemented** — status-file
bug **B2**.

User note (2026-07-21):

> Usually the parent is already in cur, because the proxies that
> cause evaluation are constructed one layer at a time to refer
> back to e.g. the parent query for an attribute proxy. Nonetheless,
> what you're planning here is good defensive coding, reducing
> entropy in cases where that didn't happen (CLI-specific caller?
> idk). Just make sure you're not inserting duplicates.

**Code — current workaround.**

The writer sidesteps B2 with a session-cumulative Ask insertion at
`logResult`. `tracing-writer.hh:753` iterates `envAsksEdges`
(session cumulative) and inserts each under `finalQ`:

```
Hash finalQ = activeQueryStack.empty() ? ... : activeQueryStack.back().currentQ;
// iterate envAsksEdges, insertAsk each under finalQ
```

Bridges parent Q's walker reachability at the cost of duplicating
sub-Q observations under parent's Q. Preserves pre-#110 fixed-Q
sweep-everything behaviour.

**Gap.**

- Composite observation not yet emitted (B2 not implemented).
- Bridging is a pattern hack. Doubles chain entropy under Q
  evolution.
- Walker-side change needed for the correct fix: recognize when a
  requestHash is a compound-Q (present in Queries pool) and
  recursively invoke `walk(subQ)` to fold sub's resultHash into
  parent's cur. Not sketched in code yet. Question Q3 in earlier
  draft (now retired in favor of implementation notes here).

## 12. `SuppressApplyBoundary` — necessary guard

**Model.** During `TracingReplayEvaluator::walk`, walker's dispatch
of `fnObj->queryApply(...)` re-enters the writer's callback-cell
creation path (`OuterApply::run` → `createCallbackCell`). Walker
validation shouldn't create phantom cells — no real callback firing
is happening. A guard suppresses cell creation during walker's
re-invocations.

The guard is a workaround for `OuterApply::run` doing double duty
(recording orchestration + pure-eval). The clean fix would split
those into two variants. Not urgent — the guard is small and
correct after B6.

**Code** (B6 fix, commit `719a9b2bf`).

Prior code had a global `SuppressApplyBoundary` around
`TracingReplayEvaluator::walk`'s entire body — a latent bug because
fallback triggered inside `ensureInner()` during dispatch could run
legitimate cb-applies with the guard still active, silently losing
cell creation.

B6 narrowed the guard to per-`queryApply` scope. Four wrapping
sites: `resolveApplyId`, `navigatePath`'s Apply step,
`dispatchAmbientQuery`'s callbackApply branch,
`TracingReplayEvaluator::apply`'s outer-direction branch. Fallback
in leaf ops outside those scopes runs unguarded — legitimate
cb-applies during `ensureInner` record cells normally.

**Gap.** None urgent. C5-in-old-status was "split `OuterApply::run`"
— now noted as long-term cleanup, no correctness concern.

## 13. Known gaps (living status: `doc/status.md`)

- **B2** — sub-Q composite observation not implemented; workaround
  in place (§11).
- **B3** — only `applyResult`-carrying `TracingObject` instances
  expose a Subject via `getSubject()`. Non-applyResult
  TracingObjects return `nullopt` and don't participate in Q
  evolution.
- **B4** — the applyResult's `ActiveQuery` is pushed AFTER the
  callback body returns, not around the invocation. Callback
  observations during the body attribute to the wrong Q — usually
  inner's `evalFile fn.nix` (a root query with no fromSubject).
- **B5** — fast/slow path perQEnvWalk residue (§3 Gap 2, §10 Gap).
- **B7** — contra-arg observations not reaching cells (§7 Gap 2).
- **§7 Gap 1** — structural probes on unforced applyResults don't
  emit QCA. Overlaps with B4.

B2, B3, B4 are structural design changes. B7 is a diagnostic
finding; fix requires tracing where callback parameters resolve.

## 13a. "Ambient" — retired, term available for repurposing

**Model** (user, 2026-07-22).

The Ambient message pairing dissolves entirely under task #110.
There are two message pairings now: **Query** and **Env**. QCA is
a Query variant; contra-arg observations are Facts on Env; the
cell mechanism (§5) is implementation detail, not a distinct
message-pairing layer. Vocab §Message pairings drops from three
to two; subj's Ambient sections retire; `dispatchAmbientQuery`
gets renamed.

**Term reservation** (user, 2026-07-22 — "Park as note only").
"Ambient" is a good word for an inner-global value — an outer
argument that's bound immediately at the `builtins.cache` call
(as opposed to at a callback application) and is 1:1 with the
inner evaluator. Such values don't need state hash tracking
because they can't vary during the inner's lifetime.

Reserving the term only. Not defining the concept formally,
since there's no implementation or design driver for it yet.
Naming without a use case risks the term drifting; the note here
is to prevent the freed word from being reused for something
unrelated in the meantime.

## 14. Removed machinery (for reading old comments)

The design got here by explicitly deleting several prior
mechanisms. Terms to be suspicious of in old comments / commit
messages / stale doc sections:

- **AmbientAsks table + `advanceChainAndAppendFact` +
  `withAmbientAsksValidation`** — replaced by `runningObsSet`
  accumulation. Task #109.
- **`InnerValueResponse` table + `contextHash`** — replaced by
  obsSet CAS.
- **`localArg sidecar`** — replaced by QCA's explicit
  `argAncestry`/`argDepth` payload fields.
- **`SubjectEvolutionEdge` table** — replaced by local
  `obs.fromHash == cur` filter.
- **`params.callbackApply` slot on outer probes** (task #103's
  MVP) — replaced by first-class `QueryCallbackApply` (task #110).
- **`OuterApply::run`'s emission of an outer-scope observation +
  applyContext bridge push** (task #108) — replaced by QCA
  compositional Q evolution.
- **`contextHash` / `boundaryContext` / `outerContext` on
  `ReplayCallbackArg`** — obsolete under obsSet CAS.
- **`chainCursor` + `<replay-local-lambda>` primop's XOR-advance**
  — dead once `dispatchApplyLive` was ripped.
- **The word "boundary"** — legacy. If a comment or symbol name
  uses it, be suspicious.

## 15. Retained machinery (that looks removable but isn't)

- **`CallbackCell` and `SuppressApplyBoundary`** — both stayed.
  Cell is now the per-application accumulator for `runningObsSet`
  (§6); `SuppressApplyBoundary` prevents walker-triggered phantom
  cells (§12).
- **`dispatchAmbientQuery`** (walker method name) — despite the
  name, all its branches dispatch task-#110 first-class Queries.
  Rename pending. Question Q9.

**`TracingCallbackApplyResult` — WRONG in the curried/nested case**
(user, 2026-07-22).

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
QCA-in-fn-slot vs a leaf state hash.

User note: "I wish the representations were a bit more inductive
style generally. I don't know what consequences that would have on
the arg state hashing, but have good feelings about doing it for
the callbacks. *Maybe* the strategy generalizes, but state hashes
feel messier in a way that might not be solved by inductive style."
Do recursive-Q for callbacks specifically; leave arg-side state
hashing at its current XOR-fold-plus-Merkle-seal shape.

**Two composition idioms coexist by design** (user, 2026-07-22 —
"that needs more research"). QCA nesting is inductive; state hash
evolution is fold-based. They may reflect a genuine difference in
nature: calls and callbacks have swapped inner/outer roles, so the
same composition style might not fit both. Both idioms are
unproven — the split isn't settled dogma, it's the current best
shape pending more experience.
