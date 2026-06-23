#!/usr/bin/env bash

# CDI #8: preFlushSubstitutions overwrite invariant.
#
# Two sibling cb invocations in one process whose deferred apply Qs
# share an oldHash (because args' initial CDI is the same empty-cell
# hash) trigger the invariant. The warning fires loudly via
# tracingCacheLog when _NIX_TRACING_CACHE_LOGGING=1; this test asserts
# it's visible.
#
# When #63 is fixed (so the collision stops happening at all), this
# test should be re-evaluated: either the warning should escalate to a
# throw and this test become a negative-assertion ("no collision
# warning"), or the warning text changes.

source common.sh

# TODO(depth-2): this whole test probes the removed
# `pre_flush_substitution_collisions` metric from the
# substitution-machinery era. The via-Asks design replaces that
# machinery entirely — there's no collision counter to assert against
# anymore. Skip until we have a corresponding invariant in the new
# design (likely something like "AmbientAsks edges' (fromFactSet,
# requestSet) keys remain unique under sibling cb invocations").
echo "skipped: probes removed pre_flush_substitution_collisions metric"
exit 77
