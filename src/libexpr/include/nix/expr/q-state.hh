#pragma once
/**
 * @file
 * QState — per-Selector-invocation state that used to live on
 * `TracingWriter::activeQueryStack`'s `ActiveSelector` frames.
 *
 * Under the cell-migration (see task list #158–#171), every function
 * application creates a new `ArgCell` (already true structurally),
 * and that cell owns a QState holding the invocation's Q-evolution
 * bookkeeping: current Selector hash, payload template, from-subject,
 * per-Q chain observation history, landing-chain anchors.
 *
 * Held via `mutable std::shared_ptr<QState>` on ArgCell so that:
 * - Topology-only cells (localCell / seedCell in expr-from-object.cc)
 *   don't pay the field's size — they never allocate a QState.
 * - Cells held via `std::shared_ptr<const ArgCell>` (the codebase's
 *   convention) can still mutate the pointer to install / reset the
 *   Q state; the pointee's fields mutate freely because the QState
 *   itself is not const-qualified.
 *
 * Concurrency invariant to preserve (per user 2026-07-24): the eval
 * cache runs with one evaluator active at a time even under I/O-driven
 * multi-evaluator concurrency. Each evaluator owns its cell trees;
 * switching evaluators means switching which tree is active; no shared
 * writer-side stack to trample. QState living on cells (not on a
 * TracingWriter field) is what makes that invariant hold structurally.
 */

#include "nix/expr/subject-id.hh"
#include "nix/expr/trace-types.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/util/hash.hh"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace nix {

struct QState
{
    /** Selector hash at the current position of Q's own chain.
        Evolves as each observation attributed to this cell folds
        into `perQEnvWalk`, driving `fromSubject`'s state hash and
        thus the `from` field of the payload. */
    Hash currentQ{HashAlgorithm::SHA256};

    /** Selector payload template. `from` gets rewritten as
        `fromSubject`'s state advances; re-hashing gives `currentQ`. */
    trace::SelectorVariant payloadTemplate;

    /** Subject whose state hash drives Q evolution. Not set for
        root selectors or selectors whose `from` is a fixed hash
        (state does not evolve). */
    std::optional<Subject> fromSubject;

    /** `argAncestry` argument to `stateHashAt(fromSubject, ...,
        perQEnvWalk, ...)`. */
    Hash fromSubjectArgAncestry{HashAlgorithm::SHA256};

    /** Cached fromSubject state hash at last recomputation. Compared
        to a fresh compute after each observation to detect Q
        evolution without re-hashing every time. */
    Hash fromSubjectLastState{HashAlgorithm::SHA256};

    /** This Selector's own observation chain. Every observation
        attributed to this cell appends here.

        Q's `from` is derived from
        `stateHashAt(fromSubject, argAncestry, perQEnvWalk,
        perQEnvWalk.size())` — using THIS cell's chain, not
        session-wide observations from other cells. Preserves the
        same-shape-collapse property: two invocations of the same
        Selector against the same fromSubject-initial-state evolve
        to the same finalQ because they see the same fold from their
        own chains. */
    std::vector<ObservationSet> perQEnvWalk;

    /** Parent Selector's terminalCur — the walker's structural
        anchor for landing chains that hop this cell's entry cur. */
    std::optional<TracingDecisionGraph::SetHash> structuralParentFactSetHash;

    /** B10 landing-chain simulation: payload as it was at push time,
        immutable through Q's evolution. Used at completion to
        simulate walker Q evolution through pre-push session Ask
        trail and insert Ask rows under each simulated (Q, cur) so a
        walker from ∅ can fold its way to this cell's session-
        cumulative entry cur. */
    nlohmann::json initialPayloadTemplate;

    /** From-subject state hash at push time — pairs with
        `initialPayloadTemplate` for the landing-chain simulation. */
    Hash initialFromSubjectState{HashAlgorithm::SHA256};

    /** Selector variant's static tag string. Captured at push for
        use at completion when synthesising composite request
        payloads for the parent (B2). */
    std::string queryTag;

    /** Size of the writer's `envAsksEdges` at push time. Used at
        completion to slice the trailing edges that belong to this
        Selector's chain vs. to earlier cells. */
    std::size_t envAsksEdgesSizeAtPush{0};
};

} // namespace nix
