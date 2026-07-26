#pragma once
/**
 * @file
 * Content-defined identity computed as a pure function of subject
 * and factset. See `doc/design/tracing-eval-cache-vocabulary.md`
 * §Subject-identity mechanism (transitional).
 *
 * state hashes are not stored anywhere; they're computed on demand
 * from a value's static structural identifier (the "subject") and
 * the current factset. The recorder and the walker call the same
 * function with the same arguments and obtain identical hashes.
 * Retires under task #178 in favour of per-cell factset curs.
 *
 * This module is currently standalone — not yet wired into the
 * recorder/walker. It exists to validate the math in isolation
 * before migration.
 */

#include "nix/expr/trace-types.hh"
#include "nix/util/hash.hh"

#include <functional>
#include <memory>
#include <source_location>
#include <string>
#include <variant>
#include <vector>

namespace nix {

struct Subject;

/** A cb arg at a static apply-stack depth N (reverse De Bruijn).
    The leaf of every Subject tree. */
struct Arg
{
    int depth;
    auto operator<=>(const Arg &) const = default;
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
    std::variant<Arg, DerivedSubject, ApplyResultSubject, PostulatedIdempotentRead> data;
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
    /* #183: separately-carried (reqHash, respHash) so walker-side
       commits can append to cell.facts as (req, resp) pairs. */
    Hash reqHash{HashAlgorithm::SHA256};
    Hash respHash{HashAlgorithm::SHA256};
};

/** An Asks edge's worth of observations. Observations in one edge are
    dispatched against a single shared precondition factset; their
    `from` fields all refer to subjects' state hashes at that precondition. */
struct ObservationSet
{
    std::vector<Observation> observations;
};

/** Build an Observation from a SelectorVariant/ResultVariant pair. Used by
    the writer at flush time where it already holds the variants. */
Observation observationFromQR(const trace::SelectorVariant & query, const trace::ResultVariant & result);

/** #178: the initial structural id of `subject`, scoped by
    `argAncestry`. Under stable Q hashes there is no evolution to
    fold; every Subject variant reduces to a positional / structural
    computation:

    - `Arg{depth}`: `SHA256("positional-D") XOR argAncestry`.
    - `PostulatedIdempotentRead{hash}`: `hash` (already scoped).
    - `ApplyResultSubject{fn, arg}`: SHA256 of a canonical
      Apply payload composed of `subjectId(fn, argAncestry)` and
      `subjectId(arg, argAncestry)`.
    - `DerivedSubject{parent, kind, name/index}`: SHA256 of a
      canonical getter payload composed of the kind, name/index,
      and `subjectId(parent, argAncestry)`. */
Hash subjectId(const Subject & subject, const Hash & argAncestry);

/** Build the per-arg-encoded `SelectorApply` payload for an apply-result
    subject at a given history edge index. The returned query's JSON
    hash equals `stateHashAt(applyResult, argAncestry, history, step)`,
    so callers can use the same value as both the Requests-pool key
    (= reqHash) and the apply-result's state hash (= what's recorded as
    `from` on downstream facts). Threads cb_arg root state hashes at
    `step` into `fromStateHashes[]`, copies the Apply step's
    `fnPath`/`argPath`/root indices into the top-level query, and
    leaves `fn`/`arg` populated only if the caller passes them for
    the legacy direct payload's readability — the per-arg fields
    alone determine the hash. */
trace::SelectorApply makeApplyResultQuery(
    const Subject & applyResultSubject,
    const Hash & argAncestry,
    const std::vector<ObservationSet> & history,
    size_t step);

/** Extract a query's `from` field as a Hash, if it carries one.
    Under #178 the `from` field is retired; this helper is a
    transitional accessor kept until the Selector types drop the
    field. Returns zero on empty. */
Hash fromStateHashOf(const trace::SelectorVariant & query);

/** Convenience wrapper around `pathAndRootsFromSubject`: returns just
    the path. Use the full helper when roots are also needed (= writer
    flush, walker probes). */
trace::PathExpr pathFromSubject(const Subject & subject);

/** Multi-root path expression for a Subject. The path navigates from
    the natural root (= `roots[0]`); Apply steps inside the path
    reference other roots by absolute index via `fnRootIndex` /
    `argRootIndex`. Roots are leaves of the subject tree: Args
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

/** Decompose a Subject into (path from roots to subject, list of
    roots). **Roots are guaranteed to be leaf variants** — `Arg` or
    `PostulatedIdempotentRead` — by construction: the builder recurses
    through `DerivedSubject` and `ApplyResultSubject` without adding
    them to `roots`, so callers can safely pass each root to the strict
    `stateHashAt` without the DerivedSubject trap. This is a
    load-bearing invariant for five call sites (outer-object.cc,
    tracing-writer.cc x2, replay-callback-arg.cc, tracing-writer.cc's
    apply-flush) — do not change the builder to emit non-leaves as
    roots without adjusting them. */
PathAndRoots pathAndRootsFromSubject(const Subject & subject);

/** Combine fn's and arg's inherited scopes into an cb-apply's
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
    Example: `arg(2)`, `getAttr(arg(2), "left")`,
    `applyResult(arg(0), arg(1))`, `opaque(ab12cd...)`. */
std::string describe(const Subject & subject);

/** Per-cb-apply observation context.

    A fresh instance is created at each cb-apply and shared
    by every Object participating in that single invocation: the
    cb-arg side OuterObject's queryFn pushes observations the inner
    makes on the outer arg; the apply-result side TracingObject /
    TracingReplayObject pushes observations made via the wrapper or
    its derived children. Both directions land in `observations` in
    chronological order, one Observation per call. Each Observation
    is conceptually its own one-fact Asks edge; `evolvedQueryFrom` on
    the wrapper wraps `observations` as a vector<ObservationSet> with one fact
    per edge so the subject-id own-loop re-evaluates `myCidAtK` per
    observation.

    The context is **always read live**: no snapshot, no freeze. state hashes
    are retrieved by re-running `stateHashAt` against the
    current state of `observations` on every `evolvedQueryFrom` call.
    Derived children of the wrapper share the same shared_ptr to the
    same ApplyContext so the chain `wrapper.getAttr("foo").getInt()`
    accumulates all three observations into one history.

    `argSubject`/`argAncestry` identify the cb arg's structural Subject and
    its inherited argAncestry (= per the subject-id Inheritance section, the
    outer-argAncestry state hashes that the per-invocation state hashes compose with). */
struct ApplyContext
{
    Subject argSubject;
    Hash argAncestry;
    std::vector<Observation> observations;
};

} // namespace nix
