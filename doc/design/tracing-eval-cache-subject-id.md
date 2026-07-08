# Subject identity

Design principles for assigning state hashes to Subjects — the
structural names of values referenced in `builtins.cache`'s trie.

Companion to
[`tracing-eval-cache.md`](./tracing-eval-cache.md) (base cache
model) and
[`tracing-eval-cache-vocabulary.md`](./tracing-eval-cache-vocabulary.md)
(term glossary). This doc is the "why" behind the machinery
defined in §§12–17 of the vocab (Subject, state hash, argAncestry,
callback-arg objects, cell navigation, evolution edges) and the
Ambient interaction described in §§10–11.

## Foundational principles

Hold for any subject-identity scheme in this cache; apply to the
present design.

1. **Numbered identifiers only at the CLI**, where seed-introducing
   constructs follow predictable patterns. Reverse De Bruijn for
   generalized curry depth. Everything below the CLI is grounded in
   the real structure of expressions.

2. **Combine structural identification with intrinsic hashes** to
   identify relevant states. Structure tells you what; intrinsic
   tells you when.

3. **Replay gets all the information to reproduce Facts and
   intrinsic hashes**, so more-specific Facts can be acquired from
   less-specific states.

4. **Maintain breadcrumbs** between more-specific and less-specific
   hashes, so less-specific Facts can be retrieved at a later time.

5. **Queries must be specific enough.** Ambiguities (more than one
   matching row) get resolved by query specificity first, then by a
   custom index (Ask, evolution edges, ...). Iteration is only a
   temporary fallback.

6. **No deep hashing of values.** Identity is built from
   observations made *through* a value, never from inspecting the
   value itself. A corollary of #7; also non-negotiable on its own
   as an identity rule.

7. **Laziness end-to-end.** Forcing is initiated by the value's
   consumer, never by the cache itself. Recording observes only
   the Queries the inner already issued — never probe a value to
   manufacture Facts. Replay serves a response only when the
   consumer probes for it — never traverse recorded structure
   ahead of the consumer.

8. **Cache behavior is independent of the argument's forcedness
   state.** Whether an argument arrives at a boundary already
   forced (e.g. the interpreter happened to evaluate it eagerly
   for an adjacent primop) or as an unforced thunk must NOT change
   what the cache records or how it looks up. Identity is derived
   only from observations the *function body* (expression-defined
   behavior) makes through the value — never from observations
   the interpreter made incidentally. This makes cache performance
   predictable: a refactor that changes evaluation order cannot
   perturb cache layout. A corollary of #7; also non-negotiable
   on its own.

9. **Cumulative dependency.** The inner evaluator is a black box.
   Every Request observed prior to a Result is part of that
   Result's dependency set — the box's state has evolved through
   each observation, and the cache cannot prove which observations
   were load-bearing. Pruning the FactSet to exclude prior Facts is
   therefore disallowed: a Result's factSet hash is cumulative
   over the writer's session up to its `logResult`.

   The point of the cache is to work accurately for any outer
   caller. The outer evaluator is outside the cache boundary, and
   the cache makes no assumptions about which outer is calling it
   or how — correctness must hold across all of them.

   *Consequence for arguments.* Outer-supplied values entering the
   cache enter as Subjects (structural identity, no pre-existing
   state hash); observations the inner makes through them refine
   identity via state-hash evolution. The cache never pins an
   argument by the outer's notion of its identity — only by what
   the inner observed via Requests.

   *Consequence for callbacks.* Outer-supplied functions the inner
   applies cannot have their response served from cache. The walker
   invokes the outer live and validates the structure of the
   resulting probes via the Ambient chain. Cached state covers the
   structural contract (what probes happened, in what order, with
   what response shape) but never the response values themselves —
   those come live each time.

   Outer's referential transparency *could* be exploited to cache
   responses (same inputs reliably give same outputs within a
   given outer evaluation). Scoped out for now: only worth doing
   if measurement shows the live re-invocation cost outweighs the
   added bookkeeping.

## Design principles

Specific commitments of the present design.

1. **State hashes are pure functions of `(Subject, argAncestry,
   history, step)`.** No mutable state. The recorder and the walker
   compute the same function from the same inputs.

