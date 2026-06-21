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
    AmbientId localId; ///< Identifies this local in `from` fields of recorded queries
    TracingWriter & writer;
    ref<SourceRoot> rootFSRoot;

    /* The argScope cell this local contributes observations to.
       Top-level Local (cb arg) and its navigation children share
       the same cell — observations on any of them XOR-fold into
       this cell's intrinsic (state creep). At flush, the cell's
       contentId() is the substituted value for `from=localId` in
       this local's recorded facts. */
    std::shared_ptr<const ArgScopeCell> argScope;

    /* Buffer this observation in the writer (deferred until logResult)
       and extend the cell's intrinsic by its placeholder-independent
       contribution. */
    void recordObservation(const trace::QueryVariant & query, const trace::ResultVariant & result);

public:
    TracingLocalObject(
        std::shared_ptr<Object> inner,
        AmbientId localId,
        TracingWriter & writer,
        ref<SourceRoot> rootFSRoot,
        std::shared_ptr<const ArgScopeCell> argScope);

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

    AmbientId getId() const
    {
        return localId;
    }
};

} // namespace nix
