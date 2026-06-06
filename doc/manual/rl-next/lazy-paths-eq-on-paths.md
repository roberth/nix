---
synopsis: "`==` on paths now compares contents, not accessor identity"
---

The `==` operator on path values now decides equality by comparing the
two paths' NAR contents (with a fast-path on matching fingerprints),
not on accessor pointer identity. Two path values rooted on distinct
accessors that resolve to the same fetched-tree contents now compare
equal, where they previously compared unequal.

This is the language semantic that `toString a == toString b` had
already implied — `toString` materialised both sides, and the
resulting store-path strings agreed. The lazy-paths regime makes the
same answer reachable without forcing materialisation: the
fingerprint-based shortcut decides for fetched trees that carry an
externally-recorded fingerprint, and a NAR walk is the fallback only
when no cheaper mechanism decides.

The previous behaviour treated path values from any caching or
mounting layer that produces multiple accessor instances for the
same store path as unequal. That was a footgun for code that
compared `getFlake "foo".outPath` to a known store-path-shaped
path. The new behaviour matches what users expected.

For the cross-type case (path × string), see
`builtins.isPathEquivalent`.