2. **Subjects are static structural names.** Four variants
   composing recursively (see vocab §12):
   - **`Arg{depth}`** — a callback arg at a static apply-stack
     depth (reverse De Bruijn).
   - **`DerivedSubject{parent, kind, name/index}`** — a value
     reached from a parent via a producer query (`getAttr`,
     `getListElem`).
   - **`ApplyResultSubject{fn, arg}`** — a value reached from a
     fn subject and an arg subject via `QueryApply`.
   - **`PostulatedIdempotentRead{hash}`** — a value from a re-
     readable source (file, expression string, literal).

   Leaves are `Arg` and `PostulatedIdempotentRead`; Subjects do
   not carry state hashes — only positions and structural
   relations.

3. **State hashes evolve alongside the FactSet.** Walking the trie
   advances both the cumulative `factSetHash` and every referenced
   Subject's state hash in lockstep, edge by edge. At any
   `(queryHash, factSetHash)` position, each Subject has a
   well-defined state hash determined by the function.

   For `Arg{depth=N}` at history position `k`:

   ```
   stateHashAt(Arg{N}, argAncestry, history, k)
     = initial(N, argAncestry)
       XOR (XOR-fold over observations in history[0..k)
            whose fromHash == stateHashAt(Arg{N}, argAncestry,
                                          history, prior step))
   ```

   For `DerivedSubject{parent, kind, name}` at step `k`:

   ```
   stateHashAtSubject(DerivedSubject{...}, argAncestry, history, k)
     = queryHash(producer_query{
                    from    = rootStateHash(subject, k),
                    name/index,
                    path    = path(parent)})
   ```

   where `rootStateHash(subject, k)` is the state hash of the
   subject's root at step `k`, and `path(parent)` walks the parent's
   `DerivedSubject` chain back to a root. All observations about
   derived values inside one arg dispatch carry `from =
   rootStateHash`, so derived observations fold into the root's
   own-loop and propagate to every derived Subject's state hash
   via the `from` field.

   For `ApplyResultSubject{fn, arg}`:

   ```
   stateHashAtSubject(ApplyResultSubject{...}, argAncestry,
                      history, k)
     = queryHash(QueryApply{fn = stateHashAtSubject(fn, ...),
                            arg = stateHashAtSubject(arg, ...)})
   ```

   Today apply-result observations fold into the apply-result's
   own chain, not back into `fn`'s. Function characterization —
   feedback that would distinguish siblings differing only in
   apply behavior — is a follow-up.

4. **Membership in "observations about V" is decided per Ask edge.**
   At an Ask edge's precondition FactSet, each Subject has a
   state hash; observations in this edge whose `fromHash` equals
   that hash are observations on V for this edge. New edges
   re-decide membership against their own precondition. No global
   filter, no recursive resolution at Fact-emission time.

   Under per-arg centralisation, all derived observations on V
   share the arg root's `fromHash` and discriminate by `path`
   inside the query — "observations about V" within an edge is
   the `(fromHash, path)` pair, not `fromHash` alone.

5. **At recording flush, observation `from` fields are rewritten
   per Ask edge.** The recorder buffers observations during a
   query's evaluation carrying placeholder identifiers. At flush,
   it builds Ask edges and substitutes each observation's `from`
   with the state hash of the referenced Subject at that edge's
   precondition FactSet. Pool keys (`requestHash`) and Ask edges
   are hashed over the post-substitution form.

   This substitution **extends to `QueryApply` requests too**: the
   apply's `arg` (and, if applicable, `fn`) fields are rewritten
   from placeholders to the referenced Subjects' state hashes at
   the relevant edge's precondition FactSet. The cb-apply gets a
   `requestHash` reflecting observations accumulated on the
   constituent Subjects up to its edge, so distinct sibling cb
   invocations land in distinct trie positions without new hashing
   machinery — the existing evaluation does the work.

