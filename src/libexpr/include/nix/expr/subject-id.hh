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
    /* #183 attribution: which cell this fact belongs to. null →
       sessionRootCell (env-fact default). */
    std::weak_ptr<const struct ArgCell> attributionCell;
};

/** An Asks edge's worth of observations. Observations in one edge are
    dispatched against a single shared precondition factset; their
    `from` fields all refer to subjects' state hashes at that precondition. */
struct ObservationSet
{
    std::vector<Observation> observations;
};

/** #178: the structural id of `subject`. Under stable Q hashes there
    is no evolution to fold; every Subject variant reduces to a
    positional / structural computation aligned with the equivalent
    Selector's content hash (#186):

    - `Arg{depth}`: `computeSelectorHash(SelectorArg{depth})`.
    - `PostulatedIdempotentRead{hash}`: `hash` (already an id).
    - `ApplyResultSubject{fn, ...}`: `computeSelectorHash(SelectorApply{fn=hex(subjectId(fn))})`.
    - `DerivedSubject{parent, kind, name/index}`:
      `computeSelectorHash(SelectorGetAttr|GetListElem{..., from=hex(subjectId(parent))})`. */
Hash subjectId(const Subject & subject);

/** Build the Selector variant whose content hash IS this Subject's
    identity. Under #186 every Subject variant maps to a Selector
    alternative (Arg → SelectorArg, DerivedSubject → SelectorGetAttr /
    SelectorGetListElem, ApplyResultSubject → SelectorApply). This
    helper returns that Selector so callers can use it directly as
    payload (e.g. obsSet entries) rather than as an opaque hash. */
trace::SelectorVariant subjectAsSelector(const Subject & subject);

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

    Field usage today: only `observations` is read (by TracingObject
    and TracingReplayObject's pushObservation). `argSubject` /
    `argAncestry` are stored at construction but never consumed under
    the per-cell factset model. */
struct ApplyContext
{
    std::vector<Observation> observations;
};

} // namespace nix
