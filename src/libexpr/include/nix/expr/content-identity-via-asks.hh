#pragma once
/**
 * @file
 * Content-defined identity computed as a pure function of subject
 * and factset. See `doc/design/tracing-eval-cache-content-identity-via-asks.md`.
 *
 * Content ids are not stored anywhere; they're computed on demand
 * from a value's static structural identifier (the "subject") and
 * the current factset. The recorder and the walker call the same
 * function with the same arguments and obtain identical hashes.
 *
 * This module is currently standalone — not yet wired into the
 * recorder/walker. It exists to validate the math in isolation
 * before migration.
 */

#include "nix/expr/trace-types.hh"
#include "nix/util/hash.hh"

#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace nix::cidasks {

struct Subject;

/** A cb arg at a static apply-stack depth N (reverse De Bruijn).
    The leaf of every Subject tree. */
struct PositionalSeed
{
    int depth;
    auto operator<=>(const PositionalSeed &) const = default;
};

/** A value derived from a parent subject via a producer query
    (getAttr by name, or getListElem by index). */
struct DerivedSubject
{
    std::shared_ptr<const Subject> parent;

    enum class Kind {
        GetAttr,
        GetListElem,
    };

    Kind kind;
    std::string name;  ///< meaningful for GetAttr
    size_t index{};    ///< meaningful for GetListElem
};

/** A value reached by applying a fn subject to an arg subject. */
struct ApplyResultSubject
{
    std::shared_ptr<const Subject> fn;
    std::shared_ptr<const Subject> arg;
};

/** Escape hatch for values whose structural Subject isn't reachable
    from a positional handle — typically apply-result args that come
    from raw inner Values (not CdiObject-wrapped). Carries the
    value's content id directly; observations on this value still
    XOR-fold into its content id at the relevant factset point. */
struct OpaqueContentSubject
{
    Hash hash;
};

struct Subject
{
    std::variant<PositionalSeed, DerivedSubject, ApplyResultSubject, OpaqueContentSubject> data;
};

/** A single observation reduced to the two hashes contentIdAt needs.
    `fromHash` is the content id the query was issued against;
    `elementHash` is SHA-256(reqHash || respHash) — the v13 H_element.
    Named `Observation` to match the doc's per-Asks-edge "facts about V"
    membership language (= each element is one observed (req, resp)
    against a subject identified by `fromHash`); distinct from the
    Asks-level `factSetHash` (= XOR-fold of all `elementHash`es, no
    `fromHash` distinction). */
struct Observation
{
    Hash fromHash;
    Hash elementHash;
};

/** An Asks edge's worth of observations. Observations in one edge are
    dispatched against a single shared precondition factset; their
    `from` fields all refer to subjects' content ids at that precondition. */
struct Edge
{
    std::vector<Observation> observations;
};

/** Build an Observation from a QueryVariant/ResultVariant pair. Used by
    the writer at flush time where it already holds the variants. */
Observation observationFromQR(const trace::QueryVariant & query, const trace::ResultVariant & result);

/** Compute the content id of `subject` after walking through all
    `edges`, inheriting `scope` (the XOR of outer-scope CDIs — e.g.
    CDI(Q) at the cb-apply boundary). Passing the zero hash for
    `scope` gives the pure structural content id, equivalent to
    no inheritance.

    Inheritance applies at the leaf: `PositionalSeed` and
    `OpaqueContentSubject` XOR `scope` into their base hash.
    `DerivedSubject` and `ApplyResultSubject` propagate `scope`
    via their constituents' (recursively scope-aware) content ids,
    so the structural derivation incorporates inheritance naturally
    via the constituents' `from`-field values. */
Hash contentIdAfter(const Subject & subject, const Hash & scope, const std::vector<Edge> & walk);

/** Compute the content id of `subject` at the precondition of the
    edge at index `edgeIndex` in `walk`, inheriting `scope`.
    `edgeIndex == 0` means the initial precondition (= empty
    factset); `edgeIndex == walk.size()` means the postcondition of
    the whole walk (equivalent to `contentIdAfter`).

    **Argument-level only.** Per the design (Principle 3, per-arg
    centralization), only argument-level subjects bear CDIs:
    `PositionalSeed` (cb_arg seed, evolves via own-loop),
    `ApplyResultSubject` (composes constituent argument CDIs), and
    `OpaqueContentSubject` (escape hatch). `DerivedSubject` does not
    have a CDI — observations on derived values fold into the cb_arg
    root's own-loop and the derived value is referenced via
    `(root_cdi, path)`. Passing a `DerivedSubject` traps; callers
    that want a content-addressed identifier for any Subject
    (including derived) should use `structuralAddress` instead. */
Hash contentIdAt(const Subject & subject, const Hash & scope, const std::vector<Edge> & walk, size_t edgeIndex);

/** Compute a content-addressed structural identifier for any
    `subject` — including `DerivedSubject`, where `contentIdAt`
    traps. For non-derived subjects this delegates to `contentIdAt`.
    For `DerivedSubject` it returns the producer query's hash:
    `qH(QueryGetAttr{name, from = root_cdi, fromCIDs, path})` for
    `GetAttr`, similarly for `GetListElem`. Used by `AmbientObject`,
    `TracingLocalObject`, etc. to expose a single-`Hash` identity
    handle even though derived values don't have CDIs proper. */
