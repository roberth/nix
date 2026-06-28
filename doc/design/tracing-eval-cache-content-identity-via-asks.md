# Content identity via Asks

Design principles for assigning content-defined identities to values
referenced in the facts of `builtins.cache`'s trie.

The trie (defined in [`tracing-eval-cache.md`](./tracing-eval-cache.md))
addresses positions by `(queryHash, factSetHash)` and connects them
via `Asks` and `Terminals` edges. This document specifies how content
ids attach to that machinery.

## Foundational principles

These hold for any content-identity scheme in this cache and apply
to the present design.

1. **Numbered identifiers only at the CLI**, where seed-introducing
   constructs follow predictable patterns. Reverse De Bruijn for
   generalized curry depth. Everything below the CLI is grounded in
   the real structure of expressions.

2. **Combine structural identification with intrinsic hashes** to
   identify relevant states. Structure tells you what; intrinsic
   tells you when.

3. **Replay gets all the information to reproduce the facts and
   intrinsic hashes**, so more-specific facts can be acquired from
   less-specific states.

4. **Maintain breadcrumbs** between more-specific and less-specific
   hashes, so less-specific facts can be retrieved at a later time.

5. **Queries must be specific enough.** Ambiguities (more than one
   matching row) get resolved by query specificity first, then by a
   custom index (Asks, decision trees, ...). Iteration is only a
   temporary fallback.

