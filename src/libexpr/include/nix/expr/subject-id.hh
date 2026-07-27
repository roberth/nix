#pragma once
/**
 * @file
 * Post-#183 minimal companion header: hosts the Observation /
 * ObservationSet / ApplyContext value types that survived the Subject
 * retirement. Identity itself is now expressed as a hex string via
 * `Object::getSelectorHashHex()` — content hash of the Selector chain
 * that produced the Object.
 *
 * TODO: rename this file to something like observation-set.hh — the
 * "subject" concept is gone.
 */

#include "nix/expr/trace-types.hh"
#include "nix/util/hash.hh"

#include <memory>
#include <vector>

namespace nix {

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

} // namespace nix
