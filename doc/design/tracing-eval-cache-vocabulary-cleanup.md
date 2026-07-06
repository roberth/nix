# Tracing eval-cache — vocabulary cleanup plan

Status: **plan, not yet executed**. This document proposes a canonical
vocabulary for the tracing eval cache and maps every current identifier
to its target name (or to a deletion). It is the blueprint for a
mechanical rename pass; each section can be actioned independently.

The plan preserves the model as `tracing-eval-cache.md` describes it —
that doc is the reference for *what the cache does*. This doc is
about *how we name what the cache does*, and about killing the
synonyms and iteration debris that accumulated between v12 and the
converged v13.

Sanity rules the plan follows:

1. **One term per concept.** Any surviving synonym must earn its
   distinct role.
2. **Kill implicit versioning in identifiers.** `v13` and `v12` are eras,
   not attributes of live data. They belong in docs, not in symbol names.
3. **Kill iteration-log language in code.** `iter N`, `Path 3`,
   bare `converged` as an unqualified noun, `trie-greedy`,
   `refuted` are archaeology; move them to the design-doc history
   or delete. Note: "converged fold" as a specific named mechanism
   (see §4.6) stays — the archaeology is the *bare* `converged`
   used as if it were a variable or code path.
4. **Prefer named layers over depth numbers.** `d=1` / `d=2` become
   `env` / `ambient` (see §2). `d=0` stays implicit — it's just "the query".
5. **Match implementation vocabulary to the design doc, not the other
   way around.** The design doc is the authority for *Query/Result*,
   *Request/Response*, *Fact/FactSet*, *RequestSet*, *Asks/Terminals*.
