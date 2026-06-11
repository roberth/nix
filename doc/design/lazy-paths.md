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

#### The kind is chosen at the admission seam

An earlier iteration asked the accessor for the kind, through a
virtual on `SourceAccessor`, and dispatched per path. Two problems
follow from that shape. The IO base class carries language-level
concepts it has no use for. And the per-path answer is not sound for
its consumers: the Copyable arm of `coerceToString` materialises the
accessor's *root*, which is only correct if that root is the
Copyable tree. A path rooted at the whole filesystem that reached
the Copyable arm through union or mount routing would have copied
the whole filesystem.

The kind therefore belongs to the site that exposes an accessor as a
language-visible path root, not to the accessor. One accessor can be
admitted more than once under different kinds. A posix accessor is
System when it is the filesystem view and Copyable when it is a view
of one store path. Each admission produces its own `SourceRoot`, and
`SourceAccessor` stays a pure IO surface.

Two consequences are load-bearing:

- libutil declares `SourceRootKind` as an opaque scoped enum with a
  known size, and libexpr defines the values. libutil holds and
  propagates the tag but never inspects it.
- `SourceRoot` has no default constructor and is constructed only
  through a factory that returns a shared reference. Consumers extend
  its lifetime through `shared_from_this`, which requires shared
  ownership to exist already, so a stack-allocated instance would
  fail at run time. A pass-key on the constructor turns that into a
  compile error instead.

The per-evaluation cache of admissions is keyed on the accessor *and*
the kind. Keying on the accessor alone would let the first
admission's kind win for every later lookup, which makes the cache a
claim about the data model rather than an optimisation.

Path values carry a raw pointer to their `SourceRoot`, in the same
slot that previously held a raw accessor pointer. The admission cache
is what keeps those roots alive for the evaluation. Equality of a
rooted path stays structural on path, kind and accessor, so two
separate admissions of the same pair compare equal whether or not
the cache handed out the same object.

### A path built by concatenation keeps the first operand's root

`path + "string"` constructs a path value, and the accessor under that
value decides how the bytes are read. The historical implementation
re-rooted the result on the filesystem, which is a no-op for a literal
`/foo/bar` and wrong for anything else: the result names the right
location and reads through the wrong view. The visible symptom is
`builtins.readFile (input.outPath + "/subfile")` failing, because the
rendered path is not on the filesystem until something forces a store
copy.

The accessor is therefore captured from the first element at the top
of the concatenation and threaded into the result.

### The shape of a fetched tree and a node inside it

Several lockfile nodes can live in one source tree. That is what a
relative path input is: it shares its parent's store object at a
different subdirectory. Encoding a node as one flat record of store
path, accessor and subdirectory leaves that relationship implicit, as
two records that happen to hold equal fields. Two types make it
structural:

    struct MountableTree { std::optional<StorePath> storePath;
                           fun<ref<SourceAccessor>()> accessor; };
    struct NodeLocation  { MountableTree tree; std::string subdir; };

A relative input's location holds its parent's tree value, with a
different subdirectory.

`MountableTree` and not `MountedTree`, because the accessor is a thunk
and the store path may be unknown: holding one does not commit to any
materialisation having happened. The store path is optional for that
reason, so the type can represent both the fetched-and-known case and
the accessor-only case honestly.

Rendering, though, takes an explicit flag and does not dispatch on
whether the store path is known. The rendering shape is what the
caller intends, not a property of the data. A consumer that holds a
tree with an unknown store path, `nix flake archive` for instance, is
not asking for a path-typed value; it is acknowledging that the store
path has not been computed. Conflating the two would put rendering
policy inside a data type that has no business holding it.

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

#### The definitive comparator

The comparator behind the language operators is allowed to
materialise, because that is what the language semantic says. It
still decides most pairs without materialising:

1. An Internal operand is rejected. An Internal path has no
   `toString`, so it has no place in the order. The error is the one
   `coerceToString` raises, so the two agree.
2. Two paths on the same accessor compare by subpath.
3. Two accessors with the same root fingerprint compare by subpath.
4. A System path against a Copyable one compares against the store
   directory prefix. A System absolute path that sorts before the
   store directory is less than every Copyable path, and one that
   sorts after it and is not in the store is greater. A System path
   inside the store directory has no cheap answer.
