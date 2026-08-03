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
    std::vector<TracingDecisionGraph::InlineFact> runningObsSet;
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

    /** Same members as `facts`, kept in insertion order. Callers
        that emit Ask chains iterate this to preserve barrier
        ordering (map iteration is by reqHash, meaningless here). */
    mutable std::vector<std::pair<Hash, FactEntry>> factsInOrder;

    /** XOR-fold of just this cell's own facts, maintained
        incrementally by addFact/removeFact (XOR is self-inverse, so
        both operations are O(1)). `factSetHash()` composes this with
        the parent chain via O(depth) hash XORs — no SHA-256 recompute
        per call. */
    mutable TracingDecisionGraph::SetHash cachedOwnFactSetHash =
        TracingDecisionGraph::emptySetHash();

    /** Per-Selector oldest recorded terminalCur on this cell — used
        as an anchor for structural-parent-based landing chains.
        Populated by logResult/logQueryResult on the cell the Q's
        Terminal is written to; each Selector stores its FIRST
        (chronologically oldest) terminalCur so structural chains
        for later child Qs on the same cell anchor at a state the
        walker can reach cheaply.

        `barrierAtRecord` snapshots writer.peekBarrier() at Q's
        record moment; `epochAtRecord` snapshots the cell's
        canonicalisationEpoch. A child Q's structural delta chain
        includes only facts with barrier >= barrierAtRecord — facts
        folded into parent's terminalCur are excluded, avoiding
        XOR-cancel.

        Lives on the cell (not global) because the same Selector on
        different argument cells resolves to different terminalCurs.

        Mutable for the same reason as `facts`. */
    struct FirstTerminalRecord
    {
        TracingDecisionGraph::SetHash terminalCur;
        uint64_t barrierAtRecord;
        uint64_t epochAtRecord;
    };
    mutable std::map<Hash, FirstTerminalRecord> firstTerminalCurs;

    /** Bumped on every removeFact call. FirstTerminalRecord captures
        the aggregate ancestry epoch (see canonicalisationEpochChain)
        at record time; a mismatch on lookup signals a canonicalisation
        happened between record and now on this cell OR any ancestor,
        so the recorded terminalCur no longer aligns with the current
        cell factset and a barrier-based delta can't reconstruct the
        removed fact's XOR contribution. Structural chain skips in
        that case; the ∅-chain fallback keeps correctness. */
    mutable uint64_t canonicalisationEpoch = 0;

    /** Sum of canonicalisationEpoch over this cell + all ancestors.
        Cheap: O(depth) walk. Since delta chains fold facts from cell
        and ancestors, ANY ancestor removeFact invalidates the delta —
        checking only the child cell's epoch would miss ancestor
        canonicalisation events. */
    uint64_t canonicalisationEpochChain() const
    {
        uint64_t sum = canonicalisationEpoch;
        for (auto c = parent.get(); c; c = c->parent.get())
            sum += c->canonicalisationEpoch;
        return sum;
    }

    /** Insert a (request, response) fact with an optional barrier
        stamp. Idempotent per request key (first stamp wins).

        Returns true when a new entry was inserted, false when this
        cell already had an entry for `reqHash`. Callers use the
        return value to gate barrier bumping and any other side
        effects that should only fire per cell-new fact (the writer's
        logOuterObservation is the canonical caller). */
    bool addFact(const Hash & reqHash, const Hash & respHash, uint64_t barrier = 0) const
    {
        auto [it, inserted] = facts.try_emplace(reqHash, FactEntry{respHash, barrier});
        if (inserted) {
            factsInOrder.emplace_back(reqHash, it->second);
            cachedOwnFactSetHash = TracingDecisionGraph::xorFactIntoHash(
                cachedOwnFactSetHash, TracingHash::of(reqHash), TracingHash::of(respHash));
        }
        return inserted;
    }

    /** Remove a fact by request hash. Used by write-time
        canonicalisation (state-creep meet lattice, see main doc's
        "state/observation-creep canonicalisation" note) — a fact
        with a redundant obsSet gets replaced by its canonical form
        (with the intersected obsSet), and the old one is removed.

        XOR is self-inverse, so removal is O(1) for the cached hash.
        The insertion-order vector needs a linear scan; canonicalisation
        is rare enough that the O(n) removal cost doesn't dominate. */
    void removeFact(const Hash & reqHash) const
    {
        auto it = facts.find(reqHash);
        if (it == facts.end())
            return;
        cachedOwnFactSetHash = TracingDecisionGraph::xorFactIntoHash(
            cachedOwnFactSetHash, TracingHash::of(reqHash), TracingHash::of(it->second.response));
        factsInOrder.erase(
            std::remove_if(factsInOrder.begin(), factsInOrder.end(),
                [&](const auto & p) { return p.first == reqHash; }),
            factsInOrder.end());
        facts.erase(it);
        ++canonicalisationEpoch;
    }

    /** Cumulative factset visible from this cell: own facts XOR-folded
        with parent's factSetHash. O(depth) XORs of incrementally-cached
        per-cell hashes — no SHA-256 recomputation per call. */
    TracingDecisionGraph::SetHash factSetHash() const
    {
        auto acc = cachedOwnFactSetHash;
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
