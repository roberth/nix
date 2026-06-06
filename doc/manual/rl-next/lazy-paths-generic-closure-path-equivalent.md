---
synopsis: "`builtins.genericClosure` gains a `pathEquivalent = true` mode"
---

`builtins.genericClosure { pathEquivalent = true; ... }` switches the
closure's dedup comparator from the type-strict default to a
toString-equivalence one. With `pathEquivalent = true`, `startSet`
is allowed to mix path and string keys; the dedup recognises
cross-type equivalence between a path value and a store-path-shaped
string that names the same tree, and collapses them.

The expected use is the module system, which historically called
`toString` on every module reference to normalise types before
passing to `genericClosure`. Under lazy paths, that `toString` walk
forced materialisation of every fetched tree. With
`pathEquivalent = true`, the module system can drop the coercion
without losing the equivalence behaviour it relied on, and the
dedup engine stays lazy (no walk on the typical
single-fetched-tree closure).

Known limitation: the `pathEquivalent` dimension currently bundles
a semantic choice (cross-type equivalence) with a performance
choice (lazy vs. eager dedup). Decoupling the two is a follow-up
not in scope for this release; `pathEquivalent = false` keeps the
historical eager-toString shape, which may trip the
`lint-fetch-whole-source-to-store` setting on fetched-tree closures
as a result.
