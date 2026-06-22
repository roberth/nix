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

/** A single observation: (query, response) pair. */
struct Fact
{
    trace::QueryVariant query;
    trace::ResultVariant result;
};

/** An Asks edge's worth of facts. Facts in one edge are dispatched
    against a single shared precondition factset; their `from`
    fields all refer to subjects' content ids at that precondition. */
struct Edge
{
    std::vector<Fact> facts;
};

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

/** SHA-256(reqHash || respHash). Same as the XOR-fold input used by
    the v13 trie's factSetHash. */
Hash hElement(const Fact & fact);

} // namespace nix::cidasks
