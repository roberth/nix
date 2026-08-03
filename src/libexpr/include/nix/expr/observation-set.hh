#pragma once
/**
 * @file
 * Observation + ObservationSet value types — per-Ask-edge fact records
 * used by the writer's obsSet CAS and consumed by the walker for cell
 * factset composition and SelectorCallbackApply payload assembly.
 *
 * Object identity is expressed via `Object::getSelectorHashHex()` —
 * content hash of the Selector chain that produced the Object.
 */

#include "nix/expr/trace-types.hh"
#include "nix/util/hash.hh"

#include <memory>
#include <vector>

namespace nix {

/** Per-Ask-edge fact record. `elementHash` = SHA-256(reqHash ++ respHash),
    the currency of the Asks-level XOR-fold into `factSetHash`. Cell-scoped
    attribution rides on `attributionCell`. */
struct Observation
{
    TracingHash elementHash;
    /* #183: separately-carried (reqHash, respHash) so walker-side
       commits can append to cell.facts as (req, resp) pairs. */
    TracingHash reqHash = trace::tracingZeroHash();
    TracingHash respHash = trace::tracingZeroHash();
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

} // namespace nix
