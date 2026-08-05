# SourceRoot integration for OuterObject wrap

Next-session handoff. Captures the SourceRoot piece the current
tracing eval-cache work punts on, and the plan we sketched
2026-08-05 for addressing it.

## Current failure

`tests/functional/tracing-eval-cache.sh`'s flake cases fail. The
concrete surface: `callFlakeViaEvaluator`'s `vOverrides` tree
contains `mkPath` values (from `sourceInfo.outPath`); once put
behind an `OuterObject`, those path values stop resolving.
`import (flakePath + "/flake.nix")` → `flakePath` coerces to empty
→ `path '/flake.nix' does not exist`.

The failure is in the OuterObject wrap itself — not specific to
`TE::apply`. Same-shape breakage triggers from any combination
that puts a non-system-root `mkPath` value behind OuterObject:
`fetchTree` + `builtins.cache` is another entry point. The current
`builtins.cache`-only test coverage happens not to exercise it
because those args carry no non-trivial SourceRoots.

## The missing element

A **path value** is a pair: `(path, SourceRoot)`. The SourceRoot
tells you *where the contents can be retrieved*. For workloads
where all paths share the system root as their SourceRoot, this
is invisible — the current design implicitly assumes one global
SourceRoot and ignores it. Under the flakes integration, that
assumption breaks: each fetched flake input has its **own** distinct
SourceRoot.

`OuterObject` today preserves the path string but loses the
SourceRoot behind it. `getStringWithContext` then coerces via a
different codepath than the raw Value would, returning the wrong
string (empty, in practice).

## Proposal: imprint SourceRoot identity via Selector

Naming SourceRoots is an open problem in general. For the flakes
case:

- When the wrap layer first encounters a SourceRoot inside an arg,
  it derives a Selector-based identity from **where the SourceRoot
  first occurs** in the arg's structure (e.g., "SourceRoot at
  `arg.overrides.foo.outPath`").
- Maintain a `SourceRoot* → Selector` map at the arg's scope. Later
  path values referring to the **same** SourceRoot pointer reuse
  the imprinted Selector.
- The Selector-based identity is what persists into the cache
  (SourceRoot pointers don't survive process boundaries — Selectors
  do, being content-derived).

## What each layer needs

**OuterObject wrap** — preserve the `(path, SourceRoot)` pair as a
first-class value inside the OuterObject, using the imprinted
Selector as the SourceRoot's persistent identity.
`getStringWithContext` / `getPath` on the wrapped path must route
through the imprinted SourceRoot rather than fall back on
system-root stringification. This is the core fix; the callers
that trigger the current failure (`TE::apply` from
`callFlakeViaEvaluator`, `fetchTree` + `builtins.cache`, etc.) all
depend on it.

**Walker (read side)** — given a recorded path Selector, reconstruct
the appropriate SourceRoot via the imprinted-identity map. The
walker cannot resurrect the original SourceRoot pointer; it must
look up which SourceRoot the current arg's imprint associates with
that Selector.

## Regression test to add

The wrap-preserves-SourceRoot property. Two equivalent shapes to
trigger:

- **Unit test** in `src/libexpr-tests/`: construct a value with a
  path whose SourceRoot isn't the system root (via
  `builtins.makePath` or a `fetchTree`-derived value), wrap the
  containing attrset via `wrapArgAsCallbackScope`, assert
  `getStringWithContext` returns the expected store path.
- **End-to-end test** in `tests/functional/`: `fetchTree` +
  `builtins.cache` should preserve non-system-root SourceRoots
  through the primop's wrap. This is what
  `tests/functional/tracing-eval-cache.sh`'s flake cases already
  approximate; a smaller test would isolate the SourceRoot-
  preservation property specifically.

Once wrap preserves SourceRoot, `tests/functional/tracing-eval-cache.sh`'s
flake cases should follow.

## Starting points

- `src/libexpr/expr-from-object.cc` — `wrapArgAsCallbackScope`.
- `src/libexpr/outer-object.cc` — OuterObject's method chain,
  particularly `getStringWithContext` / `getPath` on values that
  carry SourceRoot.
- `src/libexpr/tracing-callback-arg.cc` — TCA's parallel handling.
- Failing test: `tests/functional/tracing-eval-cache.sh` (flake
  cases at end).

## Open questions

- How to name SourceRoots in general (beyond flakes' first-
  occurrence-in-arg derivation)? Left for later; the flakes case
  is what unblocks the current failing test.
- Does the `SourceRoot → Selector` map live on the OuterObject,
  the cell, or the SelectorPool? First-occurrence derivation
  suggests per-arg scoping (on the cell), consistent with how
  observations are attributed.
