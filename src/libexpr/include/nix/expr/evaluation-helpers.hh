#pragma once
/**
 * @file
 * Helper functions for working with the Evaluator interface.
 */

#include "nix/expr/evaluator.hh"
#include "nix/store/store-api.hh"
#include "nix/util/suggestions.hh"

namespace nix::expr::helpers {

/**
 * Check if an Object represents a derivation.
 *
 * A derivation is identified by having a "type" attribute with the value "derivation".
 *
 * Intentional duplication: EvalState::isDerivation(Value &) is semantically
 * equivalent but operates on Value directly. That version stays for hot-path
 * callers to avoid Object overhead (symbol interning, heap allocation, GC root).
 * This version is for callers already using the Object interface.
 *
 * @param obj The Object to check
 * @return true if the object is a derivation, false otherwise
 */
bool isDerivation(Object & obj);

/**
 * Force evaluation of a derivation and return its store path.
 *
 * Similar implementations exist in AttrCursor::forceDerivation() and
 * PackageInfo::queryDrvPath(). This version operates on the Object interface.
 *
 * @param evaluator The Evaluator to check read-only mode
 * @param obj The Object representing the derivation (must have type = "derivation")
 * @param store The store to parse and validate the derivation path
 * @return The store path of the derivation
 * @throws Error if drvPath attribute is missing, not a derivation path,
 *         or the derivation doesn't exist in store (unless read-only mode)
 */
StorePath forceDerivation(Evaluator & evaluator, Object & obj, Store & store);

/**
 * Get the outputs to install for a derivation based on its metadata.
 *
 * The logic follows this priority:
 * 1. If outputSpecified = true, use outputName attribute
 * 2. Otherwise, if meta.outputsToInstall exists, use that list
 * 3. Otherwise, default to ["out"]
 *
 * Note: This does NOT validate that the returned output names actually exist
 * in the derivation. Validation happens downstream when building. This is
 * intentional for the flake use case where malformed derivations fail at build
 * time. For stricter validation (as needed by nix-env), use PackageInfo::queryOutputs.
 *
 * @param obj The Object representing a derivation
 * @return Set of output names to install
 */
StringSet getDerivationOutputs(Object & obj);

/**
 * Navigate an attribute path through nested attrsets.
 *
 * Does not auto-call functors or functions.
 *
 * @param obj The root object to start navigation from
 * @param attrPath The attribute path to follow (e.g., ["packages", "x86_64-linux", "hello"])
 * @return The object at the end of the path, or suggestions if attribute not found
 */
OrSuggestions<std::shared_ptr<Object>> findAlongAttrPath(Object & obj, const std::vector<std::string> & attrPath);

/**
 * Try multiple attribute paths and return the first one that succeeds.
 *
 * @param obj The root object to start navigation from
 * @param attrPaths List of attribute paths to try (as strings, will be parsed)
 * @param state EvalState for parsing attribute paths
 * @return A pair of (found object, actual path used), or suggestions if none found
 */
OrSuggestions<std::pair<std::shared_ptr<Object>, std::string>>
tryAttrPaths(Object & obj, const std::vector<std::string> & attrPaths, EvalState & state);

} // namespace nix::expr::helpers