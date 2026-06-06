---
synopsis: "Position attrsets gain a `.path` attribute"
---

Position attrsets returned by `__curPos`, `builtins.unsafeGetAttrPos`,
and similar primops now include a `.path` attribute alongside the
existing `.file`, `.line`, `.column`. The `.path` is a path value
carrying the originating `RootedPath` — useful for the NixOS module
system's location-comparison patterns, which previously had to
sniff `.file` string content to recover position equivalence.

`.file` retains its existing semantics (the resolved string form,
materialised lazily via the kind dispatch — `null` for Internal,
the raw abspath for System, the materialised storepath + subpath
for Copyable). For Internal-rooted positions, both `.path` and
`.file` resolve to `null`.

Modules that compared `.file` strings to decide whether two
position attrsets named the same file can now compare `.path`
directly without forcing materialisation.
