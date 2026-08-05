# SourceRoot integration for OuterObject wrap

Handoff for the SourceRoot piece the current tracing eval-cache
work punts on. Design as of 2026-08-05.

## Current failure

`tests/functional/tracing-eval-cache.sh`'s flake cases fail cold at
the first `nix eval git+file://…/flakeA#answer`:

```
at «flakes-internal»/call-flake.nix:80:
    flake = import (flakePath + "/flake.nix");
error: path '/flake.nix' does not exist
```

`flakePath` coerces to empty because its SourceRoot never crossed the
`OuterObject` boundary that `evaluator.apply` opens via
`wrapArgAsCallbackScope` (`src/libexpr/expr-from-object.cc:328`);
what came back was a `RootedPath{outerRootFSRoot, CanonPath{""}}` —
right path string, wrong root.

The failure is not specific to `callFlakeViaEvaluator` and not caused
by caching (it fires on cold). It's the OuterObject wrap itself
substituting a fixed SourceRoot for whatever the underlying value
carried. Any combination that puts a non-system-root `mkPath` value
behind an `OuterObject` triggers it — `fetchTree` + `builtins.cache`
is another entry point; the current `builtins.cache`-only test
coverage happens not to exercise it because those args carry no
non-trivial SourceRoots.

## Two concrete loss points

Both need fixing:

1. **Wire format drops SourceRoot.** `computeWHNFFromObject`
   (`tracing-object.cc:43`) writes
   `trace::WHNFPath{obj.getPath().path.abs()}` — just a string,
   `.abs()` on the CanonPath drops SourceRoot. The trace-layer
   payload `WHNFPath { std::string path; }`
   (`include/nix/expr/trace-types.hh:227`) has no field for it.

2. **`OuterObject::getPath` substitutes a fixed root.** The proxy
   pins one `outerRootFSRoot` at construction (the outer's
   System-kinded `state.rootFSRoot`) and returns
   `RootedPath{outerRootFSRoot, CanonPath(p->path)}` on every
   `getPath` call (`outer-object.cc:151-163`). Even if the wire
   format carried SourceRoot, this step would still substitute the
   wrong one. `TracingCallbackArg::getPath`
   (`tracing-callback-arg.cc:120-122`) and
   `ReplayCallbackArg::getPath` (`replay-callback-arg.cc:145`) have
   the same shape.

## The missing element

A **path value** is a pair `(path, SourceRoot)`. The SourceRoot tells
you *where the contents can be retrieved*. When all paths share the
System root, this is invisible — the current design implicitly
assumes one global SourceRoot and ignores it. Under flakes each
fetched input has its **own** distinct SourceRoot; the assumption
breaks.

## Existing SourceRoot naming infrastructure

Groundwork already in place, awaiting a consumer:

- `SourceRoot::unpinnedId` (`util/source-root.hh`) — a raw producer
  claim, e.g. `github:NixOS/nixpkgs` from `Input::toUnpinnedURL()`.
  Set by producers that know an Input; `nullopt` for Internal
  helpers and any producer that doesn't. **Not unique on its own** —
  two admissions of the same URL under different accessors carry the
  same raw string.
- `EvalState::allocSourceUnpinnedId(SourceRoot &)` (`paths.cc:45-73`)
  — derives `<url>#<n>` where `n` is a per-URL admission counter,
  memoised on `(url, accessor)`. The addressing scheme that makes
  the identifier unique per EvalState. Returns `nullopt` when the
  raw `unpinnedId` is `nullopt`. Defined and unit-tested; no
  production caller yet.
- `EvalState::getOrCreateRoot(accessor, kind, unpinnedId)`
  (`eval.hh:490`) — the accessor-side memoiser. Missing: a reverse
  lookup by identifier (identifier → SourceRoot).

Under the current cache the `#n` numbering is used nowhere; the
handoff below wires it in as the initial identity scheme, with a
Selector-based path as the direction of choice for cross-session
stability.

### Numbering has bounded stability

`allocSourceUnpinnedId`'s `#n` is a per-EvalState-session admission
counter. Same URL admitted first in one run and second in another
(user runs different queries, evaluation order shifts) gets `#0`
vs `#1` — same SourceRoot, different identifier. Cross-recording
reuse breaks whenever admission order shifts.

For a first landing this is acceptable — same-shape workloads
(re-running the same command) reproduce the same counters, so
warm-after-cold in one CLI shape works. Cross-invocation reuse
where the user's query pattern changes between recordings will
mismatch on the identifier and miss cleanly (a legitimate miss under
Foundational 1, not a wrong hit — the walker's live SourceRoot
lookup fails, downstream cache misses, inner fallback answers
correctly).

## Direction of choice: Selector-based imprinting

A more stable identifier scheme derives the SourceRoot's identity
from **where it first appears structurally in the arg**. When the
wrap layer first encounters a SourceRoot inside a probe response,
imprint it with the Selector that reached the containing path value
(e.g. "SourceRoot at `arg.overrides.foo.outPath`"). Maintain a
`SourceRoot* → Selector` map at the arg's cell scope; later path
values referring to the same SourceRoot pointer reuse the imprinted
Selector.

Selectors are content-derived and survive process boundaries, so
this identifier reproduces across recording sessions whenever the
arg's *structure* is stable — a stronger stability guarantee than
admission-order numbering. It's still subject to entropy where the
same SourceRoot appears via more than one structural path, but
matches the eval cache's overall identity philosophy: value
identity comes from observation history, not from live-side
allocation.

Not required for the first landing; numbering is what unblocks the
flake test. Selector-based imprinting is the direction to move
identifier construction toward once numbering has proven the shape
of the integration.