5. Otherwise both sides render to their printable form and the
   strings are compared. This is the only case that materialises a
   Copyable root, at the same point `toString` would have.

#### The dedup comparator

The comparator behind `genericClosure { pathEquivalent = true; }`
never materialises. It composes three relations in order:

1. Each value is placed in one of two groups. One group holds the
   values whose `toString` has the canonical
   `<storeDir>/<base>[/<subpath>]` shape: Copyable paths, and strings
   of that shape. The other holds everything else. The discriminator
   is the shape of the rendered form, not whether the value is a
   string or a path, because a store-path-shaped string renders the
   same as a Copyable path would.
2. Within a group, the operative subpath is compared.
3. Within a group and subpath, the accessor's equivalence-class
   identifier is compared. Two distinct accessors whose trees are
   equivalent receive the same identifier, so the classes match
   `toString` equivalence even across separate admissions of one
   tree.

The dedup state is a single map ordered by this comparator, so
collapsing happens inside the map insertion. An earlier design kept
one map per key type and scanned across the maps on each insert to
find cross-type equivalents. That scan is quadratic in the mix of
path and string keys, and it also missed nested keys: a list or
attrset whose leaves collapse under `toString` equivalence needs the
comparator to recurse, which the per-type maps did not do.

Routing every key through one comparator also settled two answers
that the per-type maps had been giving wrongly:

- An Internal key raises an error even when it is the only key. The
  per-type design deduplicated Internal keys by accessor identity,
  which asserts equivalence under a relation that is undefined for
  Internal. Using an Internal path as a `pathEquivalent` key is a
  usage error and says so.
- A store path that is named by a string key but is not reachable
  raises an error. The per-type design returned "not equivalent",
  which is a guess: the store path could name a tree that would have
  been equivalent once realised. Comparisons must not return wrong
  answers, so this one raises.

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

#### The probes, in the order they run

Seven concrete steps implement those four mechanisms. Two of them are
caches rather than mechanisms of their own, and the probe category has
two members, one over the root directory's entry names and one over a
file the caller names.

1. Pointer identity. Decides equivalent.
2. A cache of pairs already proven inequivalent. Decides
   inequivalent. The key is the pair of accessor addresses, ordered
   so that a reversed call finds the same entry.
3. Fingerprint equality, together with equality of the rebased
   paths. Decides equivalent. Two accessors that expose one
   fingerprint string but rebase a path differently are two
   positions inside one fingerprinted tree, not one position.
4. A query of the store path cache that does not populate it. When
   both sides already have a store path recorded, the comparison of
   those two store paths decides either way. The last probe would
   reach the same answer, but deciding here skips the probes in
   between, which matters when an unrelated part of the evaluation
   has already materialised both trees.
5. SHA256 over the names of the root directory's entries, in
   lexicographic order, separated by NUL bytes. A mismatch decides
   inequivalent. A match says nothing about the children. The
   separator keeps the encoding injective, so that `{"a", "b"}` and
   `{"ab"}` cannot hash alike. The result is cached per accessor.
6. SHA256 over the contents of the file at a subpath the caller
   supplies. A mismatch decides inequivalent. A match, and an
   absence on either side, say nothing. The result is cached per
   accessor and subpath, including the absence.
7. The store path of each side, computed if it is not already
   known. Decides either way.

Only the last mechanism walks a tree, and its result is recorded, so
each accessor is walked at most once per evaluation however many
comparisons name it. Across N accessors the cost is therefore N
walks, where comparing contents pairwise would be N².

The subpath probe assumes the caller is about to read that subpath
on both sides anyway. The dedup comparator reaches accessor
comparison only after it has established that the two values share
a subpath, so it passes that subpath and the read is work the
comparison was going to do. A root-level comparison passes nothing,
because the probe only reads regular files.

The caching of inequivalence, and not of equivalence, follows from
where the work sits. An equivalent pair already has both store paths
recorded, so re-comparing it is two lookups. The cheap positive
proofs are free to re-check. Caching them would add state without
saving work.

#### The fingerprint contract

Equal fingerprints promise equal contents. Different fingerprints
promise nothing: two unrelated accessors over identical trees may
expose different fingerprints, or none.

Implementations must derive a fingerprint from content, for example
a NAR hash or a git revision. A fingerprint derived from a mutable
label, a URL, or a coordinate recorded at fetch time satisfies the
wording and breaks the contract, because it lets the comparison
answer "equivalent" for trees that differ. The Copyable admission
seams hold to this by construction.

