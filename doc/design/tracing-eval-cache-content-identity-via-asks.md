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
     = initial(N) XOR (XOR-fold over facts in F about seed_N)
   ```

   For a derived or apply-result subject S:

   ```
   contentId(S, F) = qH(producer_query of S with constituent
                        subjects' content ids substituted at F)
   ```

4. **Membership in "facts about V" is decided per Asks edge.** At an
   Asks edge's precondition factset, each subject has a content id;
   facts in this edge whose `from` field equals that id are
   observations on V for this edge. New edges re-decide membership
   against their own precondition. No global filter, no recursive
   resolution at fact-emission time.

5. **At recording flush, fact `from` fields are rewritten per Asks
   edge.** The recorder buffers facts during a query's evaluation
   carrying placeholder identifiers. At flush, it builds Asks edges
   and substitutes each fact's `from` to the content id of the
   referenced subject at that edge's precondition factset. Pool keys
   (`reqHash`) and Asks edges are content-addressed over the
   post-substitution form.

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

Inheriting outer-scope CDIs ripples through every fact, so atom
sharing across cached calls is reduced. That's a deliberate
trade-off: storage cost in exchange for collision-free
disambiguation of LocalObject identity.

### Atom storage

`AmbientRequest`/`AmbientResponse` payloads are content-addressed
just like everything else. They don't need their own tables —
reuse the existing `Requests` and `Responses` CAS pools (with
`Responses` keyed by `responseHash`, fixing the current schema
which keys it by `requestHash`). The trie tables discriminate by
which atom set they reference.

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
Inheritance distinguishes recordings by making *every* fact's
`from` (and therefore `requestHash`) unique per recording from
the very first probe.

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

Each fact's `elementHash = SHA-256(requestHash || responseHash)`
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
