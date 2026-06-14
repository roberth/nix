# Lazy paths

## Motivation

Nix has a requirement of hermeticity: the ability to know and collect
all inputs to the evaluation and the build. To make this practical,
Nix implements *fetching* in both stages.

Fetching in turn must be reproducible, and this extends to the
development workflow: local invocations should produce the same
results as if they were pushed and fetched. For this reason, Nix
flakes "fetch" their local sources, but this came at the cost of
performance.

While local fetching has been optimised significantly between Nix
2.18 and 2.35, the method of creating a store path with virtual
contents still has one significant limitation: the entire file tree
must be read and hashed. An alternative hashing scheme like
git-hashing is promising in its own right, but has challenges with
submodules and does not solve the `git status` problem of having to
read the entire workdir.

Fundamentally the performance and scalability problem is hashing
the entire git workdir or local directory. So instead of a content
address, we need something else to identify a source root.

### Thunked rope representation for strings (considered)

An earlier candidate considered was a lazy "rope" representation
for strings — strings built up from concatenated segments and
materialised into their byte form when something demanded it
(hashing, equality, propagation into a derivation input). This was
not the first approach attempted: strings sit on a more
performance-sensitive path through the evaluator than paths do, and
would perhaps be easier to use in ways that force the byte
materialisation anyway, causing the hashes to be computed regardless.

There is also a language-design constraint: thunk presence must
never be language-observable, leaving no option for user expressions
to handle lazy-fetched values with extra care if needed.

Experimentation with lazy paths surfaced a narrower target.
`builtins.genericClosure` is the essential function the module system
uses for collecting modules, and the comparisons it performs reduce
to path-path and path-string equivalences. Solving those two cases
inside `genericClosure` limits the amount of change needed to
existing Nix code.

### Paths and source roots: expressing intent

A path value names an abstract source tree at a subpath: an accessor
that knows how to resolve operations, plus a position within it. A
store-path subpath string also carries information about where its
root starts (at `/nix/store/<hash>-<name>`), but it has already been
placed into the system's filesystem — its meaning has shifted from
an abstract, relocatable source tree to something more specific.

Keeping the value abstract lets equality, comparison, and
structural operations like `dirOf`/`baseNameOf` decide without
materialising the tree. Placement into the store happens only when
something genuinely needs the placed form.

The abstract answers are also more honest. Historically
`baseNameOf sourceRoot` returned `<hash>-source` — an artefact of
the storepath the tree was rendered into, almost never what the
caller intended. Under the abstract form the root has no basename,
and `baseNameOf` reports that directly. When store-path semantics
are desired for a source root, they are a mere string interpolation
away.

The intent-carrying form also lets the runtime detect boundary
escapes: a `..` segment that pops past the accessor's root, or a
symlink whose target leaves the source tree. These surface as
explicit errors at the call site rather than being silently followed.

Path values also keep untracked-file reporting simple: the accessor
identifies which source tree a path lives in directly, with no
indirection through a stringified form to navigate. Fixable other
ways, but simple is good.

## Design decisions

### Kinds of `SourceRoot`

A path's abstract source root can come from different places — a
fetched tree, a real filesystem location, nix-internal scaffolding —
and the right `toString` behaviour differs in each. Existing Nix
expressions expect `toString` of a path value to be a unique,
deterministic identifier — one that can even be used to read from.
(Whether that was the original intent is questionable: paths lack
string context, and path values' interpolation already emphasises
copying. But expressions rely on being able to read anyway.) To
support each origin without forcing one shape on all paths, each
`SourceRoot` carries a kind tag distinguishing the categories of
value the paths under it represent:

- **System** — a real filesystem location. `toString` returns the
  absolute path; `${...}` copies the subpath into the store. Models
  `/etc/foo`, `/nix/store/X-source`, and other literal-rooted paths.
- **Copyable** — a fetched tree, or an in-language tree built via
  `builtins.makePath`. Both `toString` and `${...}` materialise the
  root once and render as `<storePath>/<subpath>`.
- **Internal** — nix-internal scaffolding (corepkgs, derivation
  helpers). `toString` is undefined and throws; positions resolve
  to `null`; copies are rejected. The kind exists so that leaking
  an internal value into language space fails loudly rather than
  rendering with internal coordinates.

