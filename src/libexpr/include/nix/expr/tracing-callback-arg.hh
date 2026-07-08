#pragma once
/**
 * @file
 * TracingCallbackArg — wraps a local (inner-side) Object passed to the
 * outer evaluator during a covariant callback. Each method call
 * records an "incoming" ambient Fact in the inner trace via the
 * writer's `logIncomingAmbientInteraction`, then delegates to the
 * wrapped Object.
 *
 * Responses *are* stored to the decisionGraph's InnerValueResponse
 * here (this is the case the dispatcher can't recompute from live
 * state at replay time — the inner isn't running).
 */

#include "nix/expr/arg-cell.hh"
#include "nix/expr/subject-id.hh"
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
class TracingCallbackArg : public Object
{
    std::shared_ptr<Object> inner;
    Subject subject;  ///< Static structural identifier
    /* Inherited argAncestry: XOR of outer-argAncestry state hashes (state hash(Q) at the
       cb-apply boundary). Propagated to navigation children. */
    Hash argAncestry;
    /* The cb apply this local belongs to (= apply's resultId). Used
       at flush to group ambient layer facts into an AmbientAsks edge per
       apply. Navigation children inherit. */
    Hash ambientApplyId;
    TracingWriter & writer;
    ref<SourceRoot> rootFSRoot;

    /** This local's state hash, scoped via argAncestry. Computed
        on demand from `subject` + `argAncestry`. */
    OuterId localId() const { return stateHashAfterSubject(subject, argAncestry, {}); }

    /* The argCell cell this local belongs to. Navigation children
       share the parent's cell. Used for scope-graph topology only;
       state hashes are derived from `subject`, not the cell. */
    std::shared_ptr<const ArgCell> argCell;

    /* Memoized WHNF observation. First call to any of getType / getInt /
       getString / etc. fires `whnf()`, which records ONE QueryGetWHNF
       ambient observation. Subsequent calls decode the cached result. */
    std::optional<trace::ResultWHNF> cachedWHNF;
    trace::ResultWHNF & whnf();

    void recordObservation(const trace::QueryVariant & query, const trace::ResultVariant & result);

public:
    TracingCallbackArg(
        std::shared_ptr<Object> inner,
        Subject subject,
        TracingWriter & writer,
        ref<SourceRoot> rootFSRoot,
        std::shared_ptr<const ArgCell> argCell,
        Hash argAncestry = Hash(HashAlgorithm::SHA256),
        Hash ambientApplyId = Hash(HashAlgorithm::SHA256));

    /** This proxy's structural identity, per the
        subject-id design. */
    const Subject * getSubject() const override { return &subject; }

    /** This proxy's inherited argAncestry. */
    Hash getArgAncestry() const override { return argAncestry; }

    std::shared_ptr<const ArgCell> getProxyArgCell() const override { return argCell; }

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
    /** Object-method apply entry. Records the apply as a ambient layer
        observation (= outer is applying this local to argObj), then
        delegates to `inner->queryApply(argObj)` and wraps the result
        as another TracingCallbackArg with an `ApplyResultSubject`
        so further accesses on the result continue to land in the
        ambient layer trace. */
    std::shared_ptr<Object> queryApply(std::shared_ptr<Object> argObj) override;

    OuterId getStateHash() const { return localId(); }

    std::optional<std::string> getStateHashHex() const override
    {
        return localId().to_string(HashFormat::Base16, false);
    }
};

} // namespace nix
