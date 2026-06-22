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
    `elementHash` is SHA-256(reqHash || respHash) — the v13 H_element. */
struct Fact
{
    Hash fromHash;
    Hash elementHash;
};

/** An Asks edge's worth of facts. Facts in one edge are dispatched
    against a single shared precondition factset; their `from`
    fields all refer to subjects' content ids at that precondition. */
struct Edge
{
    std::vector<Fact> facts;
};

/** Build a Fact from a QueryVariant/ResultVariant pair. Used by the
    writer at flush time where it already holds the variants. */
Fact factFromQR(const trace::QueryVariant & query, const trace::ResultVariant & result);

/** Compute the content id of `subject` after walking through all
    `edges`. With an empty walk, returns the subject's structural
    initial id. */
Hash contentIdAfter(const Subject & subject, const std::vector<Edge> & walk);

/** Compute the content id of `subject` at the precondition of the
    edge at index `edgeIndex` in `walk`. `edgeIndex == 0` means the
    initial precondition (= empty factset); `edgeIndex == walk.size()`
    means the postcondition of the whole walk (equivalent to
    `contentIdAfter`). */
Hash contentIdAt(const Subject & subject, const std::vector<Edge> & walk, size_t edgeIndex);

/** Convenience: extract a query's `from` field as a Hash, if it has
    one. Apply queries don't have a `from`; throws. */
Hash extractFrom(const trace::QueryVariant & query);

/** Short readable representation of a Subject — for tracing logs.
    Example: `seed(2)`, `getAttr(seed(2), "left")`,
    `applyResult(seed(0), seed(1))`, `opaque(ab12cd...)`. */
std::string describe(const Subject & subject);

} // namespace nix::cidasks