#### An equality probe was tried and removed

An earlier iteration compared two trees node by node, with a
fingerprint shortcut in front and an optional single-node
discriminator. It answers correctly. It is the wrong shape for the
callers:

- Its cost is per pair. A dedup loop over N accessors compares each
  new accessor against every existing class representative, so the
  walks are N² where the store path computation is N.
- It produces nothing reusable. The store path computation leaves a
  content hash behind that every later comparison, and every later
  coercion, reads instead of walking.
- One of its three callers asks a one-sided question ("does this
  accessor materialise to the store path named by this string?").
  Feeding that through a two-accessor comparison required
  synthesising an accessor over the store path, which was then
  re-hashed on every call because it carried neither a fingerprint
  nor a filter.

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

`builtins.isPathEquivalent a b` exposes the same relation as a
predicate, over path against path, path against string, and string
against string. On path against path it agrees with the language's
`==`.

One known limitation: `pathEquivalent = true` selects a semantic
(cross-type equivalence in place of type-strict equality) and a cost
model (lazy in place of eager) with one flag. Separating them is
left for a later iteration.

### Positions

A position attrset carries `.path` next to `.file`, `.line` and
`.column`. `.path` is the origin as a path value.

The module system identifies modules through
`builtins.unsafeGetAttrPos`. With `.file` alone, doing so forces a
render of the origin, and rendering a Copyable origin materialises
the whole tree. `.path` stays a path value, so a caller compares it
with `==` or `builtins.isPathEquivalent`, appends subpaths to it, or
feeds it to `genericClosure { pathEquivalent = true; }`, and nothing
is copied. `toString` of `.path` reproduces `.file`.

`.file` itself is a thunk. It renders through the same kind dispatch
as `toString`, so a System origin renders as an absolute path and a
Copyable origin renders as a store path plus subpath, which is a
location a reader can open and `nix edit` can follow. An eager render
would have produced the accessor-internal form for a fetched flake, a
path like `/flake.nix` that means nothing outside the accessor.

An Internal origin makes the whole position attrset `null`, `.path`
included. Leaking the evaluator's own source layout into language
space is an impurity, and the position has no meaning outside the
running evaluator.

## Boundary escapes

A path value names a position inside a source tree. An operation
that leaves the tree has no meaning under that name, and
`CanonPath`'s historical handling of `..` hides the departure: the
removal of `..` is purely lexical, so a pop past the root
disappears, and the path then names some other file inside the tree.
The same is true of a symlink whose target leaves the tree.

For a System root that behaviour is what the user expects, because
the root is the real filesystem and the clamp at `/` matches what
the operating system does. The long-standing `./foo + "../bar"`
idiom depends on it. For a Copyable root there is no such
justification: the clamped location is whatever happens to live at
that place inside the fetched tree.

Three parts handle this, at three different points.

### The concat site

When the first operand of a path concatenation is rooted at a
Copyable accessor, the joined string is walked lexically and the
result is rejected if `..` would pop past the root. No accessor is
read.

An earlier sketch resolved symlinks against the accessor during the
concatenation. That was rejected: path concatenation is a pure
language operator and must not read a filesystem. Counting depth
over the components needs no reads, and it catches the case where
the user wrote `..` in the suffix, which is both the common case and
the cheapest to catch.

A System first operand keeps the silent clamp.

### The read sites

Symlink resolution takes the boundary policy from the root's kind:

- Copyable rejects a `..` that pops past the root, whether the `..`
  came from the input or from a symlink target spliced into the
  walk. It also rejects a symlink whose target is absolute.
- System uses the lenient walker, with no boundary check.
- Internal does not reach the wrapper. The few internal paths that
  read Internal-rooted files call the bare resolver. A reach through
  the wrapper terminates the process, rather than raising, because
  every raise shape available is catchable by an existing handler
  upstream and would be swallowed.

The wrapper is applied at three choke points, which between them
cover every user-reachable read: the resolution that `import`
performs, the resolution that `readFile`, `readDir`, `pathExists`,
`readFileType`, `hashFile` and `readSymlink` share, and the walk over
ancestors inside the copy to the store, which is what `"${p}"`
triggers.

Rejecting an absolute symlink target in a Copyable tree is what
makes the tree relocatable. Before materialisation, a target of
`/sibling` resolves against the accessor's root. After
materialisation the same file in the store points at the real
`/sibling`. Two different files are read at one symlink depending on
whether the tree has been materialised, so the symlink is refused at
admission instead.

The concat check and the read-site policy cover different shapes, and
neither covers both. The concat check sees a literal `..` in the
suffix and fires before any read. The read-site policy sees a symlink
whose target escapes, which carries no literal `..` at all.

The resolution mode is a second such split. Resolving to a directory
whose trailing component must stay intact leaves that component
unresolved, so an escaping symlink in the trailing position is
invisible to that call. It is caught later, at the read site, which
resolves the trailing component too.

### The flake input sites

A relative flake input, and a `?dir=` on any input, are composed by
the locking machinery rather than by a user expression. Those sites
catch the generic boundary error and raise one that names the input,
because the user wrote an input URL and not a path expression.

There are five such compositions: the relative path itself, the
`?dir=` applied when the flake is read, the `?dir=` applied to a
relative input in each of its two branches, and the `?dir=` applied
by the compatibility shortcut for an input that is already in the
store. All five use one helper, so the policy, the resolution mode
and the admission live in one place. The helper takes the resolution
mode with no default, because the choice is structural and a default
invites the wrong one.

### The string boundary

The kind belongs to the path value. Interpolating a Copyable path
into a string produces a plain string, and reading that string back
goes through the filesystem root, where `..` clamps lexically as it
always has. None of the checks above fire across that boundary.

This is the intended design. A user who converts to a string has
asked for the historical behaviour.

### Preserving the kind through coercion

Three read sites picked the kind from the type of the value they
received, and fell back to System for an attribute set. Under lazy
paths `{ outPath = <Copyable path>; }` and
`{ __toString = self: <Copyable path>; }` are ordinary shapes, so
the fallback selected the lenient walker and skipped the check
entirely.

Coercion to a path therefore has a kind-preserving form. It peels
`__toString` and `outPath` the way the plain form does, and returns
the root along with the path. A string in either slot is admitted
under the filesystem root, which is the documented contract for an
`outPath` string.

## Materialisation and laziness

### Recording a hash without mounting a tree

Mounting a fetched input does three things: it walks the tree to
compute a NAR hash and verifies it against any assertion the user
wrote, it allows the predicted store path, and it mounts the
accessor so that the predicted store path resolves to the tree.

The second and third only pay off when downstream code reads the
tree through store-path-rooted paths. Flake loading under lazy paths
does not: a path value stays rooted at the fetcher's accessor and
reads go through it. Locking an input is therefore the first part
alone, with no mount and no allowlist entry.

The hash itself is then deferred. It is installed as a lazy
attribute whose force does the walk and the comparison. Every
consumer reads it through the same forcing accessor, so a mismatch
is still detected, at force time rather than at lock time. An input
that is locked and then only read as a path value is never walked.

Two things are necessary for that to be a win rather than a loss:

- The lazy attribute memoises. Without it, each force walks again for
  an accessor with no fingerprint, where the eager form walked once
  and kept the string. A throw leaves the memo unset, so a mismatch
  is raised on every force.
- A hash computed elsewhere is recorded where the lazy attribute
  looks for it. The copy to the store computes a hash anyway, so it
  records it. A locked input then costs no walk at all.

The lazy attribute does not write its own result back into that
record. A hash derived from a formula names a store path that need
not be valid in the store yet, and a later reader of that record
would treat it as a materialisation that has not happened.

Emission to the language follows the same rule. The attribute set
that `fetchTree` returns emits the hash as a thunk on the Nix side,
so an expression that never reads `.narHash` never causes the walk.

### Verifying a user-supplied hash synchronously

When the user writes `narHash` on a fetch, the walk happens at lock
time and the comparison is made there, even though the walk is
otherwise deferred.

The eval cache records values derived from the predicted store path
before the end-of-evaluation pass runs. Deferring the comparison to
that pass would let a wrong hash be recorded first, and later
evaluations would read the recorded value back without re-checking.
Verifying at lock time is the only point before that recording. It
is also cheap, because a warm fingerprint makes the walk a lookup.

An input whose hash comes from the lockfile, without the user
repeating the assertion, is trusted and its store path is derived
from the hash by formula. No walk.

### What the abstraction gives up

The abstract form is observable in a few places, and each answer
below is deliberate.

`toString p` and `"${p}"` agree on a Copyable root and diverge on a
subpath under one. `toString p` names the tree's store path with the
subpath appended, which is the "name this exact location" meaning.
`"${p}"` copies the addressed bytes and names the store path of
those bytes, which is the historical meaning of interpolation. The
cost is the same either way.

`toString p` on a Copyable path produces a string with no context,
as it always has for path values. The materialisation that the
render performs is therefore not recorded in the output context, and
the resulting store path is registered so that a later reader of the
string can resolve it.

A Copyable path is refused as a non-first operand of a path
concatenation. The first operand sets the result's root, and a
suffix under it is a meaningful position. A Copyable operand in a
later position would have to be rendered into the middle of a path,
and the result addresses nothing. String interpolation is the
replacement.

`dirOf ./.` is `/` and `baseNameOf ./.` is empty for a Copyable
root, where both previously reported the store path and its
basename. The accessor's root has no parent and no name.

`baseNameOf` reads the canon path and not the rendered string. The
trailing component of a path does not depend on what the prefix
renders to, so the two agree wherever the path has a trailing
component, and rendering the prefix walks the tree. The one place
they disagree is the root, where the canon path has no trailing
component and the rendered form ends in `<hash>-source`. That
`<hash>-source` was an artefact of the render. A caller that needs a
name passes one, through the surrounding derivation's `name`
attribute.

Two primops take the rendered form for a Copyable source, and each
has a reason. `builtins.storePath` renders its argument to a string
rather than reading the bare path value, because rendering a
Copyable-rooted path is what names its store path, and the bare path
at the accessor root would be tested against the store directory and
fail. `builtins.path` and `builtins.filterSource` hand the filter
callback the string `toString` would produce for the file, rather
than the accessor-relative path: filters are written as regular
expressions built from `toString` of the source, and an
accessor-relative argument matches none of them. The accessor still
drives the walk. Only the string handed to the filter changes.

A flake's `outPath` stays a string. It has to name both the root of
the source tree and the position inside it, which a store path does
and a path value does not: `flakePath + "/relative"` names a copy of
the subdirectory and loses the root, which breaks the import of a
sibling file.

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

### Where it fires, and where it does not

Two sites can ingest a whole source: the copy of a tree into the
store, and the compute behind the deferred NAR hash.

The copy reports before it consults its cache. A warm cache would
otherwise hide the call site from the developer who armed the
setting, which is the one thing the setting exists to show.

The hash compute reports after its cache check. A hit there means
another call site already paid for the walk and has already
reported it, so reporting again only inflates the count.

Two conditions gate the report. The root must be Copyable, because a
System path is a location the user named. And the subpath must be
the root, because a per-file interpolation materialises one file and
not "the entire contents" the message describes. Per-file
interpolations are also intrinsic to lazy paths: reporting them
produces hundreds of messages for one realistic evaluation and
buries the whole-tree cases.

The report carries the subpath the coercion started from, not only
the accessor root. Knowing that the import of one file forced a copy
is what makes a real hit in a large evaluation actionable; knowing
that "something forced a copy" is not.

### The eval cache is refused, not bypassed

When the setting is `warn` or `fatal`, the flake eval cache is
refused with an error that names `--no-eval-cache`. A warm cache
returns a recorded value without evaluating, so it reports nothing,
which is indistinguishable from a run that materialised nothing.

The refusal is scoped to the combination of a used cache and a pure
evaluation, which is when the cache is consulted.

## Compatibility

The lazy shape of `outPath` breaks consumers that walk it
structurally. `lib.fileset` reaches the store path and its parents
through `lib.path.deconstructPath`, and stops at the accessor root
instead. Filters built from a regular expression over
`toString pkgs.path` are given an accessor-relative path and match
nothing. These consumers were written against the earlier contract,
and a flake that predates lazy paths must not be moved onto the new
shape because its user upgraded Nix.

The shape is therefore selected per flake, by four declarations,
from the most specific to the least:

1. `inputs.<name>.copyToStore`, the root flake's declaration about
   one input.
2. `inputs.self.copyToStore`, the root flake's declaration about
   itself.
3. A marker file at the flake root,
   `/.nix-flake-lazy-paths-supported`.
4. `flake-default-copy-to-store`, the user's ambient setting.

The default of that setting is the eager shape, because that is what
an existing flake was written against. Absence of the marker
therefore means the eager shape unless the flake says otherwise. The
marker and the two `copyToStore` attributes are the only way to reach
the lazy shape, and each of them names a flake that has declared it
works under the lazy shape. The polarity is the point: a flake that
never heard of lazy paths keeps the contract it was written against.

The marker file exists because the first two cannot reach an
existing flake. `inputs` is a closed structure, so an older Nix
rejects an unknown attribute in it, and a flake that wants to be
evaluated by both versions cannot use one. An unknown file at the
root is ignored by every version. The name is flat and at the top
level because the directory read that finds `flake.nix` and
`flake.lock` already covers it, where a nested marker would add a
read.

The marker is a declaration by the author, that the flake's outputs
work under lazy paths. `flake-default-copy-to-store` is a preference
of the user. They are separate surfaces because they answer
different questions.

Two properties of the eager shape are worth stating, because the
first iterations got them wrong:

- `copyToStore` is a flakes-level concept. It is not an attribute of
  a fetcher input, and routing it through the fetcher attribute set
  conflated the layers. It lives on the flake.
- `outPath` is a store-path string in both shapes. The visible
  contract says so. Only the path used for `import` changes, so that
  `./.` inside the imported `flake.nix` resolves under the store
  path with System semantics.

Anything that changes the meaning of the evaluated outputs has to be
part of the eval cache fingerprint. The setting and the marker both
do, so both are appended to it. The per-input flags need no separate
entry: they live in `flake.nix`, whose hash is already part of the
fingerprint of that input.

## Identifying a source root across versions

The eval cache keys recorded values on the inputs they were derived
from. For a path value the key has to name the source root, and it
should name it in a way that survives a re-resolution of the same
input, so that a cache entry is still reachable after the locked
revision moves.

An input therefore has an unpinned URL: its URL with the outputs of
version resolution removed (`rev`, `revCount`, `narHash`,
`lastModified`, and the final marker). The reference is kept,
because it names which branch to track, which is part of the
specification and not of the resolution.

A `SourceRoot` carries that URL as an optional identifier, stamped
by the site that admits the accessor. The first stamp wins for a
given admission; a later registration with a different identifier is
ignored. Absence means no claim of identity, and is reserved for
Internal helpers, whose content is deterministic, and for throwaway
probes, which never enter the registry.

The cache key is the identifier plus a counter scoped to the
evaluation, because two accessors can share a URL and must not
collide. The counter is in memory and per evaluation. The
identifiers are therefore not stable across processes, and do not
need to be: a replay re-runs the evaluation, and a deterministic
evaluation order produces the same sequence.

The filesystem root is stamped with the literal `path:`, so that a
System-rooted path takes part in identifier-keyed caching rather than
falling out of it.

Tracking Copyable accessors this way is a correctness requirement,
not an optimisation. Without an identity for the source root, the
cache cannot be invalidated when the source changes.

## Constructing path values in the language

`builtins.makePath` builds a path value whose contents are described
by an attribute set. A read walks that description and forces only
what it needs to answer. A path-valued node mounts a foreign
accessor at its position.

Two uses follow. A source whose cost scales with what it includes
rather than with the size of the directory it sits in. And a source
assembled from parts, where the parts are named in the language
instead of being selected out of a directory by a filter.

`builtins.readSymlink` returns a path value rather than a string, so
that the result of following a link stays inside the same source
tree and keeps its kind.

## Open questions

These are recorded here so that the same item is not repeated in
several commit messages.

- Caching a computed store path on the `SourceRoot`. The definitive
  comparator pays one store path computation per root, and the
  per-(accessor, subpath) cache already collapses repeats within one
  root. A sort over paths from many different roots still recomputes
  once per root, which a field on the root would remove.

- Whether `fetchTree { lazy = true; }` should apply a `?dir=` query
  parameter to the path it emits. The eager branch applies it through
  the store path derivation, so a lazy `outPath` points at the
  accessor's root where the eager one points at the subdirectory. The
  alternative is for the caller to compose the subdirectory. Either
  answer needs a pin in `fetchTreeLazy.sh`.

- Exposing a lazy `self.outPath`. A flake's `outPath` stays a string
  because it must name both the tree root and the position in it. A
  second attribute that carries the path value would let a consumer
  stay lazy without losing either, and needs new flake machinery.