## Identifier space design

One identifier field on the wire, not `(kind, unpinnedId)`:

- **Real System root** → bare `"system"` (no `#n`). Singular by
  construction. Distinct from any anonymous producer that happens
  to claim "system" and gets `system#n` through `allocSourceUnpinnedId`.
- **Stamped roots** (flake inputs, `fetchTree`) →
  `allocSourceUnpinnedId(root)` returns `"<url>#<n>"`.
- **Internal-kinded roots** (corepkgs, derivation-internal) →
  **disallowed** across the cache boundary. Throw at the wrap seam
  if such a path tries to cross. They're helper-file mechanics
  never meant to reach a user; refusing keeps the identifier space
  from carrying a dead branch.
- **Anonymous producer** (an ad-hoc `builtins.makePath` with no
  Input) → `allocSourceUnpinnedId` returns `nullopt`. Two options:
  fall through to the Selector-based imprinting path (when
  implemented), or refuse the crossing until then. Concrete choice
  depends on whether real workloads produce these.

Folding `kind` into the identifier keeps the wire format single-
field and moves the classification from a tag to a string prefix
convention. Selector-based imprinting also produces a
distinguishable string, so both admission schemes coexist in the
same identifier space.

## Proposal — phased

**Phase 1 — Wire-format extension for identifiable SourceRoots.**
Enough to fix the failing flake test.

- Extend `WHNFPath` in `trace-types.hh` with a `std::optional<std::string>
  sourceRootId`. Update JSON + CBOR codecs.
- Update `computeWHNFFromObject` (`tracing-object.cc:43`) to capture
  the SourceRoot's identifier via `state.allocSourceUnpinnedId(*rp.root)`
  (with the System-root synthetic `"system"` handled at admission).
- Add an identifier → SourceRoot reverse lookup, populated
  alongside `rootCache` at `getOrCreateRoot`. No canonical owner:
  each cache layer assigns or tracks identifiers on its own scope
  (an inner and outer `builtins.cache` each admit their own
  SourceRoots and don't share the mapping through a global).
- Rewrite `OuterObject::getPath` to consult the wire payload's
  `sourceRootId`: if present and lookup hits, use that SourceRoot;
  else miss cleanly and let inner fall back (correct-or-miss
  discipline — never substitute a stand-in SourceRoot). Mirror in
  `TracingCallbackArg::getPath` and `ReplayCallbackArg::getPath`.
- Refuse Internal-kinded paths at the wrap seam
  (`wrapArgAsCallbackScope`, `TracingCallbackArg` construction) with
  a clear error.

**Phase 2 — Selector Q-hash disambiguation.** `SelectorImport{std::string
path}` and `SelectorExpr{expr, baseDir}` also lose SourceRoot info.
Two flake inputs each with `/flake.nix` produce identical Selector
Q hashes — wrong-hit risk at warm cross-invocation reuse under
matching-until-divergence. Add the same `sourceRootId` field to
those Selector alternatives so their content hashes carry the
SourceRoot identity too. Not required for the cold failure;
required to keep Foundational 1 (no wrong hits) under warm cross-flake
reuse.

**Phase 3 — Selector-based imprinting for anonymous SourceRoots
and cross-session stability.** Replace the `allocSourceUnpinnedId`
numbering with (or supplement it with) Selector-derived identifiers.
Handles the `unpinnedId=nullopt` case at the same time. Groundwork
lives on the arg's cell (`SourceRoot* → Selector` map). Deferrable
until Phases 1–2 are green and we can see which anonymous roots
actually leak through in real workloads.

**Phase 4 — Regression tests.**

- Unit in `src/libexpr-tests/`: construct a value with a path whose
  SourceRoot isn't the system root (via `builtins.makePath` or a
  `fetchTree`-derived value), wrap via `wrapArgAsCallbackScope`,
  assert `getPath` round-trips the SourceRoot's identifier.
- End-to-end in `tests/functional/`: `builtins.cache` + `fetchTree`
  combo isolating the SourceRoot-preservation property.
  Once wrap preserves SourceRoot, `tests/functional/tracing-eval-cache.sh`'s
  flake cases should follow.

## Starting points

- `src/libexpr/expr-from-object.cc` — `wrapArgAsCallbackScope`
  (`:328`); OuterObject construction pins the SourceRoot
  (`:385-387`).
- `src/libexpr/outer-object.cc` — `getPath` (`:151-163`),
  `outerRootFSRoot` member (`:118`).
- `src/libexpr/tracing-callback-arg.cc` — TCA's parallel handling
  (`getPath` at `:120-122`).
- `src/libexpr/replay-callback-arg.cc` — RCA's `getPath` at `:145`.
- `src/libexpr/tracing-object.cc` — `computeWHNFFromObject`'s
  `nPath` arm (`:42-43`).
- `src/libexpr/include/nix/expr/trace-types.hh` — `WHNFPath`
  definition (`:227`), Selector alternatives (`:283`, `:293`).
- `src/libexpr/paths.cc` — `allocSourceUnpinnedId` (`:45`),
  `getOrCreateRoot` (`:75`).
- Failing test: `tests/functional/tracing-eval-cache.sh` (flake
  cases at end).

## Open questions

- **Interaction with #174 (untangle `outerRootFSRoot` from
  `TracingCallbackArg` wrap gate).** #174 concerns the wrap-gate
  mechanism using `outerRootFSRoot` as a signal. Phase 1 removes
  the "wrap-time pinned SourceRoot" semantics that #174 pushes
  against; may fall out for free or become moot.
