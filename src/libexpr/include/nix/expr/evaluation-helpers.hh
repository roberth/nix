#pragma once
/**
 * @file
 * Helper functions for working with the Evaluator interface.
 */

#include "nix/expr/evaluator.hh"

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

} // namespace nix::expr::helpers