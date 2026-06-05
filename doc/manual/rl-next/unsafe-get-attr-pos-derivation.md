---
synopsis: "`builtins.unsafeGetAttrPos` returns `null` for synthetic derivation attributes"
---

`builtins.unsafeGetAttrPos` on a derivation's return attributes (`outPath`,
`drvPath`, the per-output attrs, …) now evaluates to `null`. These attributes
are synthesised by the `derivation` primop, so reporting any source position
was an impurity, as the result varied with Nix's internal source layout.
