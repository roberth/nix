---
synopsis: "`<` on paths is now `toString a < toString b`"
---

The `<` family on path values (`<`, `<=`, `>`, `>=`,
`builtins.lessThan`, `builtins.sort` on path-keyed lists) is now
aligned with `toString a < toString b`: identity-shortcut or
structural comparisons that confused accessor identity with
toString equivalence gave a different total order than the
language admits, and have been replaced by a layered comparator.

The comparator tries cheap discriminations first (same-root subpath
compare; cross-kind storeDir-prefix test) and only materialises via
`copyPathToStore` as a last resort. For fetched-tree (Copyable)
paths, the materialisation fallback fires
`lint-fetch-whole-source-to-store` at the language `<` site, which
is intentional — `<` on paths is semantically a `toString` call
and a `toString` on a Copyable root *is* a materialisation point.

User-visible consequence: for `getFlake foo`-shaped path values,
`<` agrees with the user-intuitive store-path-string order even
when the paths come from distinct accessor instances.
