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
 * with this object in place, `resolveAmbientId` for an apply tag
 * can invoke `fn->queryApply(replayArg)` live, and downstream
 * apply-result Facts get dispatched against the live outer's
 * response. If the outer changed (different lambda body) the
 * response differs from the recorded → walk falls through. Without
 * it the dispatcher would just serve the recorded response, hiding
 * outer-side changes from the validation chain.
 */

#include "nix/expr/evaluator.hh"
#include "nix/expr/trace-ids.hh"

namespace nix {

class TracingDecisionGraph;

class ReplayLocalObject : public Object
{
    AmbientId localId;
    TracingDecisionGraph & decisionGraph;
    ref<SourceRoot> rootFSRoot;
    /* When a parent's maybeGetAttr / getListElem produces this child,
       it has the child's type from its own response. Recorder-side
       TracingLocalObject::maybeGetAttr never invokes getType on a
       wrapped child — it gets the type from the inner Object directly
       — so no QueryGetType fact for this child lands in the cache.
       Replay carries the type forward in-band instead of trying to
       look up a fact that doesn't exist. */
    std::optional<ObjectType> knownType;

public:
    ReplayLocalObject(AmbientId localId, TracingDecisionGraph & dg, ref<SourceRoot> rootFSRoot)
        : localId(localId), decisionGraph(dg), rootFSRoot(std::move(rootFSRoot)) {}

    ReplayLocalObject(AmbientId localId, TracingDecisionGraph & dg, ref<SourceRoot> rootFSRoot, ObjectType type)
        : localId(localId), decisionGraph(dg), rootFSRoot(std::move(rootFSRoot)), knownType(type) {}

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
};

} // namespace nix
