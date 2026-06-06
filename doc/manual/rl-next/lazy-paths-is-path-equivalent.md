---
synopsis: "New `builtins.isPathEquivalent` for cross-type path/string equivalence"
---

`builtins.isPathEquivalent a b` answers whether two values would
toString to the same string, accepting path × path, path × string,
or string × string. The path × path case agrees with the language
`==`. The cross-type and string × string cases answer the question
the module system has been asking via `toString` for years — "is
this string the toString of this path?" — without the
materialisation that `toString` itself triggers.

The underlying engine biases hard toward cheap discriminators
(pointer identity, fingerprint match, root-name-set probe,
storePath cache hit). A NAR walk is the fallback only when no
cheaper mechanism decides; the typical module-system flow over a
single fetched tree decides on the fingerprint shortcut alone — no
I/O.
