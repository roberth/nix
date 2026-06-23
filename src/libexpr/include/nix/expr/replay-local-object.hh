#pragma once
/**
 * @file
 * ReplayLocalObject: serves recorded responses for a local arg
 * during replay-time apply invocation.
 *
 * The OUTER's covariant callback (e.g. `f x` where `f` is an outer
 * lambda and `x` is an inner-supplied arg) lets the outer access
 * inner-side data. On the recording side the inner wraps the arg in
 * TracingLocalObject so the outer's accesses land in the inner's
 * factSet as Facts. On replay the inner isn't running, so its arg
 * isn't reconstructable as a live Object — but its CONTENT was
 * persisted in the Responses pool. ReplayLocalObject reads that
 * content back so the outer can invoke its callback against a
 * deterministic frozen image of the recorded arg.
 *
 * This is what makes covariant-callback caching actually validate:
 * with this object in place, `resolveCdiId` for an apply tag
 * can invoke `fn->queryApply(replayArg)` live, and downstream
 * apply-result Facts get dispatched against the live outer's
 * response. If the outer changed (different lambda body) the
 * response differs from the recorded → walk falls through. Without
 * it the dispatcher would just serve the recorded response, hiding
 * outer-side changes from the validation chain.
 */

#include "nix/expr/arg-scope.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/trace-ids.hh"

namespace nix {

class TracingDecisionGraph;

class ReplayLocalObject : public Object
{
    AmbientId localId;
    TracingDecisionGraph & decisionGraph;
    ref<SourceRoot> rootFSRoot;
    /* EvalState used for primop construction in `defeatCache`. The
       outer's EvalState (= where the primop will be applied) is the
       right one in principle; in practice any live EvalState works
       because primop construction allocates a Value off the shared
       gc heap and the lambda capture holds onto the construction
       arguments by value. Threaded from `materialiseLocalStandin`. */
    EvalState * state;
    /* When a parent's maybeGetAttr / getListElem produces this child,
       it has the child's type from its own response. Recorder-side
       TracingLocalObject::maybeGetAttr never invokes getType on a
       wrapped child — it gets the type from the inner Object directly
       — so no QueryGetType fact for this child lands in the cache.
       Replay carries the type forward in-band instead of trying to
       look up a fact that doesn't exist. */
    std::optional<ObjectType> knownType;

    /* Argument-scope cell. Navigation children carry the same cell
       as their parent; the top-level (cb-arg) Local carries the
       apply's cell. Cell's own `parent` field gives ancestor chain. */
    std::shared_ptr<const ArgScopeCell> argScope;

public:
    ReplayLocalObject(AmbientId localId, TracingDecisionGraph & dg, ref<SourceRoot> rootFSRoot, EvalState * state = nullptr)
        : localId(localId), decisionGraph(dg), rootFSRoot(std::move(rootFSRoot)), state(state) {}

    ReplayLocalObject(AmbientId localId, TracingDecisionGraph & dg, ref<SourceRoot> rootFSRoot, ObjectType type, EvalState * state = nullptr)
        : localId(localId), decisionGraph(dg), rootFSRoot(std::move(rootFSRoot)), state(state), knownType(type) {}

    /** Set the proxy's argScope. Returns *this for chaining. */
    ReplayLocalObject & withScope(std::shared_ptr<const ArgScopeCell> argScope_)
    {
        argScope = std::move(argScope_);
        return *this;
    }

    std::shared_ptr<const ArgScopeCell> getProxyArgScope() const override { return argScope; }

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
    ObjectType getType() override;
    ObjectType getTypeLazy() override;
    RootValue defeatCache() override;
    std::optional<FunctionInfo> getFunctionInfo() override;
    /** Recorded LocalObjects (frozen images) can't be applied without
        either reconstructing the function body from value-structure
        atoms (task #75) or comparing the live arg's content to the
        recorded arg's content (task #74's depth-2 walker). Until one
        of those lands, an apply on a ReplayLocalObject is undecidable
        — we don't know whether the recorded result still applies for
        the current live arg. Throw a recognizable signal that
        callers can interpret as "walker miss, fall through to live
        re-eval."

        Today no caller routes here: the apply chain still goes
        through `Object::defeatCache` + value-level `callFunction`,
        not `Object::queryApply`. The override exists so that when
        callers are restructured to call `queryApply` uniformly, this
        is the entry that fires. The provenance tag in
        `AmbientRegistry` keeps the existing-callers path correct
        until the restructure lands. */
    std::shared_ptr<Object> queryApply(std::shared_ptr<Object> argObj) override;
};

} // namespace nix
