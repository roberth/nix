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

    /* Phase 4 of content-defined identity: the local's intrinsic
       content-hash, maintained as observations land. Each observation
       contributes a `(queryHash_with_blanked_from, responseHash)` pair
       — the from is blanked so the hash depends only on the
       observation's content, not on which counter-derived placeholder
       this local happens to be holding (so extensionally-equivalent
       locals collapse to the same intrinsic identity, per §2). The
       writer reads the latest value at flush time to substitute the
       placeholder in this local's deferred facts. */
    TracingDecisionGraph::SetHash intrinsicHash;

    /* Buffer this observation in the writer (deferred until logResult)
       and extend the intrinsic hash by its placeholder-independent
       contribution. */
    void recordObservation(const trace::QueryVariant & query, const trace::ResultVariant & result);

public:
    TracingLocalObject(
        std::shared_ptr<Object> inner, AmbientId localId, TracingWriter & writer, ref<SourceRoot> rootFSRoot);


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
