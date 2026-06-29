#pragma once

#include "nix/expr/arg-scope.hh"
#include "nix/expr/content-identity-via-asks.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/util/hash.hh"
#include "nix/util/ref.hh"

#include <optional>
#include <string>

namespace nix {

/**
 * Object wrapper that logs all operations to a trace file and optionally
 * to a trie index via TracingWriter.
 */
class TracingObject : public Object
{
    ref<Object> inner;
    TracingWriter & writer;
    ValueHandle valueNum;
    std::optional<TriePosition> triePos;

    /* Argument-scope cell. Apply-result proxies (constructed by
       TracingEvaluator::apply) open a fresh cell rooted at the fn's
       cell; navigation children (maybeGetAttr / getListElem) inherit
       the parent's cell. Cell's own `parent` field carries the
       ancestor chain. */
    std::shared_ptr<const ArgScopeCell> argScope;

    /* For apply-result wrappers: the cidasks Subject that identifies
       this apply structurally (ApplyResultSubject{fn, arg}), and the
       inherited scope (= CDI(Q) at the cb-apply boundary). Child
       queries on this wrapper emit at
       `contentIdAt(applyResultSubject, applyScope, writer.d1CidasksWalk,
       walk.size())` — the per-arg evolved cdi the design's
       principle #3 requires for sibling discrimination. Null on
       non-apply-result wrappers (= navigation children). */
    std::optional<cidasks::Subject> applyResultSubject;
    Hash applyScope{HashAlgorithm::SHA256};

    /* Per-invocation observation context shared with the cb-arg
       AmbientObject's queryFn and propagated to derived children
       via shared_ptr. */
    std::shared_ptr<cidasks::ApplyContext> applyContext;

    /* Compute the wrapper's evolved CDI live from
       applyContext->observations. */
    std::string evolvedQueryFrom() const;

    void pushObservation(const std::string & fromHex, const Hash & queryHash, const Hash & responseHash);

    TracingObject(ref<Object> inner, TracingWriter & writer, ValueHandle valueNum, std::optional<TriePosition> triePos);

public:
    static ref<TracingObject> create(
        ref<Object> inner,
        TracingWriter & writer,
        ValueHandle valueNum,
        std::optional<TriePosition> triePos = std::nullopt);

    /** Set the proxy's argScope. Returns *this for chaining. */
    TracingObject & withScope(std::shared_ptr<const ArgScopeCell> argScope_)
    {
        argScope = std::move(argScope_);
        return *this;
    }

    /** Attach the apply-result structural identity — for apply-result
        wrappers, so subsequent child queries emit at the evolved cdi.
        Mirrors TracingReplayObject's machinery. */
    TracingObject & withApplyResultSubject(cidasks::Subject subject, Hash scope)
    {
        applyResultSubject = std::move(subject);
        applyScope = std::move(scope);
        return *this;
    }

    TracingObject & withApplyContext(std::shared_ptr<cidasks::ApplyContext> ctx)
    {
        applyContext = std::move(ctx);
        return *this;
    }

    std::shared_ptr<cidasks::ApplyContext> getApplyContext() const { return applyContext; }

    /** Expose the apply-result structural Subject when this wrapper
        is itself an apply result (= curried fn for the next apply, or
        target of further queries). Surfacing the Subject lets the
        next apply build `ApplyResultSubject{fn=this.subject, ...}`
        with constituents whose CDIs *evolve* via cidasks own-loop,
        instead of falling back to `OpaqueContent{this.cdi}` which
        freezes the CDI at construction time. Non-apply-result
        wrappers (= fresh from evalFile, navigation children)
        legitimately have no Subject — for those, the OpaqueContent
        fallback in callers describes an atom whose CDI is fully
        determined and not subject to observation-driven evolution. */
    const cidasks::Subject * getSubject() const override
    {
        return applyResultSubject ? &*applyResultSubject : nullptr;
    }

    /** Inherited scope for `contentIdAt(getSubject(), getInheritedScope(), …)`.
        For apply-result wrappers it's the cb-apply boundary's scope
        baked at construction. */
    Hash getInheritedScope() const override { return applyScope; }

    std::shared_ptr<const ArgScopeCell> getProxyArgScope() const override { return argScope; }

    /** Get the query hash string for trie identity, if available. */
    std::optional<std::string> getQueryHashStr() const
    {
        return triePos ? std::optional{triePos->queryHashStr} : std::nullopt;
    }

    std::optional<std::string> getCdiHex() const override { return getQueryHashStr(); }

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
    std::vector<std::string> getListOfStringsNoCtx() override;
    ObjectType getTypeLazy() override;
    ObjectType getType() override;
    RootValue defeatCache() override;
    std::optional<FunctionInfo> getFunctionInfo() override;
    PosIdx getPos() override;
    std::optional<std::vector<std::string>> getAttrPath() override;
    /** Object-method apply entry. Delegates to inner->queryApply and
        wraps the result as another TracingObject so subsequent
        accesses on the apply result are recorded as queries against
        the apply-result trie position. */
    std::shared_ptr<Object> queryApply(std::shared_ptr<Object> argObj) override;
};

} // namespace nix
