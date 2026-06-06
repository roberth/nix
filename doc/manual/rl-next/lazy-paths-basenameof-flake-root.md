---
synopsis: "`baseNameOf` on a flake's `./.` evaluates to `\"\"` instead of `<hash>-source`"
---

`prim_baseNameOf` now reads a path argument's trailing segment directly
from its canon path. The accessor root has no basename, so `baseNameOf
./.` on a fetched flake (or any other Copyable-rooted path at its root)
is the empty string.

Previously the path argument was routed through `coerceToString`, whose
Copyable arm walks the accessor's root via `copyPathToStore` to render
`<storePath>/`; the basename of that rendering was the trailing
`<hash>-source` segment. The `<hash>-source` value was an artefact of
that rendering, not a stable contract — and under lazy paths the walk
materialised the entire fetched tree just to derive the name and throw
the rest away.

Callers that depended on the `<hash>-source` shape (typically the
`name = baseNameOf (toString src)` pattern in wrapper derivations)
should pass an explicit `name` via the surrounding derivation's `name`
attribute instead.
