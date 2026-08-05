# SourceRoot integration for OuterObject wrap

Design + landed-state notes for the SourceRoot integration.
Phase 1 landed 2026-08-05. Phases 2–3 remain.

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
  Input) → gets `"anon#<n>"` from a per-EvalState counter (Phase 1,
  landed). Same bounded-stability tradeoff as `<url>#<n>`: reproduces
  within-process under matching-until-divergence, misroutes if
  cross-process admission order shifts. Selector-based imprinting
  (Phase 3) is the stronger direction; anon numbering unblocks the
  interim.

Folding `kind` into the identifier keeps the wire format single-
field and moves the classification from a tag to a string prefix
convention. Selector-based imprinting also produces a
distinguishable string, so both admission schemes coexist in the
same identifier space.

## Proposal — phased

**Phase 1 — Wire-format extension for identifiable SourceRoots (landed).**

- `WHNFPath` grows `std::optional<std::string> sourceRootId`. JSON
  + CBOR codecs updated (CBOR piggybacks on JSON via
  `nlohmann::json::to_cbor`).
- `computeWHNFFromObject` stamps the identifier via
  `state.stableRootIdentifier(*rp.root)`. Internal-kinded roots
  throw here — a design invariant, not an ergonomic limitation.
- `EvalState::stableRootIdentifier` returns `"system"` /
  `"<url>#<n>"` / `"anon#<n>"` / nullopt (Internal only). Reverse
  lookup (`getRootByIdentity`) populated at admission in
  `getOrCreateRoot`. Per-EvalState — no canonical owner.
- `OuterObject::getPath` / `TCA::getPath` / `RCA::getPath` route
  through the shared `reconstructPathFromWHNF` helper: look up the
  identifier, panic if absent (Internal was already refused at
  record; nullopt here is a stamping-side bug), throw with a clear
  error if the identifier isn't in this process's rootByIdentity.
  Correct-or-miss — never substitute a stand-in root.
- Two libexpr-tests pinning the wrap-layer round-trip for stamped
  and anonymous roots. `tests/functional/tracing-eval-cache.sh`'s
  flake cases pass.

Known Phase 1 limitation — **nested-cache state mismatch**. In the
`builtins.cache` primop path, `OuterObject` holds the inner
EvalState (that's what `wrapArgAsCallbackScope` receives), but the
SourceRoots for outer-side values were admitted on the outer's
state. Identifier lookup misses cleanly. Doesn't affect the flake
test (single top-level state). Needs wrap-plumbing to thread the
outer's state through — see task #174.

**Phase 2 — Selector Q-hash disambiguation.** `SelectorImport{std::string
path}` and `SelectorExpr{expr, baseDir}` also lose SourceRoot info.
Two flake inputs each with `/flake.nix` produce identical Selector
Q hashes — wrong-hit risk at warm cross-invocation reuse under
matching-until-divergence. Add the same `sourceRootId` field to
those Selector alternatives so their content hashes carry the
SourceRoot identity too. Not required for the cold failure;
required to keep Foundational 1 (no wrong hits) under warm cross-flake
reuse.

**Phase 3 — Selector-based imprinting for cross-session stability.**
Phase 1's `<url>#<n>` and `"anon#<n>"` numbering is admission-order-
dependent: a warm session with a different query shape than cold
gets shuffled counters and misroutes identifiers. Selector-derived
identifiers (imprinted at the arg's cell scope via a
`SourceRoot* → Selector` map) reproduce whenever the arg's
*structure* is stable — a stronger guarantee that matches the eval
cache's overall identity philosophy: value identity from observation
history, not live-side allocation. Deferrable until Phase 2 is
green and workloads with observed cross-session drift make the
cost/benefit concrete.

**Phase 4 — Additional regression tests.**

- End-to-end in `tests/functional/`: `builtins.cache` + `fetchTree`
  combo that would exhibit SourceRoot loss (needs the Phase 1
  nested-cache limitation resolved to be constructable as a small
  test — the flake case is currently the only end-to-end coverage).
- Phase 1e (Internal refusal) has no direct test — the error
  wording could regress silently. Cheap addition.

## Starting points (Phase 2+)

- `src/libexpr/include/nix/expr/trace-types.hh` — Selector
  alternatives to extend with `sourceRootId` for Phase 2:
  `SelectorExpr`, `SelectorImport`.
- `src/libexpr/paths.cc` — `stableRootIdentifier` /
  `allocSourceUnpinnedId` — the naming scheme both Phase 2 and
  Phase 3 will build on.
- `src/libexpr/tracing-object.cc` — `computeWHNFFromObject` +
  `reconstructPathFromWHNF`. Phase 3's Selector-imprinting fits
  as an alternate identifier source consulted before
  `stableRootIdentifier`.
- Phase 1e (Internal refusal) test — add cheap regression to
  guard the error wording.