6. **Walker advances state hashes in lockstep with `cur`.** As
   each Ask edge dispatches its Requests, the walker XOR-folds
   each dispatched Response into both `factSetHash` and the state
   hashes of relevant Subjects. Symmetric to recording.

   **Navigation invariant (mirroring the base cache's "hashes flow
   into lookups as keys, never out").** The walker's inputs to
   state-hash computation are the current walk state (`envWalk`
   for the Env layer; the Subject-evolution walker for
   Ambient/Subject-identity) and the Subjects it holds via its
   `currentProxy` chain (see vocab §16). For any known Subject
   `S`, the walker *produces* `S`'s state hash at any history
   position `k` by hashing (`stateHashAt(S, argAncestry, history,
   k)`). State hashes are outputs of hashing, then used as keys
   to look up content — never outputs of a lookup that answers
   "which Subject produces this target state hash?" When a
   dispatched Request carries `from = X`, the walker computes
   state hashes for each held Subject at each history position and
   checks equality against X. If a held Subject's computed state
   hash equals X, the walker routes the dispatch through that
   Subject's live proxy. If none matches, walker misses cleanly —
   no Subject is invented, no depth speculated. The recorder's
   symmetric obligation is that any Subject whose state hash
   appears in its recorded Facts is a Subject the walker
   demonstrably holds at the corresponding walk state.

7. **XOR-fold commutativity preserves concurrency within an edge.**
   Dispatch order within a single Ask edge does not affect the
   resulting state hashes or `factSetHash`. Edges advance one at
   a time; within an edge, requests may be probed concurrently.

   *Recorder/walker alignment obligation.* The writer's
   subject-id history advances in 1:1 lockstep with its
   `envAsksEdges`: every Ask edge added on the writer side is
   paired with a subject-id edge at the same index (ε boundaries
   insert into both; trailing closes append to both). The walker
   pushes to its subject-id history for every Ask edge it
   traverses — even when the edge contributes no observations to
   a given Subject. Without this alignment, the writer's
   flush-time state hash for a Subject at edge `k` and the
   walker's re-computation at the same `k` disagree, and every
   Query keyed on the disagreeing hash misses.

8. **Same-shape collapse is automatic.** Two Subjects with
   identical observation histories evaluate to identical state
   hashes by the function. The trie's Patricia split factors
   shared prefixes; new recordings that diverge from prior ones
   get their own branch from the divergence point onward, and
   their state hashes past that point compose the shared-prefix
   contribution with the divergent observations.

   *Corollary (from #3 ∧ Foundational #9): discrimination of
   behaviorally-distinct values is automatic.* Two mechanisms
   work in tandem:

   - **Structural distinction comes from the query.** Each
     cb-apply records a `QueryApply{fn, arg}` whose payload places
     constituent state hashes in distinct slots. The apply-result's
     state hash is the SHA-256 of that filled-in query (#3's
     apply-result formula). `f 1 2` and `f 2 1` produce different
     queries — `1`'s state hash and `2`'s state hash end up in
     different slots — so the resulting state hashes differ.
     Nested applies propagate: the second cb-apply's `fn` slot
     holds the first's state hash. Works with or without
     observations.

   - **Observational distinction via XOR-fold** (#3's `Arg`
     formula + #6). When the walker dispatches an observation
     whose live response diverges from a prior recording, the
     response XOR-folds into both `cur` and the relevant
     Subjects' state hashes. The next Ask or Terminal lookup uses
     the new `cur` — sibling cb-invocations whose observations
     diverge end up in distinct trie positions automatically.

   The XOR-fold collapses *observation-path multiplicity* into a
   single state hash per Subject (distinct ways to reach the same
   set of observations contribute to one folded identity), so
   per-`<foo>` state hashes for each "arg observed through
   `<foo>`" path would be redundant.

## Technical requirements

### XOR cancellation and commutativity

We use XOR-fold in two places: the FactSet hashes (`envFactSetHash`
and every `factSetHash` used as a key in `Ask` / `Terminal` /
`AmbientAsk`) and the subject-id own-fold inside `stateHashAt`.
XOR has set semantics (commutative, associative, self-inverse
with identity 0), which is load-bearing where set algebra is
intended and a soundness hazard everywhere else.

**Requirement.** For every value `v` produced by an XOR operation:

1. *No multiset.* The XOR-fold's inputs must be a true set:
   membership dedup'd by hash before folding (a `std::set` or
   equivalent), or guaranteed unique by upstream construction
   (e.g., subject-id-evolved `requestHash`es). Without this,
   folding the same element in twice silently cancels.

2. *No order assumption.* Any consumer that uses `v` as an
   identifier must accept that two different *sequences* of inputs
   producing the same set yield the same `v`. If order matters,
   serialise the sequence into the input before hashing (via a
   SHA-256 seal); don't reach for XOR.

3. *Accidental leakage* — the property the other two often hide
   behind. Every XOR is an *edge* in the dataflow graph between
   its two inputs and its output. Take the transitive closure:
   any value reachable from `v` along XOR edges (forward or
   backward) is in the same "XOR-connected component" and shares
   `v`'s set/cancellation algebra. A consumer that needs Merkle
   uniqueness must either sit *outside* the component (read `v`
   only after it's been absorbed into a SHA-256 atom via concat +
   hash) or accept set semantics. Watch in particular for the
   compounding case: an XOR-derived value fed *back into* another
   XOR composition layers the algebra over multiple unrelated
   inputs, each adding collision surface.

**Methodology for auditing.** Treat each XOR operation as an edge,
each value as a node, and ignore Merkle composition (SHA-256 of a
concatenated payload, which absorbs its inputs into an opaque atom
and ends the chain). Identify maximal XOR-connected components.
For each component:

- Name its generators (the atomic inputs entering it via XOR;
  these should all be fresh SHA-256 outputs).
- Document what set algebra the component represents.
- Enumerate the consumers at the component's boundary. Verify
  each either accepts set semantics or sees only the
  post-SHA-256-sealed form.
- For each XOR operation inside the component, classify each
  operand as "atomic input" or "drawn from this component". The
  latter creates compounding; cap the allowed depth and prefer
  Merkle composition for any case that would exceed it.

This is mechanical to do once and cheap to re-check during
review: introducing a new XOR site requires justifying its
component membership and any new compounding it introduces.

**Audit catalogue (as of this writing).**

*Component F — FactSet algebra.*
- Generators: `elementHash(req, resp) = SHA-256(req || resp)` —
  fresh Merkle atoms.
- Members: `envFactSetHash`; every `factSetHash` used as an
  `Ask` / `Terminal` key; every `fromFactSetHash` /
  `toFactSetHash` / `cumulativeFactSet` in an `AmbientAsk` chain.
- Operations: XOR-fold; identity = 0 (`emptySetHash`).
- Boundary consumers: SQL PK / equality lookups in `Ask`,
  `Terminal`, `AmbientAsk`.
- Set algebra is *intended* at every boundary — two recordings
  with the same set of (req, resp) pairs are meant to collide.
- Compounding: none — every fold operand is a fresh SHA-256 atom.
  Dedupe is the caller's responsibility (via `std::set` /
  `sort+dedup` / subject-id-evolved `requestHash` uniqueness).
- Verdict: sound.

*Component G — Subject-identity state hashes.*
- Generators:
  - `SHA-256("positional-D")` (atomic per depth).
  - `callArgAncestry = SHA-256("cache-import:..." | "cache-expr:...")`
    (atomic per cached call; XOR-folded with enclosing cached
    calls' contributions).
  - `queryHash(QueryGetAttr{name, from=hex(parent.stateHash)})` /
    `queryHash(QueryApply{fn=hex(fn.stateHash),
    arg=hex(arg.stateHash)})` — Merkle seals over XOR-derived
    parent/constituent state hashes, but the seal makes the output
    atomic from G's perspective.
  - `elementHash(req, resp)` for own-fold contributions.
- Members: every state hash returned by `stateHashAt` /
  `stateHashAtSubject`; every partial own-fold sum; every
  structural leaf for `Arg` / `PostulatedIdempotentRead`.
- Operations:
  - Leaf: `structural = base XOR argAncestry`.
  - Own-fold: `own(k) = XOR-fold of {f.elementHash : f at edges
    < k with f.fromHash == myStateHashAt(k)}`.
  - Final: `stateHash(k) = structural(k) XOR own(k)`.
- Boundary consumers:
  - Hex-encoded into `query.from`, then `queryHash(query)` —
    **SHA-256 seal exits G**.
  - Equality check inside the subject-id filter
    (`f.fromHash == myStateHashAt(k)`) — **stays in G**.
- Compounding sites observed:
  - `PostulatedIdempotentRead{X}.structural = X XOR argAncestry`
    where `X` is itself drawn from G: happens at
    `replay-callback-arg.hh` (walker, with `argAncestry=0` — the
    new argAncestry is the identity, so no algebraic compounding)
    and at `tracing-callback-arg.cc` (recorder, wrapping the
    apply's `argSubject.stateHash` when the arg has no proper
    Subject — one nesting layer, distinct argAncestry from the
    wrapped stateHash's origin).
  - All current sites are one layer deep. A second nesting layer
    (wrapping a `PostulatedIdempotentRead`-derived value into
    another `PostulatedIdempotentRead`) is the moment to worry —
    it stacks two independent argAncestries via XOR and the
    cancellation surface grows multiplicatively.
- Verdict: sound today, under SHA-256 entropy. Fragility lives in
  the `PostulatedIdempotentRead` wrapping path; cap allowed
  nesting depth at 1 and prefer Merkle composition (via
  `queryHash(QueryWrap{inner=hex(X)})`) for any deeper case.
- Per-use rule (separate from the nesting-depth audit above):
  see the variant's docstring at
  `src/libexpr/include/nix/expr/subject-id.hh`.
  `PostulatedIdempotentRead` postulates that the *source* can be
  re-read idempotently (fs reads under snapshot semantics,
  expression strings hashed for parsing). It does NOT promise
  that the resulting Subject's state hash is stable — the full
  `stateHashAt` is still a function of `(subject, argAncestry,
  history, step)`, and the own-loop continues folding
  observations whose `from` matches the running state hash.
  Invalid uses: values that can't be characterized completely
  ahead of time (lazy fn args given as a `Value`); taking a
  Subject's state hash by value and treating it as up-to-date.

*Cross-component bridges.*
- G → F: `stateHash → hex → queryHash(query) → SHA-256 seal →
  requestHash → elementHash(requestHash, respHash)`. Every step
  is a Merkle seal; F sees uncorrelated SHA-256 atoms regardless
  of G's internal algebra.
- F → G: not observed in the codebase. No `factSetHash` is fed
  into a subject-id construction. If introduced, the `factSetHash`
  would arrive XOR-derived and would compound with G's algebra —
  keep it banned.

## Ambient interaction: how Subject identity crosses the boundary

Vocab §§10–11 define the Ambient interaction. This section is the
subject-identity view of it: what happens to Subjects and their
state hashes when the outer probes an inner-supplied callback arg.

### Inheritance of state hashes across the boundary

A Subject's `stateHashAt` characterizes evolution *within one arg*.
The value's full characterization in a particular evaluation
context is that state hash composed with the state hashes of its
enclosing scopes — and at the cb-apply boundary, the outermost
enclosing scope is the cached call itself, contributed via
`callArgAncestry` (see vocab §14).

For a callback-arg value:

```
fullCharacterization(callbackArg)
  = stateHashAt(subject, argAncestry, history, step)
  ⊕ callArgAncestry             ← inheritance from the cached call
```

Two cb invocations from different cached calls (different
enclosing cache boundaries) have different `callArgAncestry`
values, so their callback-arg characterizations diverge from the
very first probe even though their `Arg{depth}` Subjects are
structurally identical. Their Ambient probes carry different
`from` fields and land in disjoint regions of the Ambient trie.

Two cb invocations from the *same* cached call with otherwise
identical Env-layer observations up to the apply have identical
characterizations — correctly: they share the same FactSet
position, meaning the inner evaluator's state at the apply is
literally the same. The arg was passed but not yet forced;
there's no information by which the two executions can differ
at that moment. Divergence can only arise once the outer starts
probing — and the Ambient trie captures that by observation
evolution.

Inheriting outer-scope contributions ripples through every
observation, so atom sharing across cached calls is reduced.
Deliberate trade-off: storage cost in exchange for
collision-free disambiguation of callback-arg identity.

### Ambient response storage

`InnerValueResponse` (vocab §11) is a persistent table
`(requestHash, contextHash) → payload` keyed on the walker's
Env-interaction `cur` at record time, not on the response's
own hash. That's not a CAS pool — it's a keyed table, and the
Ambient walker is the only consumer.

The `contextHash` disambiguates same-Request observations under
different outer contexts. Without it, two recordings that
reach the same `requestHash` under different outer args could
overwrite each other's responses; with it, each outer context
gets its own entry. (Historically the table was keyed on
`requestHash` alone and had a first-writer-wins collision on
`cb-repeated`'s `(cb 10) + (cb 20)` case — same abstract
`requestHash`, distinct outer contexts, wrong replay. Adding
`contextHash` closed that gap.)

The pool stores the callback-arg's value structure too: small
atoms covering attrset entries, list elements, scalars.
Content-addressed at the atom level. They're what the walker
uses to reconstruct a frozen callback-arg image at Ambient
replay.

**Lambda callback-args don't need their body stored.** A
lambda's atom is just `(subjectHash, kind=lambda)`; the walker
reconstructs it as a primop `Value` whose `impl`, when applied,
consults the `AmbientAsk` trie for a recorded edge matching the
live arg's evolved state hash, and either reproduces the
recorded apply result from stored atoms or throws an
ambient-interaction divergence exception that the surrounding
walker catches as a miss. The lambda's "application behavior"
is encoded in the recorded `AmbientAsk` edges and
`InnerValueResponse` payloads, not in a stored body.

### `AmbientAsk` — the Ambient walker's edge table

```
AmbientAsk(fromFactSetHash BLOB, requestSetHash BLOB,
           toFactSetHash   BLOB,
           PRIMARY KEY (fromFactSetHash, requestSetHash)) WITHOUT ROWID
```

No `queryHash` column — Ambient keys edges on `factSet` alone.
Inheritance discriminates *across cached calls* by folding the
cached call's identity into `callArgAncestry`, which enters
every downstream Subject's state hash at the cb-apply boundary;
every observation's `from` (and therefore `requestHash`) is
unique per cached-call invocation from the very first probe.
Discrimination *within* one cached call's sibling cb-apply
invocations is separate (same `callArgAncestry`, same initial
`Arg{depth}` state hash, so `from` values start identical;
siblings diverge via observation evolution per principle 8's
corollary).

`AmbientAsk` is a *validation skeleton*, not a response source.
It records which probes appeared, in what order, with what
resulting factSet transitions. It does *not* hold a
`requestHash → responseHash` index — that mapping is provided
by the environment (at Env: files / outer evaluator; at Ambient:
`InnerValueResponse` payloads reconstructed as a live value tree).

`RequestSetNodes` is reused for Ambient request-set storage —
same trie machinery, members are Ambient request hashes.

### Recording (Ambient)

On the cold path, when the inner emits an Ambient query (a
cb-apply), the writer enters Ambient recording. The environment
is the live inner-constructed callback arg. As the outer's fn
evaluates against it, each probe is an Ambient request with
`from = hex(callbackArg's current state hash)`; the callback
arg's reveal is the Ambient response.

Each observation's `elementHash = SHA-256(requestHash ||
responseHash)` XOR-folds into the running Ambient `factSet`.
At flush, edges (`AmbientAsk(fromFactSet, {requestHash}) →
toFactSet`) are inserted; any new atom payloads land in the
storage layer. The terminal factSet hash is the Ambient result,
which the Env-layer walker XOR-folds into its own `cur` as the
Response for the enclosing Ambient query.

### Replay (Ambient)

The Env-layer walker, having dispatched everything else live and
reached the Ambient query, enters Ambient replay.

The walker reconstructs the callback arg as a live Nix Value
tree from stored atoms, keyed via the callback arg's
characterization computed live from inherited context. It hands
the tree to the outer as the bridged arg and lets the outer's
fn run natively. The walker's role from here is purely
*observational*:

| Per outer probe | Walker does |
|---|---|
| compose Ambient request using `from = hex(currentStateHash)` | hash → requestHash |
| lookup `AmbientAsk(currentFactSet, {requestHash})` | confirms this probe was recorded at this position |
| observe the value tree's reveal (the outer's evaluation of the probe) | hash → responseHash |
| XOR-fold elementHash into currentFactSet | check the result matches the edge's `toFactSet` |

Both checks together: "the probe matches the recording" *and*
"the live response matches the recording." If both pass,
advance. When the outer finishes probing, the final factSet
hash is the Ambient result handed up to the Env-layer walker.

The walker never serves a response from a stored index. The
reconstructed value tree drives the outer; the outer's native
evaluation produces responses just like any Nix value. The env
remains the source of the `request → response` mapping,
exactly as at Env layer.

### Where staleness is caught

Ambient has no role in stale-cache detection — that's owned
entirely by the Env layer's input tracing. A changed file or
environment variable surfaces as a divergent Response at Env
dispatch; the Env-layer walker's `(queryHash, factSet)` lookup finds no
Ask edge at the new factSet and falls through. The Ambient
query lives at a specific factSet position in the Env-layer
trie; if the Env walker doesn't reach that position, Ambient is
never entered.

## Implementation hints

Not principles. Mnemonics and guidance for implementers.

### Env → Ambient flip on no-cached-Subject

At a cb-apply, the requester/responder roles invert: at Env,
inner asks outer or the system environment; at Ambient, outer
asks inner via the inner-supplied callback arg. The flip is
conceptual — a mnemonic for the symmetry, not a physical
reconfiguration. When the walker encounters a cb-apply Request
whose state hash has no recorded Fact, it runs the outer fn
live; the Ambient chain validates the probes the outer makes
back on the callback arg. Outer is never cached for serving;
Ambient only validates structure.

Caveat: speculatively performing an unused callback is
undesirable; mitigations postpone-able.
