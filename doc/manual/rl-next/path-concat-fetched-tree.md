---
synopsis: "Path `+` no longer accepts a fetched-tree path as a non-first operand"
---

`p + q` where `q` is a path value rooted on a fetched tree (`fetchTree`, flake
inputs, …) now raises an evaluation error. This shape was unstable in
lazy-paths mode: the fetched-tree operand was stringified by walking the
entire tree to splice a storepath substring into the joined path, and the
resulting subpath wasn't a meaningful address.

The first-operand-Copyable case is unchanged — `fetchTreeResult + "/sub"`
stays valid and produces a fetched-tree-rooted path at the joined subpath,
read through the tree's accessor.

The historical System+System form (e.g. `/var/lib + /var/log`) is preserved
as a backward-compatible quirk. For path-shaped composition where you need
the fetched tree on the right, use string interpolation (`"${a}/${b}"`).

The fetched-tree side is still experimental, so the breakage is contained
to opt-in usage.