6. **No deep hashing of values.** Identity is built from observations
   made *through* a value, never from inspecting the value itself.
   A corollary of laziness (#7); also non-negotiable on its own as
   a content-identity rule.

7. **Laziness end-to-end.** Forcing is initiated by the value's
   consumer, never by the cache itself. Recording observes only the
   queries the inner already issued — never probe a value to
   manufacture facts. Replay serves a response only when the
   consumer probes for it — never traverse recorded structure ahead
   of the consumer.

8. **Cache behavior is independent of the argument's forcedness
   state.** Whether an argument arrives at a boundary already forced
   (= e.g. the interpreter happened to evaluate it eagerly for an
   adjacent primop) or as an unforced thunk must NOT change what
   the cache records or how it looks up. Identity is derived only
   from observations the *function body* (= expression-defined
   behavior) makes through the value — never from observations the
   interpreter made incidentally. This is what makes cache
   performance predictable: a refactor that changes evaluation
   order cannot perturb cache layout. A corollary of (#7); also
   non-negotiable on its own.

9. **Cumulative dependency.** The inner evaluator is a black box.
   Every Request observed prior to a Result is part of that
   Result's dependency set — the box's state has evolved through
   each observation, and the cache cannot prove which observations
   were load-bearing. Pruning the factSet to exclude prior facts is
   therefore disallowed: a Result's factSet hash is cumulative over
   the writer's session up to its logResult.

   The point of the cache is to work accurately for any outer
   caller. The outer evaluator is outside the cache boundary, and
   the cache makes no assumptions about which outer is calling it
   or how — correctness must hold across all of them.

   *Consequence for arguments.* Outer-supplied values entering the
   cache enter as positional seeds (structural identity, no
   pre-existing CID); observations the inner makes through them
   refine identity via CID evolution. The cache never pins an
   argument by the outer's notion of its identity — only by what
   the inner observed via Requests.

   *Consequence for callbacks.* Outer-supplied functions inner
   applies cannot have their response served from cache. The
   walker invokes outer live and validates the structure of the
   resulting probes via the d2 chain. Cached state covers the
   structural contract (= what probes happened, in what order,
   with what response shape) but never the response values
   themselves — those come live each time.

   Outer's referential transparency *could* be exploited to cache
   responses (= same inputs reliably give same outputs within a
   given outer evaluation). Scoped out for now: only worth doing
   if measurement shows the live re-invocation cost outweighs the
   added bookkeeping.

## Design principles

These are the specific commitments of this design.

1. **Content ids are pure functions of `(subject, factset)`.** No
   mutable state. The recorder and the walker compute the same
   function from the same inputs.

2. **Subjects are static structural identifiers.** Three forms,
   composing recursively:
   - **Positional seed** — a cb arg at a static apply-stack depth N
     (reverse De Bruijn).
   - **Derived** — a value reached from a parent subject via a
     producer query (`getAttr name`, `getListElem index`).
   - **Apply result** — a value reached from a fn subject and an arg
     subject via `QueryApply`.

   Leaves are positional seeds; subjects do not carry content
   hashes — only positions and structural relations.

3. **Content ids evolve alongside the factset.** Walking the trie
   advances both the cumulative `factSetHash` and every referenced
   subject's content id in lockstep, edge by edge. At any
   `(queryHash, factSetHash)` position, each subject has a
   well-defined content id determined by the function.

   For a positional seed at depth N at factset F:

   ```
   contentId(seed_N, F)
     = initial(N) XOR (XOR-fold over observations in F about seed_N)
   ```

   For a derived subject S = parent[name] at factset F (= per-arg
   centralization, task #86):

   ```
   contentId(S, F) = qH(producer_query{
                         from = root_cdi(S, F),
                         name/index,
                         path = path(parent of S)})
   ```

   where `root_cdi(S, F) = contentId(rootSubject of S, F)` and
   `path(P)` walks `P`'s DerivedSubject chain back to its root. All
   observations about derived values inside one cb_arg dispatch
   carry `from = root_cdi`, so derived observations fold into the
   root's own-loop and propagate to every derived subject's content
   id via the `from` field of the structural formula above.

   For an apply-result subject S:

   ```
   contentId(S, F) = qH(producer_query of S with constituent
                        subjects' content ids substituted at F)
   ```

   Today apply-result observations fold into the apply-result's own
   chain, not back into `fn`'s. Function characterization (= the
   feedback that distinguishes siblings differing only in apply
   behavior) is task #87.

4. **Membership in "observations about V" is decided per Asks edge.**
   At an Asks edge's precondition factset, each subject has a
   content id; observations in this edge whose `from` field equals
   that id are observations on V for this edge. New edges re-decide
   membership against their own precondition. No global filter, no
   recursive resolution at fact-emission time.

   Under per-arg, all derived observations on V share the cb_arg
   root's `from` and discriminate by `path` inside the query —
   "observations about V" within an edge is the `(from, path)` pair,
   not `from` alone.

5. **At recording flush, observation `from` fields are rewritten per
   Asks edge.** The recorder buffers observations during a query's
   evaluation carrying placeholder identifiers. At flush, it builds
   Asks edges and substitutes each observation's `from` to the
   content id of the referenced subject at that edge's precondition
   factset. Pool keys (`reqHash`) and Asks edges are content-addressed
   over the post-substitution form.

   This substitution **extends to `QueryApply` requests too**: the
   apply's `arg` (and, if applicable, `fn`) fields are rewritten
   from placeholders to the referenced subjects' content ids at the
   relevant edge's precondition factset. The cb-apply itself thus
   gets a content-defined `requestHash` reflecting observations
   accumulated on the constituent subjects up to its edge, so
   distinct sibling cb invocations land in distinct trie positions
   without requiring new hashing machinery — the existing cidasks
   evaluation does the work.

6. **Walker advances content ids in lockstep with `cur`.** As each
   Asks edge dispatches its requests, the walker XOR-folds each
   dispatched response into both the cumulative `factSetHash` and
   the content ids of relevant subjects. Symmetric to recording.

7. **XOR-fold commutativity preserves concurrency within an edge.**
   Dispatch order within a single Asks edge does not affect the
   resulting content ids or `factSetHash`. Edges advance one at a
   time; within an edge, requests may be probed concurrently.

8. **Same-shape collapse is automatic.** Two subjects with identical
   observation histories evaluate to identical content ids by the
   function. The trie's Patricia split factors shared prefixes; new
   recordings that diverge from prior ones get their own branch
   from the divergence point onward, and their content ids past that
   point compose the shared-prefix contribution with the divergent
   observations.

   *Corollary (= #3 ∧ Foundational #9): discrimination of
   behaviorally-distinct values is automatic.* Two mechanisms
   work in tandem:

   - **Structural distinction comes from the query.** Each
     cb-apply records a `QueryApply{fn, arg}` whose payload places
     constituent CIDs in distinct slots. The apply-result's CID
     is the SHA-256 of that filled-in query (= #3's apply-result
     formula). `f 1 2` and `f 2 1` produce different queries —
     `1.cid` and `2.cid` end up in different slots — so the
     resulting CIDs differ. Nested applies propagate: the second
     cb-apply's `fn` slot holds the first's CID. Works with or
     without observations.

   - **Observational distinction via XOR-fold (= #3's positional
     seed formula + #6).** When the walker dispatches an
     observation whose live response diverges from a prior
     recording, the response XOR-folds into both `cur` and the
     relevant subjects' CIDs. The next Asks or Terminal lookup
     uses the new `cur` — sibling cb-invocations whose
     observations diverge end up in distinct trie positions
     automatically.

   The XOR-fold collapses *observation-path multiplicity* into a
   single CID per subject (= distinct ways to reach the same set
   of observations contribute to one folded identity), so
   per-`<foo>` CIDs for each "arg observed through `<foo>`" path
   would be redundant.

## Technical requirements

### XOR cancellation and commutativity

We use XOR-fold in two places: the factSet hashes
(`v13FactSetHash` / `factSetHash` keys in `Asks`/`Terminals` and
the `fromFactSet`/`toFactSet` cursors in `AmbientAsks`) and the
cidasks own-fold inside `contentIdAt`. XOR has set semantics
(commutative, associative, self-inverse with identity 0), which is
load-bearing where set algebra is intended and a soundness hazard
everywhere else.

**Requirement.** For every value `v` produced by an XOR operation:

1. *No multiset.* The XOR-fold's inputs must be a true set:
   membership dedup'd by hash before folding (= a `std::set` or
   equivalent), or guaranteed unique by upstream construction (=
   e.g., cidasks-evolved `reqHash`es). Without this, folding the
   same element in twice silently cancels.

2. *No order assumption.* Any consumer that uses `v` as an
   identifier must accept that two different *sequences* of inputs
   producing the same set yield the same `v`. If order matters,
   serialise the sequence into the input before hashing (= via a
   SHA-256 seal); don't reach for XOR.

3. *Accidental leakage* — the property the other two often hide
   behind. Every XOR is an *edge* in the dataflow graph between
   its two inputs and its output. Take the transitive closure: any
   value reachable from `v` along XOR edges (= forward or
   backward) is in the same "XOR-connected component", and shares
   `v`'s set/cancellation algebra. A consumer that needs Merkle
   uniqueness must either sit *outside* the component (= read `v`
   only after it's been absorbed into a SHA-256 atom via concat +
   hash) or accept set semantics. Watch in particular for the
   compounding case: an XOR-derived value fed *back into* another
   XOR composition layers the algebra over multiple unrelated
   inputs, each adding collision surface.

**Methodology for auditing.** Treat each XOR operation as an edge,
each value as a node, and ignore Merkle composition (= SHA-256 of a
concatenated payload, which absorbs its inputs into an opaque atom
and ends the chain). Identify maximal XOR-connected components.
For each component:

- Name its generators (= the atomic inputs entering it via XOR;
  these should all be fresh SHA-256 outputs).
- Document what set algebra the component represents.
- Enumerate the consumers at the component's boundary. Verify each
  either accepts set semantics or sees only the post-SHA-256-sealed
  form.
- For each XOR operation inside the component, classify each
  operand as "atomic input" or "drawn from this component". The
  latter creates compounding; cap the allowed depth and prefer
  Merkle composition for any case that would exceed it.

This is mechanical to do once and cheap to re-check during review:
introducing a new XOR site requires explicitly justifying its
component membership and any new compounding it introduces.

**Audit catalogue (as of this writing).**

*Component F — factSet algebra.*
- Generators: `factElementHash(req, resp) = SHA-256(req || resp)` —
  fresh Merkle atoms.
- Members: `v13FactSetHash`; every `factSetHash` used as an
  `Asks`/`Terminals` key; every `fromFactSet`/`toFactSet`/
  `cumulativeFactSet` in an `AmbientAsks` chain.
- Operations: XOR-fold; identity = 0 (= `emptySetHash`).
- Boundary consumers: SQL PK / equality lookups in `Asks`,
  `Terminals`, `AmbientAsks`.
- Set algebra is *intended* at every boundary — two recordings
  with the same set of (req, resp) pairs are meant to collide.
- Compounding: none — every fold operand is a fresh SHA-256 atom.
  Dedupe is the caller's responsibility (= via `std::set` /
  `sort+dedup` / cidasks-evolved `reqHash` uniqueness).
- Verdict: sound.

*Component G — cidasks subject identity.*
- Generators:
  - `SHA-256("positional-D")` (atomic per depth).
  - `callScope = SHA-256("cache-import:..." | "cache-expr:...")`
    (atomic per cached call).
  - `qH(QueryGetAttr{name, from=hex(parent.cdi)})` /
    `qH(QueryApply{fn=hex(fn.cdi), arg=hex(arg.cdi)})` — Merkle
    seals over XOR-derived parent/constituent cdis, but the seal
    makes the output atomic from G's perspective.
  - `factElementHash(req, resp)` for own-fold contributions.
- Members: every `cdi` returned by `contentIdAt`; every `own_k`
  partial sum; every `structural(k)` for `PositionalSeed` /
  `OpaqueContentSubject`.
- Operations:
  - Leaf: `structural = base XOR scope`.
  - Own-fold: `own_k = XOR-fold of {f.elementHash : f at edges
    <k with f.fromHash == myCidAtK}`.
  - Final: `cdi(k) = structural(k) XOR own_k`.
- Boundary consumers:
  - Hex-encoded into `query.from`, then `qH(query)` — **SHA-256
    seal exits G**.
  - Equality check inside the cidasks filter
    (`f.fromHash == myCidAtK`) — **stays in G**.
- Compounding sites observed:
  - `OpaqueContentSubject{X}.structural = X XOR scope` where `X`
    is itself drawn from G: today this happens at
    `replay-local-object.hh` (walker, with `scope=0` — the new
    scope is the identity, so no algebraic compounding) and at
    `tracing-local-object.cc` (recorder, wrapping the apply's
    `argObj.cdi` when the arg has no proper Subject — one nesting
    layer, distinct scope from the wrapped cdi's origin).
  - All current sites are one layer deep. A second nesting layer
    (= wrapping an `OpaqueContent`-derived value into another
    `OpaqueContent`) is the moment to worry — it stacks two
    independent scopes via XOR and the cancellation surface area
    grows multiplicatively.
- Verdict: sound today, under SHA-256 entropy. Fragility lives in
  the `OpaqueContent` wrapping path; cap allowed nesting depth at
  1 and prefer Merkle composition (= `qH(QueryWrap{inner=hex(X)})`)
  for any deeper case.
- Per-use rule (separate from the nesting-depth audit above):
  `OpaqueContentSubject{H}` freezes its CDI to `H` ⊕ scope —
  observation-driven evolution does not apply because the
  cidasks own-loop only folds in facts whose `fromHash ==
  myCidAtK`, and `myCidAtK` for an `OpaqueContent` subject is
  constant in `k`. Legitimate only when the subject describes
  an *atom whose constituents are already hashed into `H`* and
  no subsequent observation should re-discriminate it.
  Today's legitimate site is `TracingWriter::logDepth2ApplyFact`,
  whose subject is the cb-apply Fact — the apply has happened
  and its CDI doesn't evolve. Illegitimate use is as the subject
  of *observations* whose discrimination should depend on later
  facts; that's the "Fix B" pattern documented in
  [`tracing-eval-cache-per-arg-completion.md`](./tracing-eval-cache-per-arg-completion.md#cautionary-tale-fix-b-opaquecontent-for-apply-result-observations).

*Cross-component bridges.*
- G → F: `cdi → hex → qH(query) → SHA-256 seal → reqHash →
  factElementHash(reqHash, respHash)`. Every step is a Merkle seal;
  F sees uncorrelated SHA-256 atoms regardless of G's internal
  algebra.
- F → G: not observed in the codebase. No `factSetHash` is fed
  into a cidasks construction. If introduced, the `factSetHash`
  would arrive XOR-derived and would compound with G's algebra
  — keep it banned.

## Layering: depth-1 vs depth-2

The cache stacks two `(Query, Result)` / `(Request, Response)`
layers. Depth-0 is the user's `builtins.cache` invocation
(`Query` → `Result`). Depth-1 is *input tracing*: the
cache's interaction with files, env vars, and the outer evaluator.
Depth-2 is *interaction tracing*: the unpacking of one specific
depth-1 `Request` — the cb apply — into the probe-by-probe
exchange with the outer evaluator over a LocalObject.

Within a depth-2 sub-trace the four atoms are:

| Type | Role | Concretely, in a cb apply |
|---|---|---|
| `AmbientQuery` | the enclosing ask | `QueryApply{fn, arg}` — inner asks outer to compute `f x` |
| `AmbientResult` | the enclosing answer | the depth-2 terminal factSetHash — i.e. the value the depth-1 walker XOR-folds in as the `Response` to the `AmbientQuery` |
| `AmbientRequest` | a sub-ask inside the enclosing one | each probe the outer makes against the inner-supplied local — "what's the type at this position", "give me attr `left`", "give me your int value" |
| `AmbientResponse` | the answer to a sub-ask | the data the LocalObject reveals at that probe |

These four relate to each other exactly as `Query`/`Result`/
`Request`/`Response` do at depth-1 — the `Ambient` prefix is the
depth-2 lift. The direction inverts going from depth-1 to depth-2:
depth-1 `Request` flows cache → env (cache asks); the cb apply
inverts the asker, so depth-2 `AmbientRequest` flows env → cache
(outer asks). The `Request : Response` *abstract relation* survives
the inversion intact.

### Inheritance of Content Ids across the boundary

A Subject's CDI is purely about that single argument (positional
seed, derived child, apply result). A value's full *Content Id* in
a particular evaluation context is its CDI composed with the
content ids of its outer scopes — and at the cb-apply boundary, the
outermost scope is the cached call's `Q`.

For a LocalObject, then:

```
ContentId(LocalObject) = CDI(LocalObject)
                       ⊕ CDI(cb_arg)
                       ⊕ CDI(Q)         ← inheritance from the cached call
```

Two cb invocations from different cached calls (different `Q`s)
have different `ContentId(LocalObject)`s, even though their
LocalObject Subjects are structurally identical
(`PositionalSeed{D}`). Their depth-2 probes carry different
`from` fields and land in disjoint regions of the depth-2 trie.

Two cb invocations from the *same* cached call with otherwise
identical depth-1 observations up to the apply have *identical*
`ContentId(LocalObject)` — and that's correct: by depth-1's input
tracing they share the same factset position, which means the
inner evaluator's state at the apply is literally the same. The
arg was passed but not yet forced; there's no information by
which the two executions can differ at that moment. Divergence
can only arise once the outer starts forcing — and the depth-2
trie captures that by content-addressed observation evolution.

Inheriting outer-scope CDIs ripples through every observation,
so atom sharing across cached calls is reduced. That's a deliberate
trade-off: storage cost in exchange for collision-free
disambiguation of LocalObject identity.

### Atom storage

`AmbientRequest` payloads share the existing `Requests` CAS pool.
`AmbientResponse` payloads live in `LocalResponseMap`, which is
keyed by `requestHash` rather than `responseHash`. That's not a
CAS pool — it's a (request → response) map, and the depth-2
walker is the only consumer.

It's sound to key by `requestHash` here because the depth-2
`reqHash` is `SHA-256(query{from = cidasks-evolved cdi})` — a
pure function of (subject, scope, prior facts in the chain). Two
recordings reaching the same `reqHash` necessarily observed the
same history; a deterministic env then produces the same
response, so (request → response) is a function and first-writer-
wins under PK = `requestHash` can't surface the wrong payload.
Depth-1 doesn't read this map at all (= walker live-dispatches
against the environment and validates structurally via factset
evolution through `Asks`/`Terminals`).

The pool also stores the LocalObject's value structure: small
atoms covering attrset entries, list elements, scalars. These get
content-addressed exactly the same way. They're what the walker
uses to reconstruct a live Nix Value tree at depth-2 replay.

**Lambda LocalObjects don't need their body stored.** A lambda's
atom is just `(localId, kind=lambda)`; the walker reconstructs it
as a primop Value whose `impl`, when applied, consults the
`AmbientAsks` trie for a recorded edge matching the live arg's
evolved content id, and either reproduces the recorded apply
result from CAS atoms or throws a depth-2 divergence signal that
the surrounding walker catches as a miss. The lambda's
"application behavior" is encoded in the recorded
`AmbientAsks` edges and `AmbientResponse` payloads, not in a
stored body. This sidesteps the question of how to serialise
arbitrary Nix expressions — there's no need.

### The trie: `AmbientAsks`

```
AmbientAsks(fromFactSetHash BLOB, requestSetHash BLOB,
            toFactSetHash   BLOB,
            PRIMARY KEY (fromFactSetHash, requestSetHash)) WITHOUT ROWID
```

No `Q`/`handle` column — depth-2 keys edges on `factSet` alone.
Inheritance discriminates *across cached calls* by folding the
cached call's identity into the scope every downstream subject's
CDI inherits at the cb-apply boundary, so every observation's
`from` (and therefore `requestHash`) is unique per cached-call
invocation from the very first probe. Discrimination *within*
one cached call's sibling cb-apply invocations is separate
(= same inherited scope, same initial seed CDI, so `from`
values start identical; siblings diverge via observation
evolution per principle 8's corollary).

`AmbientAsks` is a *validation skeleton*, not a response source.
It records which probes appeared, in what order, with what
resulting factSet transitions. It does *not* hold a
`requestHash → responseHash` index — that mapping is provided by
the environment (depth-1: files/outer evaluator; depth-2: the
outer evaluator running against the reconstructed value tree).

`RequestSetNodes` is reused for depth-2 request-set storage —
same trie machinery, members are `AmbientRequest` hashes.

### Recording (depth-2)

On the cold path, when the inner emits an `AmbientQuery` (a cb
apply), the writer enters depth-2 recording. The Env at recording
is the live inner-constructed local. As the outer's f evaluates
against it, each probe is an `AmbientRequest` with `from` =
hex of the local's current cidasks-evolved content id; the live
LocalObject reveal is the `AmbientResponse`.

Each observation's `elementHash = SHA-256(requestHash || responseHash)`
XOR-folds into the running depth-2 `factSet`. At flush, the trie
edges (`AmbientAsks(fromFactSet, {requestHash}) → toFactSet`)
are inserted; any new atom payloads land in the CAS pool. The
terminal factSet hash *is* the `AmbientResult`, which the depth-1
walker XOR-folds into its own `cur` as the `Response` for the
enclosing `AmbientQuery`.

### Replay (depth-2)

The depth-1 walker, having dispatched everything else live and
reaching the `AmbientQuery`, enters depth-2.

The walker reconstructs the LocalObject as a live Nix Value tree
from the CAS pool, keyed via `ContentId(LocalObject)` computed
live from inherited CDIs. It hands the tree to the outer as the
bridged arg and lets the outer's f run natively. The walker's
role from here is purely *observational*:

| Per outer probe | Walker does |
|---|---|
| compose `AmbientRequest` using `from = hex(currentCdi)` | hash → reqHash |
| lookup `AmbientAsks(currentFactSet, {reqHash})` | confirms this probe was recorded at this position |
| observe the value tree's reveal (the outer's evaluation of the probe) | hash → respHash |
| XOR-fold elementHash into currentFactSet | check the result matches the trie's `toFactSet` |

Both checks together: "the probe matches the recording" *and*
"the live response matches the recording." If both pass, advance.
When the outer finishes probing, the final factSet hash is the
`AmbientResult` handed up to depth-1.

The walker never serves a response from a stored index. The
reconstructed Value tree drives the outer; the outer's native
evaluation produces responses just like any Nix value. The Env
remains the source of the `request → response` mapping, exactly
as at depth-1.

### Where staleness is caught

Depth-2 has no role in stale-cache detection — that's owned
entirely by depth-1's input tracing. A changed file or
environment variable surfaces as a divergent `Response` at
depth-1 dispatch; the depth-1 walker's `(Q, factSet)` lookup
finds no Asks edge at the new factSet and falls through. The
`AmbientQuery` lives at a specific factSet position in the
depth-1 trie; if depth-1 doesn't reach that position, depth-2
is never entered.

## Implementation hints

These are not principles. They are mnemonics and guidance for
implementers — convenient ways to think about the machinery the
principles induce.

### d1 → d2 flip on no-cached-CID

At a cb-apply, the requester/responder roles invert: d1 has
inner asking outer or the system environment; d2 has outer
asking inner via the inner-supplied local. The flip is
conceptual — a mnemonic for the symmetry, not a physical
reconfiguration. When the walker encounters a cb-apply Request
whose CID has no recorded fact, it runs the outer fn live; the
d2 chain validates the probes outer makes back on the local.
Outer is never cached for serving; d2 only validates structure.

Caveat: speculatively performing an unused callback is
undesirable; mitigations postpone-able.
