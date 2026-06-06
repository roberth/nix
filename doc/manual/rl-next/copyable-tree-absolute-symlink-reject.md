---
synopsis: "Reads through absolute symlinks in fetched-tree sources now error"
---

`readFile`, `import`, and `"${path}"` interpolation on a path whose
ancestor chain traverses an absolute symlink inside a fetched tree
(`fetchTree`, flake input, …) now raise an evaluation error. Pre-v8
the symlink target was silently rebased to the fetcher's accessor
root — e.g. a `/usr/bin/foo`-targeted symlink resolved to the tree-
internal `<accessor-root>/usr/bin/foo` and typically returned "file
not found".

The tree itself is **not** rejected. Fetch and lockfile creation
still succeed, and reads that don't go through the offending
symlink continue to work. Only the specific read through the
absolute symlink errors.

The new behaviour reflects a structural requirement: fetched trees
must be position-independent because they materialise into
storepaths at some point, and `/` shifts meaning across that
boundary. An absolute symlink target resolves to different files
before vs after materialisation; refusing the read at admission
keeps the contract clean. The diagnostic is *"absolute symlink '%s'
(target '%s') ... Copyable trees must be position-independent"*.

To work around when you control the upstream content: replace the
absolute symlink with a relative one. To work around when you
don't: copy the materialised path into the store explicitly (e.g.
via `builtins.path { … }`) so subsequent reads go through a System
root where absolute symlinks remain lenient.
