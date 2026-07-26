#pragma once
/**
 * @file
 * ArgCell — scope-graph node for cache-boundary proxies.
 * Carries only structural topology (depth, parent, liveObject).
 *
 * The cell exists for navigation through the proxy graph; the
 * `depth` field provides the static positional handle used by
 * `Arg{depth}` subjects. State hashes are pure functions of
 * (subject, argAncestry, history, step) computed on demand — never
 * stored on the cell. Under the multiplexer + per-cell factset
 * direction (task #176), cells also become factset carriers.
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
    the callback arg. Under Phase D2's cell-migration, this state used
    to live in a writer-side `std::vector<CallbackCell>` (indexed by
    `applyId`); moved onto the cell so:

    - Contra-arg observations append directly via the callback-arg
      proxy's own cell chain — no writer-global lookup.
    - QCA emission at applyResult WHNF force finds the cell via the
      applyResult's argCell chain — no `applyId` iteration on a
      writer-owned vector.
    - Concurrency invariant continues to hold: cell trees are
      per-active-evaluator; callback state doesn't leak across trees. */
struct CallbackState
{
    /** Identity of this callback firing; equals the natural hash of
        the apply query payload. Historically used as an index key on
        the writer-side callbackCells vector; retained for QCA
        payload identity + trace-log correlation. */
    Hash applyId{HashAlgorithm::SHA256};

    /** Fn's initial state hash (empty history). Cell lookup key at
        QCA emission (matches ApplyResultSubject's fn state hash
        under matching-until-divergence). Captured at cell allocation
        from the applyQuery's `fn` field. */
    std::string fnStateHashHex;

    /** Cached call's callArgAncestry, encoded into the QCA payload so
        the walker's ReplayCallbackArg reconstructs the arg's Subject
        at the same argAncestry. Set on first contra-arg observation. */
    std::string argAncestryHex;

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
        when computing state hashes via stateHashAfter. */
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

    /** This cell's own facts — a set of (requestHash → responseHash)
        pairs. Env facts append to session-root cell's facts; arg
        observations append to the arg's own cell. Order-independent
        (XOR-commutative fold). Cumulative — never cleared (state
        creep, dedup at CAS).

        `mutable` because ArgCells are held via
        `shared_ptr<const ArgCell>`; appends happen through the
        const pointer. */
    mutable std::map<Hash, Hash> facts;

    /** Insert a (request, response) fact into this cell's set.
        Idempotent for duplicate (request, response) pairs. */
    void addFact(const Hash & reqHash, const Hash & respHash) const
    {
        facts.emplace(reqHash, respHash);
    }

    /** Cumulative factset visible from this cell: own facts XOR-folded
        with parent's factSetHash. Pull-based (recursive) — computed
        on demand from `facts`, not stored. */
    TracingDecisionGraph::SetHash factSetHash() const
    {
        auto acc = TracingDecisionGraph::emptySetHash();
        for (auto & [req, resp] : facts)
            acc = TracingDecisionGraph::xorFactIntoHash(acc, req, resp);
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