The tag lives on the SourceRoot rather than on individual path
values. All paths under one accessor share its semantics, so the
kind decision is paid once at admission rather than re-derived per
operation.

### Comparison and deduplication

Nix's `<` family on path values is aligned with `toString`. The
language operators `<`, `<=`, `>`, `>=`, and `builtins.lessThan` all
defer to this comparison, and existing Nix code relies on the
resulting ordering. Preserving this alignment across paths from
different source roots requires computing the store path each would
render to.

`builtins.genericClosure` is the primitive the module system uses
to collect modules. Given a `startSet` of keyed elements and an
`operator` that produces more keyed elements from each, it iterates
the operator until no new keys appear. Until lazy paths, whether a
key was "new" was decided using the `<` family. However, the ordering of `<` was not
exposed through this function. The only observable behavioural
choice was laziness in keys: if the family of keys shaped
`[ "a" x ]` has only one occurrence, `[ "a" (abort "no") ]` is an
acceptable key.

When paths come from different source roots, `<` needs to force the
store path computation.

Deduplication's minimal constraint is identifying equivalent keys,
not ordering them. The ordering between non-equivalent keys is not
exposed to callers in any case. A dedup comparator that agrees with
`<` on equivalence is enough — and may skip the store path
computation whenever the equivalence can be decided cheaply.

Since the actual ordering is not exposed, the dedup comparator is
free to pick a more efficient one that is not even stable across
`genericClosure` invocations. It only needs to be a valid total
ordering for the duration of the `genericClosure` computation.

### Source root equivalence

Whether two source roots would render to the same `toString` is a
distinct subproblem from the comparator itself. Four mechanisms
answer it, in increasing cost:

- **Pointer identity** — two source roots from the same fetch call
  are the same source.
- **Fingerprint** — an optional identifier for the input that
  produced an accessor. When both sides expose the same fingerprint,
  the `toString`s are guaranteed to agree.
- **Probe** — a cheap structural check, for example a SHA256 over
  the trees' top-level entry names, or over an arbitrary subpath
  deemed relevant by the caller. Mismatches are decisive negative
  evidence; matches are inconclusive.
- **Store path computation** — when no cheaper mechanism decides,
  materialise both sides and compare the resulting storepaths.

Three of the four mechanisms are inequality probes — a single
mismatch decides. An equality probe would have to run on every node
pair, multiplied across source-root comparisons. Even with per-file
hash caching the total exceeds ingesting each tree once.
Fingerprint is the only cheap positive proof; everything else routes
to the store-path computation, so that the complete probe is
preserved for reuse as a content hash.

Each mechanism is decisive or inconclusive — never wrong.

### `pathEquivalent` and the module system

The module system historically called `toString` on every module
reference because `genericClosure` rejects cross-type comparisons,
and references could be path values or strings. The `toString`
normalisation made dedup possible — at the cost of materialising
every path-valued reference.

Lazy paths invites dropping `toString`, but the type restriction
remains. Lifting it alone is not enough either: `toString` was doing
real work — treating similar paths and strings as equivalent for
import resolution.

`genericClosure { pathEquivalent = true; }` does both. It admits
cross-type comparisons between paths and strings and uses
`toString`-equivalence as the comparator. The module system can drop
the coercion without losing the equivalence it relied on.

## The materialisation diagnostic

The evaluator can report when a Copyable root is materialised. The
setting is named `lint-fetch-whole-source-to-store` to fit Nix's
existing diagnostics infrastructure; the user-facing semantics is
closer to a log setting with an optional terminate-and-stack-trace
mode.

The priority is truth. The diagnostic reports materialisations as
they happen — never silently, never falsely. Tripping it is not
itself a problem; it is the infrastructure telling you what the
evaluator did. What to do with that information is yours.

The expected usage pattern is temporary investigation: run at `warn`
to see what materialises, switch to `fatal` to get a stack trace at
the offending site, fix or accept the cause, then lower the setting.
Users do not enable `fatal` permanently — `toString` is a
materialisation point by language semantics, so any code that
genuinely needs the materialised form will trip the diagnostic by
design.

The ill-fitting "lint" framing can create a temptation to suppress
the diagnostic in cases where materialisation looks unavoidable.
This is never a goal. The user expects every whole-source
materialisation to be reported; if complete avoidance is not
possible for some code path, the user will lower the setting after
investigation. That is the intended flow, not a reason to make the
diagnostic ineffective.
