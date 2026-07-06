#pragma once
/**
 * @file
 * LambdaApplyResultObject — writer-side wrapper for the result of
 * applying a TracingCallbackArg (= an inner-supplied lambda crossing
 * back from outer's body via `<cached-fn>(TLO).impl`) to its contraArg.
 *
 * Sibling of TracingObject; the difference is *where* method-level
 * observations land:
 *
 *  - TracingObject's getType/getInt/etc. record sub-Q `Terminals`
 *    rows in the main trie via `writer.logQuery + logResult`. That's
 *    d=1 storage — appropriate for cached-fn results and other
 *    apply-results whose evolved argStateId participates in the d=1 walk.
 *
 *  - LambdaApplyResultObject's methods record d=2 observations via
 *    `writer.logAmbientObservation`. They are grouped with the
 *    enclosing cb-apply boundary's recursive apply Fact (= the same
 *    boundary `logAmbientApplyFact` appended to). At flushAmbient
 *    finalize the writer's d=2 loop stamps each observation with
 *    `from = hex(scopeStateIdAt(applyResultSubject, scope, walk, i))`,
 *    inserts the response payload into `LocalResponseMap` keyed by
 *    the resulting reqHash, and inserts an `AmbientAsks` edge.
 *
 * The walker's `<replay-local-lambda>` primop, when its synthetic
 * apply-result probes `getType` / `getInt` etc. via
 * `ReplayCallbackArg`, computes the same reqHash via
 * `stampPerArgFields(query, syntheticSubject, syntheticScope,
 * walkFacts, walkFacts.size())` (where `walkFacts.size() == 1` after
 * the primop pushed the recursive apply Fact). Lookup keys agree by
 * the cidasks formula on both sides; the response payload comes
 * back; the synthetic's `advanceChainAndAppendFact` consumes the
 * matching AmbientAsks edge.
 *
 * Closes the d=1 main-trie bypass diagnosed in
 * `tracing-eval-cache-higher-order-replay.md` — the recordings land
 * exactly where the walker's lambda-LO mechanism reads from.
 */

#include "nix/expr/arg-scope.hh"
#include "nix/expr/content-identity-via-asks.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/trace-types.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/util/hash.hh"
#include "nix/util/ref.hh"

#include <optional>
#include <string>

namespace nix {

class LambdaApplyResultObject : public Object
{
    ref<Object> inner;
    TracingWriter & writer;

    /* ApplyResultSubject{PostulatedIdempotentRead{TLO.scopeStateId}, contraArg.subject}.
       Matches what `<replay-local-lambda>`'s primop builds for the
       synthetic apply-result subject at warm. flushAmbient's
       d=2 loop uses this subject to stamp each observation's `from`
       field at the appropriate edge index. */
    cidasks::Subject applyResultSubject;

    /* Scope inherited from the cb-apply boundary — = contraArg's
       inheritedScope = the resolver's callScope. The walker's
       sidecar lookup recovers the same scope. */
    Hash applyScope;

    /* The enclosing cb-apply boundary's `applyId` (= what `runOn`
       computed as `queryHash(QueryApply{fn, arg})` when it pushed
       this boundary). Captured BEFORE `IT::apply`'s
       `openApplyBoundary` would push a new entry, so the
       observations route to the correct boundary's d=2 chain. */
    Hash depth2ApplyId;

    /* scopeStateIdAfter(applyResultSubject, applyScope, {}) hex — the
       content-only apply-result argStateId exposed via getScopeStateIdHex. Computed
       once at construction to match `TracingEvaluator::apply`'s
       `applyScopeStateIdHex` (= what the walker computes too). */
    std::string applyScopeStateIdHex;

    /* Argument-scope cell — same shape as TracingObject. */
    std::shared_ptr<const ArgScopeCell> argScope;

    /* Memoized WHNF observation. First call to any of getType / getInt /
       getString / etc. fires `whnf()`, which records ONE QueryGetWHNF
       d=2 observation. Subsequent calls decode the cached result. */
    std::optional<trace::ResultWHNF> cachedWHNF;
    trace::ResultWHNF & whnf();

    void recordD2(const trace::QueryVariant & query, const trace::ResultVariant & result);

public:
    LambdaApplyResultObject(
        ref<Object> inner,
        TracingWriter & writer,
        cidasks::Subject applyResultSubject,
        Hash applyScope,
        Hash depth2ApplyId);

    LambdaApplyResultObject & withScope(std::shared_ptr<const ArgScopeCell> cell)
    {
        argScope = std::move(cell);
        return *this;
    }

    std::shared_ptr<const ArgScopeCell> getProxyArgScope() const override { return argScope; }

    /** Symmetric to TracingObject/TracingReplayObject: surface the
        ApplyResultSubject so a subsequent apply on this wrapper
        composes evolving ApplyResultSubject constituents instead of
        the frozen PostulatedIdempotentRead{applyScopeStateIdHex} fallback. */
    const cidasks::Subject * getSubject() const override { return &applyResultSubject; }

    Hash getInheritedScope() const override { return applyScope; }

    std::optional<std::string> getScopeStateIdHex() const override { return applyScopeStateIdHex; }

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
