#pragma once
/**
 * @file
 * ArgCell — scope-graph node for cache-boundary proxies.
 * Carries structural topology (depth, parent, liveObject), the cell's
 * own facts (req/resp pairs, XOR-composed with parent's for
 * `factSetHash()`), plus per-Selector-invocation `qState` and
 * per-callback-firing `callbackState` when applicable.
 *
 * `depth` provides the static positional handle used by
 * `SelectorArg{depth}` producers. Object identity is the content
 * hash of the producer Selector; per-cell factset composition is
 * what discriminates apply-result Terminals across sibling
 * callback firings.
 */

#include "nix/expr/evaluator.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/util/hash.hh"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nix {

struct QState; // defined in q-state.hh; forward-declared here so
               // topology-only cells don't pull the heavy dependencies.

/** Per-callback-firing accumulator, living on the ArgCell created for
    the callback arg. Contra-arg observations append directly via the
    callback-arg proxy's own cell chain; QCA emission at applyResult
    WHNF force finds the cell via the applyResult's argCell chain.
    Concurrency invariant: cell trees are per-active-evaluator, so
    callback state doesn't leak across trees. */
struct CallbackState
{
    /** Fn's Q hash hex, captured at cell allocation from the
        applyQuery's `fn` field. Emitted as the SelectorCallbackApply
        payload's `fn` field at QCA time. */
    std::string fnStateHashHex;

    /** Observations the outer made on this cell's contra-arg during
        the callback body's evaluation. Snapshotted into the
        ObservationSet CAS at QCA emission. */
    std::vector<TracingDecisionGraph::Observation> runningObsSet;
};

struct ArgCell : std::enable_shared_from_this<ArgCell>
{
    /** Reverse-De-Bruijn depth: 0 at the cache call's argument,
        N+1 in a cell whose parent is at depth N. Set at
        construction, immutable. Used as the positional handle
        for `SelectorArg{depth}` producers. */
    int depth = 0;

    /** Next-outer cell. Null at the root (the cache call's
        argument). */
    std::shared_ptr<const ArgCell> parent;

    /** The live Object the cell represents. The walker's
        cell-chain resolution returns this to identify the live
        proxy for a recorded positional handle. */
    std::shared_ptr<Object> liveObject;

    /** Per-Selector-invocation Q-evolution state. Non-null on cells
        that represent an application (SelectorApply / SelectorCallbackApply /
        root SelectorImport / SelectorExpr) and thus own a Selector
        chain. Null on topology-only cells (localCell / seedCell in
        expr-from-object.cc) that carry proxy identity through a
        boundary without owning a Selector chain.

        `mutable` because ArgCells are held throughout via
        `shared_ptr<const ArgCell>`; the pointer needs to be
        assignable through a const cell. The QState pointee itself is
        not const — its fields evolve as observations attribute to
        this cell.

        See q-state.hh for the field breakdown and the concurrency
        rationale. */
    mutable std::shared_ptr<QState> qState;

    /** Per-callback-firing accumulator. Non-null on cells created
        for a callback arg (OuterApply::run's localCell). Null on all
        other cells. See CallbackState above for field semantics. */
    mutable std::shared_ptr<CallbackState> callbackState;

    /** Per-fact entry. `response` is the fact's response hash (identity
        contribution to factSetHash). `barrier` is a writer-side monotonic
        sequence number used at logResult time to group facts into
        causally-ordered Ask edges (Foundational principle 9). Barriers
        are NOT persisted — only the writer's Ask-insertion logic reads
        them; the walker's fact additions use the writer's current
        barrier value at peek time. */
    struct FactEntry
    {
        Hash response;
        uint64_t barrier = 0;
    };

    /** This cell's own facts — a set of (requestHash → FactEntry)
        pairs. Env facts append to session-root cell's facts; arg
        observations append to the arg's own cell. XOR-fold over
        response hashes is order-independent (identity is a set, not
        a sequence). Cumulative — never cleared (state creep,
        dedup at CAS).

        `mutable` because ArgCells are held via
        `shared_ptr<const ArgCell>`; appends happen through the
        const pointer. */
    mutable std::map<Hash, FactEntry> facts;

    /** Insert a (request, response) fact with an optional barrier
        stamp. Idempotent per request key (first stamp wins). */
    void addFact(const Hash & reqHash, const Hash & respHash, uint64_t barrier = 0) const
    {
        facts.try_emplace(reqHash, FactEntry{respHash, barrier});
    }

    /** Cumulative factset visible from this cell: own facts XOR-folded
        with parent's factSetHash. Pull-based (recursive) — computed
        on demand from `facts`, not stored. */
    TracingDecisionGraph::SetHash factSetHash() const
    {
        auto acc = TracingDecisionGraph::emptySetHash();
        for (auto & [req, entry] : facts)
            acc = TracingDecisionGraph::xorFactIntoHash(acc, req, entry.response);
        if (parent)
            acc = TracingDecisionGraph::xorHashes(acc, parent->factSetHash());
        return acc;
    }

    /** Construct a cell whose parent is `parent_`. depth is one
        deeper than parent (or 0 if parent is null). `liveObject_`
        may be null at construction if the live proxy isn't yet
        constructed; assign to the cell's `liveObject` field
        afterwards. */
    static std::shared_ptr<ArgCell> make(
        std::shared_ptr<const ArgCell> parent_,
        std::shared_ptr<Object> liveObject_)
    {
        auto cell = std::make_shared<ArgCell>();
        cell->parent = parent_;
        cell->depth = parent_ ? parent_->depth + 1 : 0;
        if (liveObject_)
            cell->liveObject = std::move(liveObject_);
        return cell;
    }
};

/** Return the proxy's argCell cell — the nearest enclosing apply's
    cell. Navigation children carry the parent's cell directly; apply
    results carry their own fresh cell. Returns null for non-proxy
    Objects or for proxies that haven't been scoped. */
inline std::shared_ptr<const ArgCell> effectiveArgCell(const Object & obj)
{
    return obj.getProxyArgCell();
}

} // namespace nix
