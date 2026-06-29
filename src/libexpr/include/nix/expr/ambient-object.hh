#pragma once
/**
 * @file
 * AmbientObject — Object backed by an ambient query callback.
 *
 * A value from the ambient (outer) evaluator, accessed by the local
 * (inner) evaluator through ambient queries. Each Object method issues
 * a query through the provided callback and interprets the response.
 */

#include "nix/expr/arg-scope.hh"
#include "nix/expr/content-identity-via-asks.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/source-root.hh"
#include "nix/expr/trace-ids.hh"
#include "nix/expr/trace-types.hh"

#include <functional>
#include <optional>
#include <string>

namespace nix {

/**
 * Response from an ambient query: the result plus an optional child id
 * for queries that produce child Objects (getAttr, getListElem, apply).
 */
struct AmbientQueryResult
{
    trace::ResultVariant result;
    std::optional<AmbientId> childId; // id of child Object in the resolver, if applicable
};

/**
 * Callback type for issuing ambient queries. Takes the caller's
 * Object id, the query, the caller's Subject, and the caller's
 * inherited scope (both for content-id attribution at the writer).
 */
using AmbientQueryFn = std::function<AmbientQueryResult(
    AmbientId objectId,
    const trace::QueryVariant &,
    cidasks::Subject,
    Hash inheritedScope)>;

/**
 * Callback type for ambient function application.
 * Takes the function's Object id, the argument Object, and the
 * calling AmbientObject's effective argScope cell (the chain
 * root from which the new local cell's depth descends). Returns
 * the result Object id.
 *
 * Why pass `callerScope`: the cb is reached via a navigation
 * chain (e.g. seed.items[0]), and `resolve(fnId)` may return an
 * InterpreterObject without a proxy parent chain — so the
 * callee can't infer depth from the resolved fn. The caller
 * (AmbientObject::queryApply) knows its own proxy graph
 * position and threads the effective cell through.
 */
using AmbientApplyFn = std::function<AmbientId(
    AmbientId fnId, std::shared_ptr<Object> argObj, std::shared_ptr<const ArgScopeCell> callerScope)>;

/**
 * Object implementation backed by ambient queries to the outer evaluator.
 * Each method composes a Query, issues it via the callback, and
 * interprets the Result.
 */
class AmbientObject : public Object
{
    cidasks::Subject subject; ///< Static structural identifier (positional/derived/apply)
    /* Inherited scope: XOR of outer-scope argStateIds (chiefly the cached
       call's argStateId(Q)) for content-id inheritance, per
       content-identity-via-asks.md. Set at the cb-apply boundary;
       propagated to children. Zero hash if no inheritance. */
    Hash inheritedScope;
    /* Per-apply observation context. Set on cb-arg seed AmbientObjects
       by makeCachedFnPrimOp.impl at the apply boundary; the queryFn
       closure routes observations through this context so the
       apply-result wrapping can compute its evolved scope state id via
       cidasks::scopeStateIdAfter against the accumulated walk. Null on
       non-cb-arg AmbientObjects. */
    std::shared_ptr<cidasks::ApplyContext> applyContext;
    AmbientQueryFn queryFn;   ///< Callback to issue ambient queries
    AmbientApplyFn applyFn;   ///< Callback for function application (may be null)
    /* lazy-paths: stable SourceRoot for paths returned by `getPath`.
       Held as a member so the SourceRoot outlives the Value the
       outer evaluator constructs from the RootedPath (Value stores a
       raw SourceRoot pointer). Resolved at construction by the
       resolver from the outer EvalState's `rootFSRoot`. */
    ref<SourceRoot> ambientRootFSRoot;

    /* Argument-scope wiring. `argScope` is the nearest enclosing
       apply's cell — navigation children carry the same cell as their
       parent; apply-result proxies open a fresh cell rooted at the
       fn's cell. The cell carries its own `parent` field, so the
       ancestor chain is reachable from the cell — no proxy `parent`
       field needed. */
    std::shared_ptr<const ArgScopeCell> argScope;

public:
    AmbientObject(cidasks::Subject subject, AmbientQueryFn queryFn, ref<SourceRoot> ambientRootFSRoot, AmbientApplyFn applyFn = {});

    /** This proxy's structural identity (positional / derived /
        apply-result), per the content-identity-via-asks design. */
    const cidasks::Subject * getSubject() const override { return &subject; }

    /** This proxy's inherited scope (outer-scope argStateIds composed),
        used by cidasks to make sibling cached-call recordings'
        scope state ids distinct. */
    Hash getInheritedScope() const override { return inheritedScope; }

    /** Set the proxy's argScope. Call right after construction at
        boundary sites. Returns *this for chaining. */
    AmbientObject & withScope(std::shared_ptr<const ArgScopeCell> argScope_)
    {
        argScope = std::move(argScope_);
        return *this;
    }

    /** Set the proxy's inherited scope (outer-scope argStateIds).
        Children created by this proxy inherit this scope. */
    AmbientObject & withInheritedScope(const Hash & h)
    {
        inheritedScope = h;
        return *this;
    }

    /** Attach a per-apply observation context. Used on cb-arg seed
        AmbientObjects at the cb-apply boundary; the queryFn closure
        routes observations into this context. */
    AmbientObject & withApplyContext(std::shared_ptr<cidasks::ApplyContext> ctx)
    {
        applyContext = std::move(ctx);
        return *this;
    }

    /** Read this proxy's apply context (= null unless this is a
        cb-arg seed). */
    std::shared_ptr<cidasks::ApplyContext> getApplyContext() const { return applyContext; }

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
    RootValue toValueOrProxy(EvalState & state, std::shared_ptr<AmbientResolver> resolver) override;
    std::optional<FunctionInfo> getFunctionInfo() override;
    PosIdx getPos() override;
    std::optional<std::vector<std::string>> getAttrPath() override;

    /**
     * Issue a QueryApply. The resolver registers the arg and creates
     * the lazy application. Returns an AmbientObject wrapping the result.
     */
    std::shared_ptr<Object> queryApply(std::shared_ptr<Object> argObj) override;

    AmbientId getCdi() const
    {
        /* scope state id at the empty factset, with this proxy's inherited
           scope applied. For multi-edge use, callers must pass the
           relevant walk via cidasks::scopeStateIdAt instead. */
        return cidasks::structuralAddressAfter(subject, inheritedScope, {});
    }

    std::optional<std::string> getScopeStateIdHex() const override
    {
        return getCdi().to_string(HashFormat::Base16, false);
    }
};

} // namespace nix