Hash structuralAddress(const Subject & subject, const Hash & scope, const std::vector<Edge> & walk, size_t edgeIndex);

/** Convenience: `structuralAddress` at the walk's tail (= edgeIndex
    = walk.size()). Mirrors `contentIdAfter` but defined for all
    subject forms. */
Hash structuralAddressAfter(const Subject & subject, const Hash & scope, const std::vector<Edge> & walk);

/** Build the per-arg-encoded `QueryApply` payload for an apply-result
    subject at a given walk edge index. The returned query's JSON
    hash equals `contentIdAt(applyResult, scope, walk, edgeIndex)`,
    so callers can use the same value as both the Requests-pool key
    (= reqHash) and the apply-result's cdi (= what's recorded as
    `from` on downstream facts). Threads cb_arg root cdis at
    `edgeIndex` into `fromCIDs[]`, copies the Apply step's
    `fnPath`/`argPath`/root indices into the top-level query, and
    leaves `fn`/`arg` populated only if the caller passes them for
    the legacy direct payload's readability — the per-arg fields
    alone determine the hash. */
trace::QueryApply makeApplyResultQuery(
    const Subject & applyResultSubject,
    const Hash & scope,
    const std::vector<Edge> & walk,
    size_t edgeIndex);

/** Convenience: extract a query's `from` field as a Hash, if it has
    one. Apply queries don't have a `from`; throws. */
Hash extractFrom(const trace::QueryVariant & query);

/** Convenience wrapper around `pathAndRootsFromSubject`: returns just
    the path. Use the full helper when roots are also needed (= writer
    flush, walker probes). */
trace::PathExpr pathFromSubject(const Subject & subject);

/** Multi-root path expression for a Subject. The path navigates from
    the natural root (= `roots[0]`); Apply steps inside the path
    reference other roots by absolute index via `fnRootIndex` /
    `argRootIndex`. Roots are leaves of the subject tree: PositionalSeeds
    or OpaqueContentSubjects. Same-leaf occurrences (= e.g. fn and arg
    both deriving from the same cb_arg) collapse to one entry by Subject
    equality. Function characterization (= task #87) needs this so that
    observations on apply-result descendants reference both fn-root and
    arg-root in the wire payload. */
struct PathAndRoots
{
    trace::PathExpr path;
    std::vector<Subject> roots;
};

PathAndRoots pathAndRootsFromSubject(const Subject & subject);

/** Walk a Subject's parent chain through DerivedSubject nodes to
    the root form (PositionalSeed, OpaqueContentSubject, or
    ApplyResultSubject). Used by the per-arg flush path to compute
    the cb_arg root's CDI for `from` substitution while the access
    path is encoded separately as a PathExpr. */
const Subject & rootSubjectOf(const Subject & subject);

/** Bridge from an Object's identity surface to a cidasks Subject.
    Prefers `getSubject()` (= the proxy's static structural identifier
    when one is registered); falls back to wrapping `getCdiHex()` as
    an `OpaqueContentSubject` for non-proxy or pre-existing-cdi
    Objects. Returns nullopt if the Object exposes neither. Used at
    apply boundaries to compose `ApplyResultSubject` from the fn/arg
    constituents without needing to dynamic_cast each proxy type. */
struct ObjectIdentityLike
{
    const Subject * subject; ///< from `Object::getSubject()`, may be null
    std::optional<std::string> cdiHex; ///< from `Object::getCdiHex()`, fallback
};
std::optional<Subject> subjectFromObjectIdentity(const ObjectIdentityLike & id);

/** Short readable representation of a Subject — for tracing logs.
    Example: `seed(2)`, `getAttr(seed(2), "left")`,
    `applyResult(seed(0), seed(1))`, `opaque(ab12cd...)`. */
std::string describe(const Subject & subject);

/** Per-cb-apply observation context.

    A fresh instance is created at each cb-apply boundary and shared
    by every Object participating in that single invocation: the
    cb-arg side AmbientObject's queryFn pushes observations the inner
    makes on the outer arg; the apply-result side TracingObject /
    TracingReplayObject pushes observations made via the wrapper or
    its derived children. Both directions land in `observations` in
    chronological order, one Observation per call. Each Observation
    is conceptually its own one-fact Asks edge; `evolvedQueryFrom` on
    the wrapper wraps `observations` as a vector<Edge> with one fact
    per edge so the cidasks own-loop re-evaluates `myCidAtK` per
    observation.

    The context is **always read live**: no snapshot, no freeze. CIDs
    are retrieved by re-running `cidasks::contentIdAt` against the
    current state of `observations` on every `evolvedQueryFrom` call.
    Derived children of the wrapper share the same shared_ptr to the
    same ApplyContext so the chain `wrapper.getAttr("foo").getInt()`
    accumulates all three observations into one walk.

    `argSubject`/`scope` identify the cb arg's structural Subject and
    its inherited scope (= per the cidasks Inheritance section, the
    outer-scope CDIs that the per-invocation CIDs compose with). */
struct ApplyContext
{
    Subject argSubject;
    Hash scope;
    std::vector<Observation> observations;
};

} // namespace nix::cidasks
