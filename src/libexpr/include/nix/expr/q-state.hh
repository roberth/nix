#pragma once
/**
 * @file
 * QState — per-Selector-invocation state owned by an ArgCell.
 *
 * Every function application creates an ArgCell whose QState holds the
 * invocation's stable Selector hash plus walker-local buffers
 * (pending/committed edge observations and per-walk request memoization).
 *
 * Held via `mutable std::shared_ptr<QState>` on ArgCell so that:
 * - Topology-only cells (localCell / seedCell in expr-from-object.cc)
 *   don't pay the field's size — they never allocate a QState.
 * - Cells held via `std::shared_ptr<const ArgCell>` (the codebase's
 *   convention) can still mutate the pointer to install / reset the
 *   Q state; the pointee's fields mutate freely because the QState
 *   itself is not const-qualified.
 *
 * Concurrency invariant (per user 2026-07-24): the eval cache runs with
 * one evaluator active at a time even under I/O-driven multi-evaluator
 * concurrency. Each evaluator owns its cell trees; switching evaluators
 * means switching which tree is active. Cell-owned QState is what makes
 * that hold structurally — no shared global state across trees.
 */

#include "nix/expr/observation-set.hh"
#include "nix/expr/trace-types.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/util/hash.hh"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace nix {

struct ArgCell;

struct QState
{
    /** Weak back-pointer to the cell that owns this qState. Populated
        at logSelectorOnCell / logRootSelectorOnCell time. Used to look
        up `cell->factSetHash()` for Ask/Terminal keying under the
        per-cell factset direction (task #177). weak_ptr avoids the
        obvious cycle (cell holds shared_ptr<QState>). */
    std::weak_ptr<const ArgCell> cell;

    /** Selector hash for this Q. Stable under #178 — no evolution;
        the same Q hash for the whole invocation. */
    Hash currentQ{HashAlgorithm::SHA256};

/* -------------------- Walker walk-local -------------------------
       Walker per-walk buffers. Cell.facts is the source of truth for
       observed state (populated by dispatch via commitEdge). These
       fields are transient buffers scoped to one walk. */

    /** Per-edge buffer: dispatch() appends facts here; the walker's
        edge-commit callback drains them into cell.facts on commit,
        discards on reject. */
    std::vector<Observation> pendingEdgeObservations;

    /** Dedup committed edges within this walk (XOR-fold of
        element hashes within the edge). addFact is idempotent, so
        this is a perf shortcut to skip the drain loop on repeats. */
    std::unordered_set<TracingHash> committedEdgeFingerprints;

    /** Memoize requestHash -> responseHash for stable requests within
        this walk (file reads, env vars — no `from` state, response is
        a pure function of request). Skipped for outer-value requests
        whose `from` is pre-response and can produce different
        responses under matching-until-divergence divergence. */
    std::unordered_map<TracingHash, TracingHash> responseFor;
};

} // namespace nix
