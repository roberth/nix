---
synopsis: "Flake source paths drop the `<hash>-<hash>-source` shape in favour of `<hash>-source`"
---

String-coercing a flake's `./.` (or any other expression that materialises
the flake's source root through `coerceToString`) now produces a store
path of the form `/nix/store/<hash>-source` rather than the historical
`/nix/store/<hash>-<hash>-source`.

The double-hash form was an undesired artefact of the old flake-loading
path (which copied the already-fetched tree into the store a second time
under the outer flake's fetch-derived name), tracked in issue #10627.
Under lazy paths the materialisation goes through the fetcher's
single store-path naming, so the second hash is no longer composed in.

The store paths of flake sources will therefore change at upgrade time.
Code that hard-codes a specific `<hash>-<hash>-source` store path needs
to be regenerated against the new shape; everything that goes through
the flake's `outPath` (or `sourceInfo.outPath`) continues to work
without change.
