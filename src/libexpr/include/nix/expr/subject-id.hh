#pragma once
/**
 * @file
 * Content-defined identity computed as a pure function of subject
 * and factset. See `doc/design/tracing-eval-cache-content-identity-via-asks.md`.
 *
 * state hashes are not stored anywhere; they're computed on demand
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

#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace nix {

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

/** A Subject postulating that its source can be re-read idempotently
    within an evaluator invocation; `hash` identifies the source. The
    link to actual content is established by Facts as evaluation
    proceeds, not by this hash.

    ## Valid cases

    - **Filesystem reads.** Nix treats the FS as a snapshot within
      one evaluator invocation, so a file read is identified by its
      path (= or a hash of the path, assuming a single root — WIP).
    - **Hash of an expression string to be parsed.** Assuming the
      parsing and loading parameters are identical, this uniquely
      identifies the value *by content* (and no Facts are necessary
      to pin down its behavior).
    - **Hash of a literal's construction parameters** (= `mkInt(42)`,
      `mkString("foo")`, `mkBool(true)`). The literal is fully
      determined by its parameters, so the hash identifies it; no
      Facts are necessary to pin down its behavior.

    ## Invalid cases

    - **Values that cannot be characterized completely ahead of
      time** — e.g. a lazy function argument given as a `Value`. Its
      behavior is whatever the caller eventually probes through it,
      which can't be summarized by a single hash up front.
    - **Taking an arbitrary subject id by value and using it as if
      it's an up-to-date id.** The id may have come from a lazy
      function argument; downstream behavior keyed on that id then
      conflates all possible future states of the argument and
      silently picks one of them, or produces other buggy behavior. */
struct PostulatedIdempotentRead
{
    Hash hash;
};

struct Subject
{
    std::variant<PositionalSeed, DerivedSubject, ApplyResultSubject, PostulatedIdempotentRead> data;
};

/** A single observation reduced to the two hashes stateHashAt needs.
    `fromHash` is the state hash the query was issued against;
    `elementHash` is SHA-256(reqHash || respHash) — H_element (SHA-256 of the request+response bytes).
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
    `from` fields all refer to subjects' state hashes at that precondition. */
struct Edge
{
    std::vector<Observation> observations;
};

/** Build an Observation from a QueryVariant/ResultVariant pair. Used by
    the writer at flush time where it already holds the variants. */
Observation observationFromQR(const trace::QueryVariant & query, const trace::ResultVariant & result);

/** Compute the state hash of `subject` after walking through all
    `edges`, inheriting `argAncestry` (the XOR of outer-argAncestry state hashes — e.g.
    state hash(Q) at the cb-apply boundary). Passing the zero hash for
    `argAncestry` gives the pure structural state hash, equivalent to
    no inheritance.

    Inheritance applies at the leaf: `PositionalSeed` and
    `PostulatedIdempotentRead` XOR `argAncestry` into their base hash.
    `DerivedSubject` and `ApplyResultSubject` propagate `argAncestry`
    via their constituents' (recursively state-hash-aware) state hashes,
    so the structural derivation incorporates inheritance naturally
    via the constituents' `from`-field values. */
Hash stateHashAfter(const Subject & subject, const Hash & argAncestry, const std::vector<Edge> & walk);

/** Compute the state hash of `subject` at the precondition of the
    edge at index `edgeIndex` in `walk`, inheriting `argAncestry`.
    `edgeIndex == 0` means the initial precondition (= empty
    factset); `edgeIndex == walk.size()` means the postcondition of
    the whole walk (equivalent to `stateHashAfter`).

    **Argument-level only.** Per the design (Principle 3, per-arg
    centralization), only argument-level subjects bear state hashes:
    `PositionalSeed` (cb_arg seed, evolves via own-loop),
    `ApplyResultSubject` (composes constituent argument state hashes), and
    `PostulatedIdempotentRead` (escape hatch). `DerivedSubject` does not
    have a state hash — observations on derived values fold into the cb_arg
    root's own-loop and the derived value is referenced via
    `(root_cdi, path)`. Passing a `DerivedSubject` traps; callers
    that want a content-addressed identifier for any Subject
    (including derived) should use `subjectHashAt` instead. */
Hash stateHashAt(const Subject & subject, const Hash & argAncestry, const std::vector<Edge> & walk, size_t edgeIndex);

/** Grouping-independent converged fold. Flattens `walk` into a
    deduplicated observation pool (by (fromHash, elementHash)) and
    repeatedly partitions it by state-match: at each round, all
    observations whose `fromHash` equals `subject`'s current state
    are pulled out as a synthetic edge and appended to a growing
    hypothetical walk; the round terminates when no observation
    matches. Returns `subject`'s state at the tail of that
    hypothetical walk — a fixed point of the greedy convergence.

    The result depends only on the SET of observations in `walk`,
    not on how they are grouped into edges. This is the alignment
    property the search→asks project needs: recorder and replayer
    reach the same value from any two walks carrying the same
    observations, regardless of edge boundaries. Semantically
    equivalent to iterating the observation-permutation loop in
    `TracingReplayEvaluator::resolveCdiId` to its fixed point. */
Hash stateHashConverged(
    const Subject & subject, const Hash & argAncestry, const std::vector<Edge> & walk);

