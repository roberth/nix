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
