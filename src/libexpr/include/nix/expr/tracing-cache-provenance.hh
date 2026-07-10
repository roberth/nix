#pragma once
/**
 * @file
 * Provenance registry for the tracing eval cache.
 *
 * Every hash the cache manipulates — requestHash, resultHash,
 * stateHash, factSetHash, contextHash, etc. — has a construction
 * story (what it was hashed from, at which cur, for which subject).
 * When something goes wrong, we routinely see a prefix like
 * `f77aae8f5091` in logs and want to know what value that hash is
 * a fingerprint of. The registry keeps a persistent record so the
 * question is always answerable.
 *
 * Enable by setting `NIX_CACHE_PROVENANCE_FILE=/path/to/prov.jsonl`.
 * On process exit the file receives one JSON line per recorded
 * entry — hash + kind + structured details. Off by default; when
 * off, calls are no-ops (registration is skipped, memory footprint
 * is a single boolean check).
 *
 * The registry is thread-safe (mutex around a hash-keyed map) and
 * write-once (first registration for a given hash wins; later
 * calls for the same hash are ignored). Hashes are content-derived,
 * so re-registration for the same hash with the same details is a
 * no-op semantically, and re-registration with different details
 * would indicate a bug worth investigating without silently
 * clobbering the original record.
 */

#include "nix/util/hash.hh"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace nix {

/**
 * Register what a hash represents. `kind` is a short tag
 * (`"requestHash"`, `"stateHash"`, `"contextHash"`, …). `details`
 * carries the structured description — for a requestHash this is
 * the query payload; for a stateHash the subject/argAncestry/step,
 * and so on.
 *
 * Cheap and safe to call unconditionally: if provenance capture is
 * disabled the call returns immediately.
 */
void recordProvenance(const Hash & h, std::string_view kind, nlohmann::json details);

/**
 * Look up the provenance entry for a hash. Returns nullopt if no
 * entry has been registered (either because provenance capture is
 * disabled, or because the hash was constructed at a site that
 * doesn't yet register itself).
 */
std::optional<nlohmann::json> lookupProvenance(const Hash & h);

/**
 * Render a hash for logging as its 12-char hex prefix, followed
 * by its provenance kind if known. Cheap wrapper on lookupProvenance.
 */
std::string describeHash(const Hash & h);

/**
 * True if provenance capture is enabled (i.e. NIX_CACHE_PROVENANCE_FILE
 * is set). Callers can gate expensive `details` construction on this
 * to skip work that would be immediately discarded.
 */
bool provenanceEnabled();

} // namespace nix
