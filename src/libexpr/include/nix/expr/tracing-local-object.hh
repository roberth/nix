#pragma once
/**
 * @file
 * TracingLocalObject — wraps a local (inner-side) Object passed to the
 * outer evaluator during a covariant callback. Each method call
 * records an "incoming" ambient Fact in the inner trace via the
 * writer's `logIncomingAmbientInteraction`, then delegates to the
 * wrapped Object.
 *
 * Responses *are* stored to the decisionGraph's Responses pool here
 * (this is the case the dispatcher can't recompute from live state
 * at replay time — the inner isn't running).
 */

#include "nix/expr/arg-scope.hh"
#include "nix/expr/content-identity-via-asks.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/source-root.hh"
#include "nix/expr/trace-ids.hh"
#include "nix/expr/trace-types.hh"
#include "nix/expr/tracing-decision-graph.hh"

#include <functional>
#include <memory>
#include <vector>

namespace nix {

class TracingWriter;

/**
 * Object decorator that records every access made on it as an
 * incoming ambient Fact. Used to wrap the argObj a covariant
 * callback receives from the inner side, so the outer's accesses
 * land in the inner trace.
 */
class TracingLocalObject : public Object
{
    std::shared_ptr<Object> inner;
    cidasks::Subject subject;  ///< Static structural identifier
    /* Inherited scope: XOR of outer-scope CDIs (CDI(Q) at the
       cb-apply boundary). Propagated to navigation children. */
    Hash inheritedScope;
    /* The cb apply this local belongs to (= apply's resultId). Used
       at flush to group depth-2 facts into an AmbientAsks edge per
       apply. Navigation children inherit. */
    Hash depth2ApplyId;
    TracingWriter & writer;
    ref<SourceRoot> rootFSRoot;

    /** This local's content id, scoped via inheritedScope. Computed
        on demand from `subject` + `inheritedScope`. */
    AmbientId localId() const { return cidasks::contentIdAfter(subject, inheritedScope, {}); }

    /* The argScope cell this local belongs to. Navigation children
       share the parent's cell. Used for scope-graph topology only;
       content ids are derived from `subject`, not the cell. */
    std::shared_ptr<const ArgScopeCell> argScope;

    void recordObservation(const trace::QueryVariant & query, const trace::ResultVariant & result);

public:
    TracingLocalObject(
        std::shared_ptr<Object> inner,
        cidasks::Subject subject,
        TracingWriter & writer,
        ref<SourceRoot> rootFSRoot,
        std::shared_ptr<const ArgScopeCell> argScope,
        Hash inheritedScope = Hash(HashAlgorithm::SHA256),
        Hash depth2ApplyId = Hash(HashAlgorithm::SHA256));

    /** This proxy's structural identity, per the
        content-identity-via-asks design. */
    const cidasks::Subject * getSubject() const override { return &subject; }

    /** This proxy's inherited scope. */
    Hash getInheritedScope() const override { return inheritedScope; }

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
    ObjectType getTypeLazy() override;
    ObjectType getType() override;
    RootValue defeatCache() override;
    std::optional<FunctionInfo> getFunctionInfo() override;
    PosIdx getPos() override;
    std::optional<std::vector<std::string>> getAttrPath() override;
    /** Object-method apply entry. Records the apply as a depth-2
        observation (= outer is applying this local to argObj), then
        delegates to `inner->queryApply(argObj)` and wraps the result
        as another TracingLocalObject with an `ApplyResultSubject`
        so further accesses on the result continue to land in the
        depth-2 trace. */
    std::shared_ptr<Object> queryApply(std::shared_ptr<Object> argObj) override;

    AmbientId getCdi() const { return localId(); }

    std::optional<std::string> getCdiHex() const override
    {
        return localId().to_string(HashFormat::Base16, false);
    }
};

} // namespace nix
