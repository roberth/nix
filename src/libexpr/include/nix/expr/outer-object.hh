#pragma once
/**
 * @file
 * OuterObject — Object backed by an outer query callback.
 *
 * A value from the outer evaluator, accessed by the local
 * (inner) evaluator through outer queries. Each Object method issues
 * a query through the provided callback and interprets the response.
 */

#include "nix/expr/arg-cell.hh"
#include "nix/expr/subject-id.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/source-root.hh"
#include "nix/expr/trace-ids.hh"
#include "nix/expr/trace-types.hh"

#include <functional>
#include <optional>
#include <string>

namespace nix {

/**
 * Response from an outer query: the result plus an optional child
 * Object for queries that produce child Objects (getAttr, getListElem).
 * The child is the outer's Object at the queried position, which the
 * caller wraps in a new OuterObject.
 */
struct OuterQueryResult
{
    trace::ResultVariant result;
    std::shared_ptr<Object> child; // outer's child Object, if applicable
};

/**
 * Callback type for issuing outer queries. Takes the outer Object
 * to query, the query itself, and the caller's producer Selector
 * (identifies the caller for logging + attribution). Passing the
 * outer Object directly (rather than an id) reflects that OuterObject
 * wraps a specific Object from a different Interpreter.
 */
using OuterQueryFn = std::function<OuterQueryResult(
    std::shared_ptr<Object> outerObj,
    const trace::SelectorVariant &,
    trace::SelectorVariant producer)>;

/**
 * Callback type for outer function application. Takes the outer fn
 * Object, the calling OuterObject's producer Selector (identifies
 * the fn for the SelectorApply payload — the outer Object itself
 * typically has no producer, so the wrapping OuterObject provides
 * it), the argument Object, and the calling OuterObject's effective
 * argCell (the chain root from which the new local cell's depth
 * descends). Returns the outer's apply-result Object.
 *
 * Why pass `callerScope`: the cb is reached via a navigation chain
 * (e.g. arg.items[0]), and `fnObj` may not carry a proxy parent chain
 * — so the callee can't infer depth from `fnObj` alone. The caller
 * (OuterObject::queryApply) knows its own proxy graph position and
 * threads the effective cell through.
 */
using OuterApplyFn = std::function<std::shared_ptr<Object>(
    std::shared_ptr<Object> fnObj,
    trace::SelectorVariant fnProducer,
    std::shared_ptr<Object> argObj,
    std::shared_ptr<const ArgCell> callerScope)>;

/**
 * Object implementation backed by outer queries to the outer evaluator.
 * Each method composes a Query, issues it via the callback, and
 * interprets the Result.
 */
class OuterObject : public Object
{
    /** #183: producer Selector — the SelectorVariant whose content hash
        IS this OuterObject's identity. */
    trace::SelectorVariant producer;
    /* The Object from the outer Interpreter this proxy wraps.
       Methods on OuterObject dispatch through this reference: the
       inner side asks OuterObject (via Object interface), OuterObject
       dispatches the equivalent method on `outerObj` (executing in the
       outer's Interpreter), and hands the result back to the inner. */
    std::shared_ptr<Object> outerObj;
    /* Per-apply observation context. Set on cb-arg arg OuterObjects
       by makeCachedFnPrimOp.impl at the cb-apply; the queryFn
       closure routes observations through this context so the
       apply-result wrapping can compute its evolved state hash via
       stateHashAfter against the accumulated history. Null on
       non-cb-arg OuterObjects. */
    std::shared_ptr<ApplyContext> applyContext;
    OuterQueryFn queryFn;   ///< Callback to issue outer queries
    OuterApplyFn applyFn;   ///< Callback for function application (may be null)
    /* lazy-paths: stable SourceRoot for paths returned by `getPath`.
       Held as a member so the SourceRoot outlives the Value the
       outer evaluator constructs from the RootedPath (Value stores a
       raw SourceRoot pointer). Resolved at construction by the
       resolver from the outer EvalState's `rootFSRoot`. */
    ref<SourceRoot> outerRootFSRoot;

    /* Argument-argAncestry wiring. `argCell` is the nearest enclosing
       apply's cell — navigation children carry the same cell as their
       parent; apply-result proxies open a fresh cell rooted at the
       fn's cell. The cell carries its own `parent` field, so the
       ancestor chain is reachable from the cell — no proxy `parent`
       field needed. */
    std::shared_ptr<const ArgCell> argCell;

    /* Memoized WHNF observation. First call to any of getType / getInt /
       getString / etc. fires `whnf()`, which issues ONE SelectorGetWHNF
       through `queryFn`. Subsequent calls decode the cached result
       without re-querying. */
    std::optional<trace::ResultWHNF> cachedWHNF;
    trace::ResultWHNF & whnf();

public:
    OuterObject(trace::SelectorVariant producer, std::shared_ptr<Object> outerObj, OuterQueryFn queryFn, ref<SourceRoot> outerRootFSRoot, OuterApplyFn applyFn = {});

    /** Set the proxy's argCell. Call right after construction at
        boundary sites. Returns *this for chaining. */
    OuterObject & withArgCell(std::shared_ptr<const ArgCell> argScope_)
    {
        argCell = std::move(argScope_);
        return *this;
    }

    /** Attach a per-apply observation context. Used on cb-arg arg
        OuterObjects at the cb-apply; the queryFn closure
        routes observations into this context. */
    OuterObject & withApplyContext(std::shared_ptr<ApplyContext> ctx)
    {
        applyContext = std::move(ctx);
        return *this;
    }

    /** Read this proxy's apply context (= null unless this is a
        cb-arg arg). */
    std::shared_ptr<ApplyContext> getApplyContext() const { return applyContext; }

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
    RootValue toValueOrProxy(EvalState & state, std::shared_ptr<OuterResolver> resolver) override;
    std::optional<FunctionInfo> getFunctionInfo() override;
    PosIdx getPos() override;
    std::optional<std::vector<std::string>> getAttrPath() override;

    /**
     * Issue a SelectorApply. The resolver registers the arg and creates
     * the lazy application. Returns an OuterObject wrapping the result.
     */
    std::shared_ptr<Object> queryApply(std::shared_ptr<Object> argObj) override;

    /** State-hash hex = content hash of the stored producer Selector.
        Under Q-space identity every OuterObject has a stable identity
        derived from its producer — no separate `producingQHex`
        override needed. */
    std::optional<std::string> getStateHashHex() const override
    {
        return TracingDecisionGraph::computeSelectorHash(producer)
            .to_string(HashFormat::Base16, false);
    }
};

} // namespace nix
