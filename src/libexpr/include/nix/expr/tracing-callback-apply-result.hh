#pragma once
/**
 * @file
 * TracingCallbackApplyResult — writer-side wrapper for the result of
 * applying a TracingCallbackArg (= an inner-supplied lambda crossing
 * back from outer's body via `<cached-fn>(TracingCallbackArg).impl`) to its contraArg.
 *
 * Sibling of TracingObject; the difference is *where* method-level
 * observations land:
 *
 *  - TracingObject's getType/getInt/etc. record sub-Q `Terminals`
 *    rows in the main trie via `writer.logQuery + logResult`. That's
 *    env storage — appropriate for cached-fn results and other
 *    apply-results whose evolved state hash participates in the env walk.
 *
 *  - TracingCallbackApplyResult's methods record ambient observations via
 *    `writer.logAmbientObservation`. They are grouped with the
 *    enclosing cb-apply boundary's recursive apply Fact (= the same
 *    boundary `logAmbientApplyFact` appended to). At flushAmbient
 *    finalize the writer's ambient loop stamps each observation with
 *    `from = hex(stateHashAt(applyResultSubject, argAncestry, walk, i))`,
 *    inserts the response payload into `InnerValueResponse` keyed by
 *    the resulting reqHash, and inserts an `AmbientAsks` edge.
 *
 * The walker's `<replay-local-lambda>` primop, when its synthetic
 * apply-result probes `getType` / `getInt` etc. via
 * `ReplayCallbackArg`, computes the same reqHash via
 * `stampPerArgFields(query, syntheticSubject, syntheticScope,
 * walkFacts, walkFacts.size())` (where `walkFacts.size() == 1` after
 * the primop pushed the recursive apply Fact). Lookup keys agree by
 * the subject-id formula on both sides; the response payload comes
 * back; the synthetic's `advanceChainAndAppendFact` consumes the
 * matching AmbientAsks edge.
 *
 * Closes the env main-trie bypass diagnosed in
 * `tracing-eval-cache-higher-order-replay.md` — the recordings land
 * exactly where the walker's lambda-LO mechanism reads from.
 */

#include "nix/expr/arg-cell.hh"
#include "nix/expr/subject-id.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/trace-types.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/util/hash.hh"
#include "nix/util/ref.hh"

#include <optional>
#include <string>

namespace nix {

class TracingCallbackApplyResult : public Object
{
    ref<Object> inner;
    TracingWriter & writer;

    /* ApplyResultSubject{PostulatedIdempotentRead{TracingCallbackArg.state hash}, contraArg.subject}.
       Matches what `<replay-local-lambda>`'s primop builds for the
       synthetic apply-result subject at warm. flushAmbient's
       ambient loop uses this subject to stamp each observation's `from`
       field at the appropriate edge index. */
    Subject applyResultSubject;

    /* Scope inherited from the cb-apply boundary — = contraArg's
       argAncestry = the resolver's callArgAncestry. The walker's
       sidecar lookup recovers the same argAncestry. */
    Hash applyArgAncestry;

    /* The enclosing cb-apply boundary's `applyId` (= what `runOn`
       computed as `queryHash(QueryApply{fn, arg})` when it pushed
       this boundary). Captured BEFORE `IT::apply`'s
       `openApplyBoundary` would push a new entry, so the
       observations route to the correct boundary's ambient chain. */
    Hash ambientApplyId;

    /* stateHashAfter(applyResultSubject, applyArgAncestry, {}) hex — the
       content-only apply-result state hash exposed via getStateHashHex. Computed
       once at construction to match `TracingEvaluator::apply`'s
       `applyArgAncestryStateHashHex` (= what the walker computes too). */
    std::string applyArgAncestryStateHashHex;

    /* Argument-argAncestry cell — same shape as TracingObject. */
    std::shared_ptr<const ArgCell> argCell;

    /* Memoized WHNF observation. First call to any of getType / getInt /
       getString / etc. fires `whnf()`, which records ONE QueryGetWHNF
       ambient observation. Subsequent calls decode the cached result. */
    std::optional<trace::ResultWHNF> cachedWHNF;
    trace::ResultWHNF & whnf();

    void recordD2(const trace::QueryVariant & query, const trace::ResultVariant & result);

public:
    TracingCallbackApplyResult(
        ref<Object> inner,
        TracingWriter & writer,
        Subject applyResultSubject,
        Hash applyArgAncestry,
        Hash ambientApplyId);

    TracingCallbackApplyResult & withArgCell(std::shared_ptr<const ArgCell> cell)
    {
        argCell = std::move(cell);
        return *this;
    }

    std::shared_ptr<const ArgCell> getProxyArgCell() const override { return argCell; }

    /** Symmetric to TracingObject/TracingReplayObject: surface the
        ApplyResultSubject so a subsequent apply on this wrapper
        composes evolving ApplyResultSubject constituents instead of
        the frozen PostulatedIdempotentRead{applyArgAncestryStateHashHex} fallback. */
    const Subject * getSubject() const override { return &applyResultSubject; }

    Hash getArgAncestry() const override { return applyArgAncestry; }

    std::optional<std::string> getStateHashHex() const override { return applyArgAncestryStateHashHex; }

    std::shared_ptr<Object> maybeGetAttr(const std::string & name) override;
    std::vector<std::string> getAttrNames() override;
    std::string getStringIgnoreContext() override;
    std::string getStringWithoutContext() override;
    std::pair<std::string, NixStringContext> getStringWithContext() override;
    RootedPath getPath() override;
    bool getBool(std::string_view errorCtx = "") override;
    NixInt getInt(std::string_view errorCtx = "") override;
    NixFloat getFloat(std::string_view errorCtx = "") override;
    size_t getListSize() override;
    std::shared_ptr<Object> getListElem(size_t index) override;
    ObjectType getTypeLazy() override;
    ObjectType getType() override;
    RootValue defeatCache() override;
    std::optional<FunctionInfo> getFunctionInfo() override;
    PosIdx getPos() override;
    std::optional<std::vector<std::string>> getAttrPath() override;
    std::shared_ptr<Object> queryApply(std::shared_ptr<Object> argObj) override;
};

} // namespace nix
