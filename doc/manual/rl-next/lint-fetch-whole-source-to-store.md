---
synopsis: "New setting: `lint-fetch-whole-source-to-store`"
---

A new evaluator-level lint that surfaces flows which read the entire
contents of a fetched source (a flake input, a `fetchTree` result, …)
into the store — either by computing a NAR hash or by copying the NAR.

The setting accepts `ignore` (default; no change in behaviour), `warn`,
or `fatal`. Under lazy paths most consumers stay on metadata and
sub-paths and never trigger such a walk; when one does fire, it is
either an eager fallback inside Nix that can be improved or a
user-visible coercion such as `toString src`. Setting the lint to
`warn` or `fatal` is useful during development for finding the
offending callsite.
