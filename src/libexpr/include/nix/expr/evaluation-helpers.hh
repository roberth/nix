#pragma once
/**
 * @file
 * Helper functions for working with the Evaluator interface.
 */

#include "nix/expr/evaluator.hh"
#include "nix/store/store-api.hh"

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

} // namespace nix::expr::helpers