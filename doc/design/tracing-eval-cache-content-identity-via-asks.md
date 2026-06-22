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
   Preserves laziness; non-negotiable for Nix semantics.

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

The cache has two `Request`/`Response` layers; conflating them in a
single set of pools is what causes sibling cb invocations with
identical Subjects (and therefore identical `valueHandle`s) to
collide.

| | Depth-1 (input tracing) | Depth-2 (interaction tracing) |
|---|---|---|
| Key into trie | top-level `Q` (Query for the cached call) | `valueHandle` (= `contentIdAfter(subject, {})` for an inner-supplied value crossing the boundary) |
| Atom request | `Request` (file read, env var, AmbientQuery for outgoing-ambient) | `AmbientRequest` (a probe applied by the outer to a LocalObject — force, getAttr, getListElem, getInt, …) |
| Atom response | `Response` (file hash, env value, outgoing-ambient result) | `AmbientResponse` (the value the probe produced — type tag, child handle, scalar) |
| Fact | `(Request, Response)` | `(AmbientRequest, AmbientResponse)` |
| FactSet | XOR-fold over Fact hashes | XOR-fold over AmbientFact hashes |
| Edges | `Asks(Q, factSet) → factSet'` via `RequestSet` | `AmbientAsks(handle, factSet) → factSet'` via `AmbientRequestSet` |
| Terminal | `Terminals(Q, factSet) → Result` | the final `factSet` itself — no separate Result; what's downstream lives in depth-1 |
| Live source at replay | recomputed from the live environment; no Response payload retained | served from a content-addressed `AmbientResponses` pool — there is no live producer for an inner-supplied value once the inner stops running |

The **cross-layer linkage** is that each depth-1 `Response` for an
`AmbientQuery`-shaped `Request` is an `AmbientFactSetHash` — the
content hash of the depth-2 trie's terminal that the outer's probing
landed at. The depth-1 walker, when dispatching such a `Request`,
hands control to the depth-2 walker, which proceeds reactively: each
live probe the outer makes on the LocalObject selects the next
depth-2 Asks edge; the cumulative XOR-fold lands at the terminal
factSet that *is* the depth-1 `Response` value.

### Storage schema (depth-2)

Mirrors the depth-1 schema with two differences: an explicit
`AmbientResponses` pool (because there is no live producer at
replay), and no separate Terminals table (the terminal factSet hash
is its own identity).

```
AmbientRequests  (requestHash  BLOB PRIMARY KEY, payload BLOB)
AmbientResponses (responseHash BLOB PRIMARY KEY, payload BLOB)

AmbientRequestSetNodes(nodeHash BLOB PRIMARY KEY, payload BLOB) WITHOUT ROWID

AmbientAsks(handle BLOB, factSetHash BLOB, requestSetHash BLOB,
            nextFactSetHash BLOB,
            PRIMARY KEY (handle, factSetHash, requestSetHash)) WITHOUT ROWID
```

`AmbientResponses` is CAS by `responseHash` — multiple recorded
responses to the same `AmbientRequest` coexist as distinct rows.
This is what makes same-`valueHandle` sibling cb invocations safe:
their `AmbientFact`s share `requestHash` but have distinct
`responseHash`es, and the `AmbientAsks` edges from `(handle,
factSet)` select the right one per branch.

### Recording (depth-2)

When the cold path runs and the outer probes a `TracingLocalObject`,
each probe emits an `AmbientFact`. The writer XOR-folds it into the
current depth-2 `factSet` for this `valueHandle`. At flush, the
recorded edges are added to `AmbientAsks` and the response payloads
to `AmbientResponses`. The final factSet hash becomes the depth-1
`Response` payload (a `Hash`) for the enclosing `AmbientQuery`
`Request`.

### Replay (depth-2)

When the depth-1 walker dispatches an `AmbientQuery` `Request`, it
enters depth-2 replay for the corresponding `valueHandle`. The depth-2
walker starts at `(handle, ∅)` and proceeds reactively: the cb apply
on the standin runs the outer live; each probe the outer makes
identifies a depth-2 Asks edge whose `AmbientRequestSet` contains
the probe's hash; the walker fetches the recorded
`AmbientResponse` from `AmbientResponses[responseHash]`, returns it
to the outer, and XOR-folds the fact into the running factSet. When
the outer stops probing, the final factSet hash is the value of the
depth-1 `Response`.

A live probe that doesn't appear on any recorded branch from the
current depth-2 position is a stale-cache signal: the walker bails,
and the depth-1 walker falls through to inner re-evaluation.
