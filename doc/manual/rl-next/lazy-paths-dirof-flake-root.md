---
synopsis: "`dirOf` on a flake's `./.` evaluates to `/` instead of the store directory"
---

Under lazy paths a flake's `./.` is a path value rooted at its accessor's
root rather than at the flake's materialised store path. `prim_dirOf` on a
root path returns the same path unchanged (it has no parent to take), and a
root path JSON-renders as `"/"`.

Previously `./.` was rooted at the materialised store path (e.g.
`/nix/store/<hash>-source`), so `dirOf ./.` was the containing
`/nix/store`. Callers that relied on `dirOf ./.` to recover the store
directory should read the configured store directory directly (e.g. via
`builtins.storeDir`) rather than deriving it from a flake-source path.