/** Compute a content-addressed structural identifier for any
    `subject` — including `DerivedSubject`, where `stateHashAt`
    traps. For non-derived subjects this delegates to `stateHashAt`.
    For `DerivedSubject` it returns the producer query's hash:
    `qH(QueryGetAttr{name, from = root_cdi, fromCIDs, path})` for
    `GetAttr`, similarly for `GetListElem`. Used by `AmbientObject`,
    `TracingCallbackArg`, etc. to expose a single-`Hash` identity
    handle even though derived values don't have state hashes proper. */
Hash subjectHashAt(const Subject & subject, const Hash & argAncestry, const std::vector<Edge> & walk, size_t edgeIndex);

/** Convenience: `subjectHashAt` at the walk's tail (= edgeIndex
    = walk.size()). Mirrors `stateHashAfter` but defined for all
    subject forms. */
Hash subjectHashAfter(const Subject & subject, const Hash & argAncestry, const std::vector<Edge> & walk);

/** Per-subject observation trie fold step, as consumed by the subject-evolution fast-path
    stamping / navigation. Emitted by `stateHashAtStamping`
    whenever an observation matches the subject's running state
    and folds into it. The tuple `(curBefore, obsFromHash,
    obsElementHash) → curAfter` is uniquely identifying — cold
    stamps insert exactly the rows walker's navigation looks up. */
struct EvolutionStep {
    Hash curBefore;
    Hash obsFromHash;
    Hash obsElementHash;
    Hash curAfter;
};

/** Variant of `stateHashAt` that emits a callback per fold
    step. `stateHashAt` delegates to this with a no-op hook.
    Cold's writer passes a callback that inserts each step into
    `SubjectEvolutionEdges` (Subject-evolution stamping). Used only at cold
    record time — walker doesn't call this variant. */
Hash stateHashAtStamping(
    const Subject & subject,
    const Hash & argAncestry,
    const std::vector<Edge> & walk,
    size_t edgeIndex,
    const std::function<void(const EvolutionStep &)> & hook);

/** Build the per-arg-encoded `QueryApply` payload for an apply-result
    subject at a given walk edge index. The returned query's JSON
    hash equals `stateHashAt(applyResult, argAncestry, walk, edgeIndex)`,
    so callers can use the same value as both the Requests-pool key
    (= reqHash) and the apply-result's state hash (= what's recorded as
    `from` on downstream facts). Threads cb_arg root state hashes at
    `edgeIndex` into `fromCIDs[]`, copies the Apply step's
    `fnPath`/`argPath`/root indices into the top-level query, and
    leaves `fn`/`arg` populated only if the caller passes them for
    the legacy direct payload's readability — the per-arg fields
    alone determine the hash. */
trace::QueryApply makeApplyResultQuery(
    const Subject & applyResultSubject,
    const Hash & argAncestry,
    const std::vector<Edge> & walk,
    size_t edgeIndex);

/** Convenience: extract a query's `from` field as a Hash, if it has
    one. Apply queries don't have a `from`; throws. */
Hash fromStateHashOf(const trace::QueryVariant & query);

/** Convenience wrapper around `pathAndRootsFromSubject`: returns just
    the path. Use the full helper when roots are also needed (= writer
    flush, walker probes). */
trace::PathExpr pathFromSubject(const Subject & subject);

/** Multi-root path expression for a Subject. The path navigates from
    the natural root (= `roots[0]`); Apply steps inside the path
    reference other roots by absolute index via `fnRootIndex` /
    `argRootIndex`. Roots are leaves of the subject tree: PositionalSeeds
    or PostulatedIdempotentReads. Same-leaf occurrences (= e.g. fn and arg
    both deriving from the same cb_arg) collapse to one entry by Subject
    equality. Function characterization needs this so that observations
    on apply-result descendants reference both fn-root and arg-root in
    the wire payload. */
struct PathAndRoots
{
    trace::PathExpr path;
    std::vector<Subject> roots;
};

PathAndRoots pathAndRootsFromSubject(const Subject & subject);

/** Combine fn's and arg's inherited scopes into an apply boundary's
    argAncestry. Apply treats both sides equally (= unlike QueryAttr or
    curried-result subjects which have a neat single parent), but the
    combination must be non-commutative (= `f a` ≠ `a f`; cf.
    `flip apply`), so SHA-256 over a tagged concatenation rather
    than XOR. */
inline Hash combineArgAncestries(const Hash & fnArgAncestry, const Hash & argArgAncestry)
{
    std::string s = "apply-argAncestry:";
    s += fnArgAncestry.to_string(HashFormat::Base16, false);
    s += ":";
    s += argArgAncestry.to_string(HashFormat::Base16, false);
    return hashString(HashAlgorithm::SHA256, s);
}

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
    per edge so the subject-id own-loop re-evaluates `myCidAtK` per
    observation.

    The context is **always read live**: no snapshot, no freeze. state hashes
    are retrieved by re-running `stateHashAt` against the
    current state of `observations` on every `evolvedQueryFrom` call.
    Derived children of the wrapper share the same shared_ptr to the
    same ApplyContext so the chain `wrapper.getAttr("foo").getInt()`
    accumulates all three observations into one walk.

    `argId`/`argAncestry` identify the cb arg's structural Subject and
    its inherited argAncestry (= per the subject-id Inheritance section, the
    outer-argAncestry state hashes that the per-invocation state hashes compose with). */
struct ApplyContext
{
    Subject argId;
    Hash argAncestry;
    std::vector<Observation> observations;
};

} // namespace nix