6. **Identifiers vs. characterizations.** Two distinct roles
   previously blurred:

   - An **identifier** is a stable name for a *thing*. The thing
     may vary across observations, walks, ancestry, or invocations
     — many instances of "the second curried arg" exist across
     callback invocations — but the identifier is the same.
     Identifiers get `Id` when hashed (`argIdHash`), or keep their
     structural type name when algebraic (a `Subject` value is an
     `argId` in algebraic form).
   - A **characterization** is a key for a specific *state* or
     *payload*. Same identifier + different state → different
     characterization. Characterizations can be represented as:
     - **data** — `walk`, `observations`, `FactSet` members,
       `envAsksEdges` — natural type names, no suffix rule.
     - **hashes** — `stateHash` (subject's evolving state at k),
       `factSetHash` (XOR-fold of a FactSet's members),
       `argAncestry` (ancestral chain's boundary state),
       `queryHash` (a specific query payload). Use `Hash` (with
       a semantic modifier where precision helps) for the hash
       representation.

   The rule about `Id` is a stability promise: any `*Id` name must
   be provably invariant under observations, walks, ancestry, and
   invocation. If a `*Id` name currently identifies something that
   depends on those, that's a bug to fix, not a rename choice.
   Nothing analogous is enforced on `Hash` — a hash's value tracks
   whatever it's a hash of.

---

## 1. Model recap in one page

The cache sits between the *caller* (an `Evaluator`-consuming CLI or
another `Evaluator`) and the *environment* (source files, env vars,
and — for `builtins.cache` — a live outer evaluator's values).

Two boundaries, three atomic conversations:

| Layer | Direction | Atomic pair | Where it comes from |
|-------|-----------|-------------|---------------------|
| **Query** | caller ↔ evaluator | `Query` → `Result` | `evalFile`, `getAttr`, `getString`, `apply` |
| **Env** | evaluator ↔ environment | `Request` → `Response` | file reads, `getEnv`, ambient-value probes |
| **Ambient** | outer-evaluator ↔ inner-supplied local | `Request` → `Response` | outer's probes on an argument that inner (a cached call) handed it during a covariant callback |

A **Fact** is one env-layer `(Request, Response)`. A recording maps a
Query onto a chain of Facts (its trace) and a Result at the end.
On replay, the cache walks the chain against the live environment;
matching Responses land it at the recorded Result.

Everything else — the Patricia split, the RequestSet trie, subject
identities (see §3.5), Ambient Asks, `builtins.cache` — is machinery
in service of those three tables:

```
Requests, Queries, Results, LocalResponses          -- content-addressed atoms
RequestSetNodes                                     -- trie of RequestSets
Asks(Q, factSet)          -> requestSet             -- env-layer walk edge
Terminals(Q, factSet)     -> result                 -- env-layer chain end
AmbientAsks(fromFactSet)  -> requestSet, toFactSet  -- ambient-layer walk edge
SubjectEvolutionEdges                               -- subject-id fast-path
```

## 2. Layer names

The single biggest source of confusion is that "depth 1" / "depth 2"
and the fn/arg pair "outer" / "inner" and the evaluator-stack pair
"outer" / "inner" all coexist. We rename to fix that.

**Rule:** *layer* is the boundary; *side* is which participant we're
talking about across a layer; *role* is fn-vs-arg (never "outer/inner").

| Old | New | Rationale |
|-----|-----|-----------|
| depth-1 / d=1 / d1 | **env** | The environment layer. |
| depth-2 / d=2 / d2 | **ambient** | The layer that lives *inside* a cb-apply, where the outer evaluator observes a value the inner (a cached call) supplied. Already the term used for `AmbientAsks`; extend it everywhere. |
| depth-0 / d=0 | (implicit) | It's just "the Query". No prefix needed. |
| outer / inner (evaluator stack) | **outer** / **inner** | Retain — but only for the *evaluator stack*. |
| outer (as in "outer arg", "outer's g 5") | **caller** or explicit *fn* / *arg* | The evaluator-stack meaning wins the word "outer". Use "caller side" or the fn/arg role explicitly when talking about function application. |
| covariant callback | **callback** or **cb-apply** | Keep "cb-apply" as the noun; the "covariant" qualifier is a footnote for the design doc, not for identifiers. |
| cb-arg | **callback arg** | Small change; just spell it. |

## 3. The naming spine

These are the canonical terms. Everything else in the code is either
one of these or a helper name that should not compete with them.

### 3.1 Atoms (content-addressed by SHA-256 of payload)

| Term | Meaning |
|------|---------|
| `Query` | An operation the caller asks (with parameters + parent's `queryHash`). |
| `Result` | The value the evaluator returns for a Query. |
| `Request` | An operation the evaluator asks the environment (or, at the ambient layer, an operation the outer asks against an inner-supplied local). |
| `Response` | The environment's reply to a Request. |

### 3.2 Sets

| Term | Meaning |
|------|---------|
| `Fact` | One `(Request, Response)` pair. |
| `FactSet` | A set of Facts. Identified by an XOR-fold hash. Members are recomputed, never persisted. |
| `RequestSet` | A set of Requests. Identified by a canonical CBOR-sorted-Merkle hash and stored as a Patricia trie in `RequestSetNodes`. |

### 3.3 Edges

| Term | Meaning |
|------|---------|
| `Asks edge` | `Asks(queryHash, factSetHash) -> requestSetHash`. The env layer's walk edge. |
| `Terminal` | `Terminals(queryHash, factSetHash) -> resultHash`. Chain end. |
| `Ambient Asks edge` | `AmbientAsks(fromFactSetHash) -> (requestSetHash, toFactSetHash)`. The ambient layer's walk edge, keyed on factSet only (Q-independent). |
| `Subject evolution edge` | `SubjectEvolutionEdges(subject, cur, obs.from, obs.elem) -> nextCur`. Subject-id fast-path for a subject's evolution under one observation. |

### 3.4 Walker state

| Term | Meaning |
|------|---------|
| `cur` | The walker's running factSet hash at the env layer. Bare "cur" always means this. |
| `nextCur` | `cur` after XOR-folding one edge's Facts in. |
| `startCur` | The `cur` the walk starts at (defaults to `∅`). |
| `terminalCur` | The `cur` the walk lands at when committing a Terminal. |

For subject identity, `cur` is overloaded (see §4.1). We fix that.

### 3.5 Subject identity and state characterization

The most-tangled part of the vocabulary. The module currently called
`cidasks` maintains two distinct notions per rule 6: **identifiers**
(stable structural names for callback args and derived values) and
**characterizations** (hash-form keys for a specific state at a
specific walk position). The current names conflate them, letting
`scope` and `state id` both pretend to be identifiers when they're
actually characterizations of state.

**Namespace flatten.** Drop `nix::cidasks`; flatten into top-level
`nix::`. libexpr doesn't otherwise carve per-topic namespaces, and
none of the moved names (`Subject` and its variants, `Observation`,
`Edge`, `EvolutionStep`, `ApplyContext`, `PathAndRoots`) collide at
`nix::` scope. The file is still renamed:
`content-identity-via-asks.{hh,cc}` → **`subject-id.{hh,cc}`**. The
acronym `cidasks` disappears entirely.

**Doc-level vocabulary.** Drop "content-defined identity" / "CDI" /
"content identity" — they collapsed together and treated ancestry
as if it produced stable identity, which it doesn't. Replace with
**state hash** for the evolving family and **id** for the truly
stable structural identity.

**The `scope` parameter is renamed.** It's not a lexical scope in
the ordinary sense — `let` bindings and other lexical constructs
don't cross the cache boundary. Only *callback arguments* do. The
value is the XOR-fold of the enclosing chain of callback args'
state hashes at boundary entry — a characterization of the
ancestral chain's state at that moment. Rename to **`argAncestry`**
(parameter, field, prose). It characterizes ancestry; it is not
itself an identifier.

**Concept dictionary:**

| Term | Type | Stability | Meaning |
|------|------|-----------|---------|
| `Subject` | algebraic | stable | Structural name for a value inside a cache-boundary callback's body. Four variants: `PositionalSeed`, `DerivedSubject`, `ApplyResultSubject`, `PostulatedIdempotentRead`. Not a hash — a tree. |
| **`argId`** | `Subject` or `Hash` | stable | The truly-stable identity of a callback argument, independent of ancestry, walk, and invocation. Two representations of the same identity: the `Subject` value (e.g. `Subject{PositionalSeed{1}}`) and its atomic hash (`sha256("positional-1")`, currently spelled `selfHash`). Both are `argId`; disambiguate with `argIdHash` only when the type context requires it. |
| **`argAncestry`** | `Hash` | situational | XOR-fold of the enclosing chain of callback args' state hashes at boundary entry. A state hash itself — its value reflects outer observations already made. Formerly `scope`. |
| **state hash** (`stateHashAt(argId, argAncestry, walk, k)`) | `Hash` | situational | The evolved state of an arg-level subject at walk position k. Combines `argId`, `argAncestry`, and observations from walk[0..k]. Stamped as the `from` field on the next observation this subject emits. Traps on `DerivedSubject`. Formerly `scopeStateIdAt`. |
| **`stateHashAfter`** | `Hash` | situational | `stateHashAt` at `k = walk.size()`. |
| **`stateHashConverged`** | `Hash` | situational | Grouping-invariant fixed point — depends only on the set of observations, not on how they were partitioned into edges. |
| **`subjectHashAt`** | `Hash` | situational | Mixed-concept dispatch accessor, not a coherent notion. For arg-level Subjects it returns a state hash — identifying the subject's own evolving state at k. For `DerivedSubject` it returns the producer QueryGetAttr's hash — identifying a Queries-pool payload (which happens to embed the parent's state hash in its `from` field). Two different things being identified, both by content-derived hashes; unification is a caller-convenience shortcut. Formerly `structuralAddress`. Kept for the rename pass; see §5.3 for the follow-up split into `stateHashAt` (arg-level) and `producerQueryHashAt` (derived). |

**No such thing as a "state hash at entry".** Do not invent a name
for `stateHashAt(argId, argAncestry, {}, 0)`. It's already a state
hash — its distinguishing power at k=0 depends entirely on whether
observations have already flowed into argAncestry via outer state.
Two structurally-identical cb-apply invocations with the same
outer state produce the same value here; they only diverge through
subsequent observations. A "stable within invocation" claim is
wrong at k=0 because the invocation itself doesn't produce identity
— observations do.

**Why not a distinct "structural identity" as a public term.** For
arg-level subjects the universal accessor (formerly
`structuralAddress`, now `subjectHashAt`) collapses to
`stateHashAt` — they return the same hash on the common path. The
internal "structural" step (subject shape + argAncestry +
constituents' evolution at k, *before* this subject's own
observations fold in) is a lambda inside `stateHashAt`'s
implementation, not exposed. Naming an external function
`structuralIdAt` invites the false inference that it computes that
internal step; it doesn't.

**Walker-side running value.** The per-subject running state as
observations fold in is a **`stateHash`** (or `subjectStateHash`
if multiple subjects' running states live in one scope). Never
`subjectId` — the value evolves.

## 4. Renaming table

Each row is `old → new`. Grouped by topic. Rows tagged in the Notes
column: **[type]** struct/class, **[member]** class field,
**[method]** method or free function, **[param]** function parameter,
**[local]** local variable, **[comment]** appears only in comments.

### 4.1 Env-layer walk vs subject-identity walk

The confusion: the writer keeps two "walks" going in parallel — the
`Asks`-edge chain and the subject-id fold-step chain — and both use `cur`.

| Old | New | Notes |
|-----|-----|-------|
| `d1CidasksWalk` | `envWalk` | [member] `std::vector<Edge>` on `TracingWriter`. Aligned 1:1 with `envAsksEdges` (below). |
| `perQAsksEdges` | `envAsksEdges` | [member] Boundary log of finalized Asks edges for the current Query. |
| `lastQFactsHash` | `envCur` | [member] The env-layer `cur` after the last successful walk. "Facts hash" is imprecise — this is a factSet hash, and it's the running env `cur`. |
| `dispatchedTrie` | `envDispatchedTrie` | [member] Session-cumulative trie of dispatched Requests. Distinguishing from writer-side `allRequestsTrie`. |
| `dispatchCache` | `responseFor` | [member] Match the writer's naming, which already calls the request→response map `responseFor`. |
| `allRequestsTrie` | `sessionRequestsTrie` | [member] Writer's session-cumulative trie. "All requests" is ambiguous — this is all requests *this writer has seen*, not "all recorded". |
| `allRequestsRsHash` | (parameter renamed inline) `sessionRequestsRsHash` | [param] Match. |
| `curRequests` | `dispatchedSoFar` | [local] The set of Requests already consumed by the walk. "Cur requests" hides what's meant. |
| `remaining` | (keep) | [local] |
| `useful` / `usefulDispatch` | (keep) | Already a good term. |
| **cidasks `cur`** (subject-identity context) | `stateHash` (or `subjectStateHash` if disambiguation needed) | Only inside `subject-id.cc` code paths and subject-evolution fast-path. Global `cur` remains the env-layer factSet hash. |

### 4.2 Recording

| Old | New | Notes |
|-----|-----|-------|
| `v13FactSet` | `envFactSet` | [member] The writer's monotonically-growing set of Facts (env layer). |
| `v13FactSetHash` | `envFactSetHash` | [member] Its XOR-fold hash. |
| `v13Walk` | `walk` (on `TracingReplayEvaluator`) | [method] Just `walk`. If a name clash with `TracingDecisionGraph::walk` is a problem, use `walkReplay` — but even that is unnecessary in-class. |
| `logResponse` | (keep) | Good term, matches design doc. |
| `logResult` | (keep) | Good. |
| `logQuery` | (keep) | Good. |
| `logDepth2Observation` | `logAmbientObservation` | [method] Layer rename per §2. |
| `logDepth2ApplyFact` | `logAmbientApplyFact` | [method] |
| `logAmbientInteraction` | (keep) | Good. |
| `noteEnvObservation` | (keep) | Distinct from `logResponse`: `logResponse` records during interpretation and inserts into the pool; `noteEnvObservation` runs during warm replay to keep the writer's in-process state consistent with the walker's dispatched facts (no pool insertion). Different sides of the recording/replay boundary — the naming distinction is load-bearing. |
| `record()` / `record(Q, factSet, R, responseFor, allRequests, allRequestsRsHash)` | (keep) | Design-doc canonical. |
| `primeFactSetCache` | `installFactSet` | [method] Verb "prime" is jargon. What it does is install a members list under a caller-supplied hash. |
| `flushPendingAmbient` | `flushAmbient` | [method] "Pending" is redundant — nothing else in the writer is being flushed. |
| `splitFlush` | `closeAsksEdge` | [method] The operation is "close the current Asks edge boundary and flush the ambient buffer that belongs to it". |
| `markApplyBoundary` | `openApplyBoundary` | [method] Symmetric with `closeAsksEdge`; makes the pair readable. |
| `PendingApplyBoundary` | `ApplyBoundary` | [type] Nested in `TracingWriter`. It stops being "pending" once the boundary opens; keep the state field separately. |
| `PerQAsksEdge` | `AsksEdgeRecord` | [type] Nested. "Per-Q" is redundant — the whole struct is per-query already because it lives on `TracingWriter` per session. |
| `SuppressApplyBoundary` | (keep) | Clear. |
| `TriePosition` | (keep) | Clear. |

### 4.3 Replay-object hierarchy

| Old | New | Notes |
|-----|-----|-------|
| `TracingObject` | (keep) | Writer-side Object wrapper — logs to `TracingWriter`. |
| `TracingReplayObject` | (keep) | Replay-side Object wrapper — dispatches through `walk()`. |
| `TracingLocalObject` | `TracingCallbackArg` | [type] Wraps the inner-supplied value at a cb-apply boundary; records the outer's probes on it. The current name is technically accurate ("local of the inner"), but "callback arg" says *what* the local is. **Do not rename to anything with "Ambient" in it — the object is not ambient; only the probes coming at it are.** |
| `ReplayLocalObject` | `ReplayCallbackArg` | [type] Replay counterpart. Frozen image reconstructed from `LocalResponseMap`. |
| `AmbientObject` | (keep) | Outer's proxy for an inner-supplied value in the callback body. Genuinely ambient-side. |
| `LambdaApplyResultObject` | `TracingCallbackApplyResult` | [type] Records the outer's probes on the *result* of applying a fn to a callback arg. Symmetric with `TracingCallbackArg`. "Lambda" is misleading — it wraps any callable. |
| `ensureInner` / "activate inner" | `ensureInner` | [method] Keep one; delete the "activate" phrasing from comments. |
| `defeatCache` | (keep) | Descriptive. |

### 4.4 Subject-identity vocabulary (formerly `cidasks`)

Namespace flattened into `nix::` (see §3.5). Types keep their bare
names; parameter and function names are reworked to honour the
stable/situational discipline of rule 6. `scope` (as a parameter,
field, and prose term) becomes `argAncestry` everywhere.

**File rename:**

| Old | New | Notes |
|-----|-----|-------|
| `content-identity-via-asks.hh` / `.cc` | `subject-id.hh` / `.cc` | Filename reflects the topic. |
| Doc phrase "content-defined identity" / "CDI" / "content identity" | "state hash" (situational) / "id" (stable) | Old family of near-synonyms treated ancestry as if it produced stability; new pair enforces the semantic distinction. |

**Types (namespace dropped; names unchanged unless noted):**

| Old | New | Notes |
|-----|-----|-------|
| `cidasks::Subject`, `PositionalSeed`, `DerivedSubject`, `ApplyResultSubject`, `PostulatedIdempotentRead` | `nix::Subject` and variants (names kept for the mechanical pass) | Under the discipline, a `Subject` value IS an `argId` (algebraic form). The variants are all stable identifiers and would earn `Id` suffixes (`PositionalArgId`, `DerivedId`, `ApplyResultId`, `IdempotentSourceId`) — plus `Subject` → `Id` for the sum type — under a strict rule-6 application. That rename is a broader ripple deferred to §5.3; the mechanical pass keeps the old variant names. |
| `cidasks::Observation` | `nix::Observation` | |
| `cidasks::Edge` | `nix::Edge` | Different from the env-layer `Asks edge` — that's a domain concept, not a C++ type. When docs mention an "edge" and ambiguity arises, qualify as "Asks edge" or "subject-id edge" in prose. |
| `cidasks::EvolutionStep` | `nix::EvolutionStep` | |
| `cidasks::PathAndRoots` | `nix::PathAndRoots` | (Note: `PathExpr` and `PathStep` are already in `nix::trace::` and stay there — `cidasks` only *uses* them.) |
| `cidasks::ApplyContext` | `nix::ApplyContext` | Kept as-is: `Apply` is genuinely descriptive (the context holds a cb-apply's worth of observations). |

**Parameter rename (applies across all functions and callers):**

| Old | New | Notes |
|-----|-----|-------|
| `scope` (parameter, field, prose) | `argAncestry` | The value is the XOR-fold of enclosing callback args' state hashes at boundary entry. "Scope" evokes lexical scope, but this only captures ancestry of args — no `let`-bound or otherwise lexically-in-scope non-args. And the value is itself a state hash, not a stable id. |

**Functions (namespace dropped; renamed per §3.5):**

| Old | New | Notes |
|-----|-----|-------|
| `scopeStateIdAt(subject, scope, walk, k)` | `stateHashAt(argId, argAncestry, walk, k)` | The value is a running state hash, not a stable id. Applies only to arg-level subjects; traps on `DerivedSubject`. |
| `scopeStateIdAfter(subject, scope, walk)` | `stateHashAfter(argId, argAncestry, walk)` | Convenience at `k = walk.size()`. |
| `scopeStateIdAtConverged(...)` | `stateHashConverged(argId, argAncestry, observations)` | Drops `At` — the converged variant has no walk index by definition. |
| `scopeStateIdAtWithHook(...)` | `stateHashAtStamping(...)` | The hook stamps subject-evolution rows; the name should say what it does. |
| `structuralAddress(subject, scope, walk, k)` | `subjectHashAt(id, argAncestry, walk, k)` | Neutral name. The function dispatches on subject variant: arg-level → the subject's state hash at k; `DerivedSubject` → the producer QueryGetAttr's hash (a Queries-pool payload's key). Both are content-derived hashes identifying something, but they identify different kinds of thing — a subject state vs. a query payload. Unification is a caller-convenience shortcut. See §5.3 for the follow-up split. |
| `structuralAddressAfter(subject, scope, walk)` | `subjectHashAfter(id, argAncestry, walk)` | Convenience at `k = walk.size()`. External callers use only this variant. |
| `extractFrom(query)` | `fromStateHashOf(query)` | Returns the `from` state hash stamped on a query payload; name what it returns and what it is. |

**Adjacent renames (callers of these APIs):**

| Old | New | Notes |
|-----|-----|-------|
| `selfHash` / `subjectSelfHash` / `resultSelfHash` (locals) | `argIdHash` (or `argId` if the type is unambiguous in scope) | These locals hold `stateHashAt(argId, 0, {}, 0)` = the atomic hash of the Subject alone with no argAncestry. That value IS a truly-stable `argId` in `Hash` form. Rename accordingly; drop the `self*` prefix. |
| `argSubject`, `argSubj`, `seedSubject` (locals holding a `Subject`) | `argId` | Under the discipline, a `Subject` value is the algebraic form of an `argId`. |
| `argStateId` (in comments / doc) | `stateHash` | Same concept; kill the synonym. |
| `argId` where it actually means `stateHashAfter(argId, argAncestry, {})` (e.g. `expr-from-object.cc:300`) | `stateHash` (rename the local) or inline the computation | The former use of `argId` here was a false-stability claim: the value depends on `argAncestry` and so is already a state hash, even with an empty walk. Not a distinct concept; do not invent an "at-entry" name. |
| `structuralAddress` in comments outside the module | `subjectHash` | |
| `inheritedScope` (field on `PendingFact` etc.) | `argAncestry` | The concept is unified across the codebase now. |
| `applyScope` (cidasks helper — combines fn+arg argAncestries) | `applyArgAncestry` | Non-commutative combinator producing the argAncestry inside an apply-result. Parameters `fnScope`/`argScope` become `fnArgAncestry`/`argArgAncestry`. |
| `callScope` (field on `AmbientResolver` + locals) | `callArgAncestry` | Hash-typed field naming the cache call's own argAncestry. |
| `ArgScopeCell` (type in `arg-scope.hh`) | `ArgCell` | **Not an argAncestry** — a navigation cell carrying `(depth, parent, liveObject)` for walking the proxy chain. The word "scope" in the old name meant proxy-chain position, not cidasks-scope; keeping it would leave a landmine after `scope` → `argAncestry` sweeps. File `arg-scope.hh` → `arg-cell.hh`. |
| `argScope` field (`std::shared_ptr<const ArgScopeCell>` type) | `argCell` | Field rename mirrors the type. |
| `effectiveArgScope()` / `getProxyArgScope()` (free function / virtual method) | `effectiveArgCell()` / `getProxyArgCell()` | Same reason. |

### 4.5 Subject-evolution fast-path (formerly "Path-3")

The mechanism ships; the *name* "Path-3" is doc-history scaffolding
and should not appear in code identifiers.

| Old | New | Notes |
|-----|-----|-------|
| `SubjectEvolutionEdges` | (keep) — table name is fine | It literally records subject evolutions. |
| `insertSubjectEvolutionEdge` | (keep) | Method is well-named. |
| `getSubjectEvolutionEdge` | (keep) | |
| Comments referencing "Path 3" | Delete or replace with "subject-evolution fast-path" | [comment] Path 3 is a doc-history term; not visible in code. |
| Path-1/Path-2 references | Delete | These were refuted approaches; if their history is worth preserving, keep only in the archived split of `search-to-asks.md` (see §5.2). |

### 4.6 Fallback: converged / trie-greedy

The three-branch walker (initial K=0 + subject-evolution fast-path +
converged fold) is load-bearing and stays. The *names* need cleanup.

| Old | New | Notes |
|-----|-----|-------|
| `converged` (used as noun in code) | `groupingInvariantFold` (or just "converged fold" as a fixed phrase in comments) | The mechanism is: fold observations to a fixed point regardless of how they were grouped into edges. |
| `trie-greedy` | Delete from code. | The name refers to a specific historical alternative; the code has picked converged fold + subject-evolution fast-path. If both survive as branches, name them explicitly. |
| Iteration-tagged probes (`iter 61 probe`, `iter 108`, etc.) in code comments | Delete. If the comment is load-bearing ("we tried X; it produced Y regressions"), rewrite it without the iteration number. |

### 4.7 `builtins.cache` primop

| Old | New | Notes |
|-----|-----|-------|
| `<cached-fn>` / `makeCachedFnPrimOp` | (keep) | Names of the runtime primop and its factory. |
| `<ambient-fn>` / `makeAmbientFnPrimOp` | (keep) | |
| `<replay-local-lambda>` | `<replay-callback-arg-lambda>` | Match §4.3. |
| `AmbientResolver` | (keep) | |
| `AmbientQueryFn`, `AmbientApplyFn` | (keep) | |
| "standin" (in comments) | Replace with "replay object" everywhere. | `ReplayCallbackArg` *is* the standin; no need for a second word. |
| "proxy" (in comments, meaning AmbientObject) | Replace with "AmbientObject" | Same reason. |
| `seed:` / `local:` / `apply:` string prefixes | (keep) | Wire format. |
| `virtual-root ID`, `virtual value` | (keep) | Distinct concept: an ID assigned to a value that isn't a recorded Object. |
| "outer's `g 5`" / "outer body" (in docs, meaning the fn side of a cb-apply) | Rewrite as "the callback fn" / "the callback body" | Frees "outer" for the evaluator-stack sense (§2). |

### 4.8 Sink / writer / db

| Old | New | Notes |
|-----|-----|-------|
| `TracingWriter` | (keep) | |
| `TraceSink` | (keep) | |
| `TraceFile` (JSON) | (keep) | |
| `TracingDecisionGraph` | (keep) | Design-doc canonical. |
| `trace-file.hh` | Merge into `trace-sink.hh` OR clarify — the current `TracingDatabase` typedef inside is obsolete. | Grep confirms `TracingDatabase` is dead weight. |
| `log*` methods | (keep, sink layer) | `log*` = "emit to JSON sink; may also index". Consistent. |
| `record()` on `TracingDecisionGraph` | (keep) | Different semantic — index insertion. |
| `insert*` on `TracingDecisionGraph` | (keep) | SQL-layer verbs. Consistent. |

### 4.9 v13 / v12 stripping

Rules:

1. In *identifiers*, delete `v13` / `v12` prefixes and suffixes. The
   cache has one live version; the name doesn't need to say so.
2. In *file names*, delete `v13` prefixes on shell scripts under
   `tests/perf/tracing-cache/`. Audit confirmed no scripts test a
   v12 baseline — all three (`v13-smoke.sh`, `v13-complex-workload.sh`,
   `v13-hit-rate.sh`) target the shipped cache and get renamed.
3. In *docs*, `v13` remains: it identifies the era. Same for the
   `outdated/` docs, which are specifically about earlier versions.

Identifier renames are covered in §4.1–§4.2 (`v13FactSet` →
`envFactSet`, `v13FactSetHash` → `envFactSetHash`, `v13Walk` →
`walk`). Comments: delete or move to design docs.

Test files: `tests/perf/tracing-cache/v13-*.sh` → drop `v13-`
prefix. Audit is done: no v12 baselines exist.

## 5. Deprecations and deletions

### 5.1 Delete now (confirmed dead)

- `doc/outdated/next-session-prompt-cb-sibling-b-v2.md`
- `doc/outdated/next-session-prompt-cb-sibling-b-v3.md`
  Both are session-prompt scaffolding, not referenced anywhere.
- `trace-file.hh`'s `TracingDatabase` typedef — audit shows unused.
- Any remaining `iter N` markers in code comments (the design-doc
  archaeology captures this).
- References in code to "Fix A" / "Fix B" / "supplementary-objects
  stack" / "offline AmbientResult walk" — all reverted mechanisms.
- Comment references to a "depth-2 divergence signal" as if it were a
  type — it's exception-based flow; describe it as such.

### 5.2 Audit before deleting

- **All `doc/outdated/*.md` files** — audit confirmed: no live code
  references. Only `doc/design/tracing-eval-cache.md` cross-links
  them as historical context; that reference gets rewritten to
  point at the archived history section (or simply dropped) as part
  of step 8. Then delete the outdated dir contents.
- The `search-to-asks.md` design doc — split it in step 9:
  extract the shipped design (converged fold + subject-evolution
  fast-path) as `doc/design/tracing-eval-cache-subject-id.md`;
  move the iteration archaeology to `doc/outdated/` (or delete).
  The split's granularity is a doc-editorial call at step-9 time
  and doesn't block code renames.
- `tracing-cache-stats.hh` — audit done: references `v13Walk` in
  comments (lines 12–13). Updated as part of step 2's `v13Walk`
  → `walk` sweep; no separate metric-name rename needed.

### 5.3 Consider collapsing

These may or may not simplify to one type; call it out for the cleanup
session to judge, not decide up front:

- `TracingObject` vs `TracingReplayObject`. The design decision was
  clean separation. If a mode flag would collapse them cleanly, do
  it; if not, leave them as siblings.
- `Query`-payload types in `trace-types.hh` — 851 lines of variants.
  Some (`QueryGetString`, `QueryGetStringWithContext`) may be
  collapsible; only worth doing after the rename pass so their new
  names read consistently.
- `CoarseEvalCacheCursorObject` → `CoarseEvalCacheObject`. Out of
  scope for the tracing-cache cleanup — the coarse cache is
  orthogonal — but the "Cursor" suffix is a vestige of the older
  eval-cache's `AttrCursor` and is misleading. Fix opportunistically
  or in a separate pass.
- **Full rule-6 application to Subject and its variants.** `Subject`
  → `Id` (the sum type is precisely an identifier); variants gain
  `Id` suffixes: `PositionalSeed` → `PositionalArgId`,
  `DerivedSubject` → `DerivedId`, `ApplyResultSubject` →
  `ApplyResultId`, `PostulatedIdempotentRead` →
  `IdempotentSourceId`. All four variants are stable structural
  identifiers, so the rename is semantically clean. It's deferred
  from the mechanical pass because the ripple is large — every
  `Subject` type reference, every variant construction site, plus
  the "seed" prose in `expr-from-object.cc` and elsewhere. Also
  intersects with the `seed:` wire-format prefix (kept per §8),
  which needs prose-level care.
- **Split `subjectHashAt` into two functions.** Currently a mixed-
  concept dispatch: for arg-level Subjects it returns the subject's
  own state hash at k; for `DerivedSubject` it returns the producer
  QueryGetAttr's hash (which is that payload's Queries-pool key).
  Both are content-derived hashes, but they identify different
  kinds of thing — subject state vs. query payload — papered over
  as one Hash. Follow-up should expose them
  separately as `stateHashAt(argLevelSubject, argAncestry, walk, k)`
  (traps on Derived) and `producerQueryHashAt(derivedSubject,
  argAncestry, walk, k)`, then delete `subjectHashAt`. Requires
  a per-callsite audit: every external caller (`AmbientObject`,
  `ReplayLocalObject`, `TracingObject`, and any others) needs
  enough context in scope to dispatch on subject variant. Not a
  mechanical rename — a small refactor with real risk if a caller
  currently relies on the unified dispatch and doesn't have that
  context available.

## 6. Migration order

Renames are staged as numbered steps. Each numbered step is a
**checkpoint**: mandatory build + full test-suite run before
moving on. Within a step, commits are as fine-grained as
compilation allows (typically one identifier per commit; rarely,
several identifiers move together because a struct rename requires
its field renames to land atomically).

Step 0 exists to catch pre-existing issues before they get blamed
on a rename.

**Step 0. Baseline.** Confirm current tip builds and passes tests.
`meson compile -C build && meson test -C build` green. No commits.
If red, fix or investigate before starting.

**Step 1. Layer rename (§2).** *Specific* identifier renames only —
`d1CidasksWalk` → `envWalk`, `logDepth2Observation` →
`logAmbientObservation`, `logDepth2ApplyFact` → `logAmbientApplyFact`.
Comment/prose updates for "d=1"/"d=2" → "env"/"ambient" happen in
step 7 (comment cleanup pass). One commit per identifier.
*Checkpoint.*

**Step 2. `v13` → unversioned (§4.9).** Identifier renames
(`v13FactSet`, `v13FactSetHash`, `v13Walk`) plus shell-script
rename for all three `tests/perf/tracing-cache/v13-*.sh` files
(no v12 baselines exist — confirmed). Also updates `v13Walk`
references in `tracing-cache-stats.hh` comments. One commit per
identifier / file. *Checkpoint.*

**Step 3. Writer field renames (§4.2).** `perQAsksEdges` →
`envAsksEdges`, `lastQFactsHash` → `envCur`, `dispatchedTrie` →
`envDispatchedTrie`, `dispatchCache` → `responseFor`,
`allRequestsTrie` → `sessionRequestsTrie`, `curRequests` →
`dispatchedSoFar`. One commit per field. *Checkpoint after
each; full test-suite checkpoint at end of step.*

**Step 4. Writer method + nested-type renames (§4.2).**
`flushPendingAmbient` → `flushAmbient`, `splitFlush` →
`closeAsksEdge`, `markApplyBoundary` → `openApplyBoundary`,
`primeFactSetCache` → `installFactSet`, `PendingApplyBoundary`
→ `ApplyBoundary`, `PerQAsksEdge` → `AsksEdgeRecord`. One
commit per identifier. *Checkpoint.*

**Step 5. Object-type renames (§4.3).** `TracingLocalObject` →
`TracingCallbackArg`, `ReplayLocalObject` → `ReplayCallbackArg`,
`LambdaApplyResultObject` → `TracingCallbackApplyResult`. Each
type rename is a single atomic commit (file rename +
constructor / member updates). *Checkpoint after each commit;
full test-suite at end.*

**Step 6. Subject-identity vocabulary (§4.4).** Three sub-steps,
each with its own checkpoint:

- **6a. Namespace flatten + file rename.** `nix::cidasks::` →
  `nix::`; `content-identity-via-asks.{hh,cc}` →
  `subject-id.{hh,cc}`. `#include` updates. Single atomic commit.
  *Checkpoint.*
- **6b. `scope` and scope-family compounds → argAncestry / argCell
  families.** Mixed Green + Red. Sub-order chosen so the
  navigation-cell rename lands first (removes the noun-collision
  landmine before we sweep the cidasks-scope):
  1. **Navigation family (Green).** `ArgScopeCell` → `ArgCell`
     (type); `arg-scope.hh` → `arg-cell.hh` (file); `argScope`
     field on `TracingObject` and elsewhere → `argCell`;
     `effectiveArgScope()` → `effectiveArgCell()`;
     `getProxyArgScope()` → `getProxyArgCell()`. Word-boundary
     safe once the type is renamed. Commit atomically per
     rename target.
  2. **Cidasks-scope hash family (Green).** `inheritedScope`
     field → `argAncestry`. `applyScope` (cidasks helper) →
     `applyArgAncestry`, with parameters `fnScope`/`argScope`
     → `fnArgAncestry`/`argArgAncestry`. `callScope` field on
     `AmbientResolver` and locals → `callArgAncestry`. One
     commit per identifier.
  3. **Bare `scope` in `subject-id.{hh,cc}` (Green).** After
     the navigation family is renamed, remaining `scope`
     parameters and locals inside cidasks are unambiguously the
     hash-typed cidasks-scope. Word-boundary `sed` inside the
     module is now safe. Commit once for the module.
  4. **Bare `scope` locals in callers (Red).** Per-site editor
     pass on `tracing-object.cc`, `tracing-replay-object.cc`,
     `tracing-evaluator.cc`, `replay-local-object.cc`,
     `expr-from-object.cc`, `ambient-object.cc`. Each `scope`
     variable is either a cidasks-scope (rename) or a
     C++/lexical usage in a comment (leave). Read every site.
     Commit per file.
  *Checkpoint after each sub-item; full-suite at end of step.*
- **6c. Function renames.** `scopeStateIdAt` → `stateHashAt`,
  `scopeStateIdAfter` → `stateHashAfter`,
  `scopeStateIdAtConverged` → `stateHashConverged`,
  `scopeStateIdAtWithHook` → `stateHashAtStamping`,
  `structuralAddress` → `subjectHashAt`,
  `structuralAddressAfter` → `subjectHashAfter`,
  `extractFrom` → `fromStateHashOf`. Plus caller-side locals
  (`selfHash`/`subjectSelfHash` → `argIdHash`, `argSubject`
  /`seedSubject` → `argId`, `argId`-that-means-a-state-hash
  → `stateHash`). One commit per function; locals batched
  by file. *Checkpoint.*

**Step 7. Comment cleanup (§4.5, §4.6, §5.1, §7.2).** The Red-
category prose pass. Read every touched comment across the
codebase; delete Path-1/2/3, `iter N`, reverted-mechanism, and
"depth-2 divergence signal"-as-type references; rewrite sentences
whose logic doesn't survive the renames. Also handles "d=1"/"d=2"
→ "env"/"ambient" prose transitions deferred from step 1. No
identifier changes. *Checkpoint.*

**Step 8. Dead-file deletion (§5.1, §5.2).** Delete
`TracingDatabase` typedef and all `doc/outdated/*.md` files;
update the one reference to them in
`doc/design/tracing-eval-cache.md`. Single commit per file (or
one commit for the outdated dir en masse). *Checkpoint.*

**Step 9. Split `search-to-asks.md`.** Extract the shipped design;
archive or delete the iteration log. Doc-only, single commit.
*Checkpoint (build only — no code change).*

**Step 10. Optional: collapse consideration (§5.3).** Not
executed as part of this rename pass. Track as separate follow-up
work; each item in §5.3 gets its own future PR with its own audit,
tests, and risk assessment.

### 6.1 Checkpoint policy

At each checkpoint:

```
# fast path — every commit within a step
meson compile -C build
meson test -C build -t 2  # unit tests, ~2 min

# full path — end of step
meson compile -C build
meson test -C build       # unit + functional, ~20 min
tests/perf/tracing-cache/v13-smoke.sh   # smoke perf, minutes
```

If a checkpoint fails: **stop, do not commit further renames.**
Investigate whether the rename hit a hidden collision, a substring
false match, or a comment-logic contradiction. Fix in place if the
issue is local; if the rename itself is unsafe, revert and re-plan.

Every step's final checkpoint must be green before the next step
starts. No batching a step's rename onto an already-red HEAD.

The intent is that a reader picking up the code at any step
boundary sees a *strictly-less-confused* codebase than at the
previous boundary — no rename increases the vocabulary count on
net, and no rename lands on a broken build.

## 7. Execution notes

Not every rename in this plan is a safe `sed`. Executors should
categorize each row by risk and pick the right tool. Comments are
never a `sed` target — they need reading. This section spells that
out so the plan is actually runnable.

### 7.1 Rename risk categories

**Green (mechanical, word-boundary find-and-replace safe):**

Distinct compound identifiers with no substring risk. Verify with
`grep -w OLDNAME src/libexpr/` before, apply, verify empty after.

- `d1CidasksWalk`, `perQAsksEdges`, `lastQFactsHash`,
  `dispatchedTrie`, `allRequestsTrie`, `allRequestsRsHash`,
  `curRequests`
- `v13FactSet`, `v13FactSetHash`, `v13Walk`
- `primeFactSetCache`, `flushPendingAmbient`, `splitFlush`,
  `markApplyBoundary`, `logDepth2Observation`,
  `logDepth2ApplyFact`
- `PendingApplyBoundary`, `PerQAsksEdge`, `SuppressApplyBoundary`
- `TracingLocalObject`, `ReplayLocalObject`,
  `LambdaApplyResultObject`
- `scopeStateIdAt`, `scopeStateIdAfter`,
  `scopeStateIdAtConverged`, `scopeStateIdAtWithHook`,
  `structuralAddress`, `structuralAddressAfter`,
  `inheritedScope` (the specific field)

**Yellow (word-boundary safe but existing collisions to check):**

Target names that may already appear elsewhere. Grep for the *new*
name before applying to catch conflicts.

- `v13Walk` → `walk` — `TracingDecisionGraph::walk` already
  exists as a member. The rename lives on `TracingReplayEvaluator`,
  which is a different class — no C++ collision — but confirm no
  call site relies on disambiguating the two by their prefix.
- `PendingApplyBoundary` → `ApplyBoundary` — verify no other
  `ApplyBoundary` type exists in `nix::` scope.
- `dispatchCache` → `responseFor` — `responseFor` may already be a
  common local name; verify the target site isn't shadowing another.
- `d2` → `ambient` prefix — the token `d2` is a substring of
  `md2`, `sd2`, decimals, etc. Use `\bd2[A-Z]` regex, or spell out
  specific tokens (`logDepth2Observation`, `d2CidasksWalk`, etc.)
  rather than blind substring replacement.

**Red (cannot be done with find-and-replace — per-site inspection
required):**

Bare common words or overloaded terms. Each occurrence needs a
human read of surrounding context.

- **`scope` → `argAncestry` (bare, in callers).** ~300
  occurrences in `src/libexpr/`. Not all mean the cidasks-scope:
  many are C++ variable scope, `let`-binding "in scope" prose, or
  generic namespace scope. Inside `subject-id.{hh,cc}` the
  parameter is unambiguously cidasks-scope (Green — handled in
  step 6b sub-item 3); in callers each `scope` variable needs
  per-site inspection (Red — sub-item 4). Rule of thumb: any
  variable passed as the second arg to `stateHashAt` /
  `subjectHashAt` / other cidasks functions is a cidasks-scope
  and gets renamed. All comments: read every one; if the sentence
  is about ancestry-of-args, rewrite to use "argAncestry"; if
  about lexical or C++ scope, leave.
- **`selfHash`, `subjectSelfHash`, `resultSelfHash` locals** —
  ~40 occurrences. Each is `stateHashAt(argId, 0, {}, 0)`. Word-
  boundary find-replace is *safe* for these specific compound
  spellings, but bare `selfHash` (if any exists in unrelated
  contexts) needs verification. Grep first, then apply.
- **`argSubject`, `argSubj`, `seedSubject` locals** — each
  occurrence names a `Subject` value. Word-boundary safe on the
  compound names; check for `Subject` alone in the same scope
  (unlikely).
- **`argStateId` in comments** — this term appears only in
  comments (no code identifiers currently). Read each; some mean
  "the arg's state hash", some are the false-stability shorthand
  that Rule 6 corrects. Replace with `stateHash` and, where the
  sentence contradicts the corrected model, rewrite the sentence.
- **`structuralAddress` in comments** — read; replace with
  `subjectHashAt` where the reference is to the function; rewrite
  discussion where the comment describes it as if "address" were
  the concept.

### 7.2 Comment handling

Comments cannot be find-and-replaced cleanly. Every rename that
touches a live comment needs someone to read the surrounding
sentence and answer three questions:

1. **Is the comment describing current behavior or old behavior?**
   Old-behavior comments (design-history "we used to X, then Y",
   iteration markers, reverted-approach notes) are archaeology per
   Rule 3 — delete, don't rename.
2. **Does the sentence's logic still hold under the new name?**
   Some comments justify a name choice ("we call this `scope`
   because it's the surrounding lexical context"). After the
   rename, that justification is false. Rewrite the sentence to
   the current rationale ("we call this `argAncestry` because
   it's the XOR-fold of enclosing callback args' state hashes"),
   or delete if the justification is no longer load-bearing.
3. **Is the surrounding prose still coherent?** A comment like
   "the scope's argStateId at K" becomes "the argAncestry's
   stateHash at K" under naive substitution — technically correct
   but awkward. Recast to natural prose: "the callback arg's
   state hash at K, given the enclosing argAncestry".

Practical workflow: after each rename step, `git diff` and read
every touched comment. Anything that reads mechanical-substituted
gets rewritten. This is real work — budget for it.

### 7.3 Verification workflow per commit

For each mechanical rename step:

```
# 1. Verify no unexpected pre-existing hits on the target name
grep -wrn NEWNAME src/libexpr/

# 2. Apply the rename
find src/libexpr/ -type f \( -name '*.hh' -o -name '*.cc' \) \
    -exec sed -i 's/\bOLDNAME\b/NEWNAME/g' {} +

# 3. Verify the old name is gone
grep -wrn OLDNAME src/libexpr/  # should be empty or only in
                                # historical/outdated docs

# 4. Read the diff, focus on comments
git diff --stat
git diff src/libexpr/

# 5. Compile + test
meson compile -C build
meson test -C build
```

For the Red category, replace step 2 with a targeted editor pass
guided by grep output — do not use `sed`.

### 7.4 What's out of scope for the mechanical pass

Not everything the plan proposes can be done by a rename script.
The following need judgment calls that belong in follow-up work,
not this pass:

- Comments describing reverted mechanisms ("Fix A", "Fix B",
  "supplementary-objects stack", "offline AmbientResult walk"):
  read every hit; delete, don't attempt to rename.
- Comments that reference the design doc's history section:
  audit whether the reference still points at extant content
  after the doc split (§5.2).
- Any comment that would end up self-contradictory after a rename
  (e.g. "we chose `Address` because it identifies stored content"
  after renaming to `Hash`) — rewrite the sentence.
- Splits and collapses in §5.3 — these are refactors, not renames.

## 8. Non-goals

This plan does **not**:

- Change behaviour. No renamed identifier introduces a new codepath.
- Change file locations except for the deletions in §5.1 and the split
  in §5.2. Header/source pairs remain paired.
- Alter the SQLite schema or on-disk formats. Table and column names
  stay for backwards compatibility with existing caches.
- Address the JSON trace-file format or CLI flags. Those are
  user-visible and get a separate deprecation pass if we touch them.

## Appendix A: quick-reference table

```
Old identifier                    New identifier
------------------------------    ---------------------------------
d1CidasksWalk                     envWalk
d2  (as identifier prefix)        ambient
perQAsksEdges                     envAsksEdges
lastQFactsHash                    envCur
dispatchedTrie                    envDispatchedTrie
dispatchCache                     responseFor
allRequestsTrie                   sessionRequestsTrie
allRequestsRsHash                 sessionRequestsRsHash
curRequests                       dispatchedSoFar
v13FactSet                        envFactSet
v13FactSetHash                    envFactSetHash
v13Walk                           walk (method)
primeFactSetCache                 installFactSet
flushPendingAmbient               flushAmbient
splitFlush                        closeAsksEdge
markApplyBoundary                 openApplyBoundary
logDepth2Observation              logAmbientObservation
logDepth2ApplyFact                logAmbientApplyFact
PendingApplyBoundary              ApplyBoundary
PerQAsksEdge                      AsksEdgeRecord
TracingLocalObject                TracingCallbackArg
ReplayLocalObject                 ReplayCallbackArg
LambdaApplyResultObject           TracingCallbackApplyResult
nix::cidasks::*                   nix::*   (flatten; no new namespace)
content-identity-via-asks.hh/cc   subject-id.hh/cc
scope (cidasks param/field/local) argAncestry
inheritedScope (field)            argAncestry
applyScope (helper + locals)      applyArgAncestry
fnScope / argScope (params)       fnArgAncestry / argArgAncestry
callScope (field + locals)        callArgAncestry
ArgScopeCell (type)               ArgCell
arg-scope.hh (file)               arg-cell.hh
argScope (field: ArgCell type)    argCell
effectiveArgScope()               effectiveArgCell()
getProxyArgScope()                getProxyArgCell()
scopeStateIdAt                    stateHashAt
scopeStateIdAfter                 stateHashAfter
scopeStateIdAtConverged           stateHashConverged
scopeStateIdAtWithHook            stateHashAtStamping
structuralAddress                 subjectHashAt
structuralAddressAfter            subjectHashAfter
extractFrom                       fromStateHashOf
selfHash / subjectSelfHash (loc)  argIdHash  (or argId if unambig)
argSubject / seedSubject (loc)    argId  (type: Subject)
argStateId (in comments)          stateHash
<replay-local-lambda>             <replay-callback-arg-lambda>
```

## Appendix B: things intentionally kept

These names look "off" but are actually load-bearing; do not rename
them thinking you're being helpful:

- `Asks` / `Terminals`: the design-doc names for the two edge tables.
  Anchor of the whole model.
- `Request` / `Response` / `Query` / `Result`: capitalised, canonical.
  Same term for env and ambient layers is *intended* — the ambient
  layer is a Request/Response conversation, just aimed at an
  inner-supplied local rather than the environment.
- `cur` / `nextCur` / `startCur` / `terminalCur`: walker-state canon.
- `FactSet` XOR hashing, `RequestSet` Merkle hashing: two distinct
  hashing schemes with distinct algebraic properties; the naming
  distinction matters.
- `useful` / `usefulDispatch`: canonical for "the requests on this
  edge that aren't already in cur's facts".
- `TrieBuilder`: the incremental request-set trie helper.
- `primop`, `<cached-fn>`, `<ambient-fn>`: user-visible names, not
  cache internals.
- The **identify/characterize** split (rule 6). It looks like
  Hungarian notation; it isn't. `Id` names a stable *thing* whose
  value doesn't depend on observations, walks, ancestry, or
  invocations — a genuine invariance promise. Characterizations
  (of state or payload) may be **data** (`walk`, `FactSet`
  members, `observations`) or **hashes** (`stateHash`,
  `factSetHash`, `argAncestry`, `queryHash`); the `Hash` suffix
  applies only to the hash representation, not to the underlying
  concept. Any `*Id` must be provably invariant under ancestry
  and walk; if you catch one that isn't, that's a bug, not a
  rename opportunity.
