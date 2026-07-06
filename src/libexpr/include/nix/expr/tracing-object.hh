#pragma once

#include "nix/expr/arg-cell.hh"
#include "nix/expr/subject-id.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/util/hash.hh"
#include "nix/util/ref.hh"

#include <optional>
#include <string>

namespace nix {

/** Compute a value's WHNF in one pass by calling the Object's
    per-type getters. Used by TracingObject::whnf to record a single
    QueryGetWHNF observation, and by the walker's dispatch to compute
    the live response for a recorded QueryGetWHNF. */
trace::ResultWHNF computeWHNFFromObject(Object & obj);

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

    /* Argument-argAncestry cell. Apply-result proxies (constructed by
       TracingEvaluator::apply) open a fresh cell rooted at the fn's
       cell; navigation children (maybeGetAttr / getListElem) inherit
       the parent's cell. Cell's own `parent` field carries the
       ancestor chain. */
    std::shared_ptr<const ArgCell> argCell;

    /* For apply-result wrappers: the subject-id Subject that identifies
       this apply structurally (ApplyResultSubject{fn, arg}), and the
       inherited argAncestry (= state hash(Q) at the cb-apply boundary). Child
       queries on this wrapper emit at
       `stateHashAt(applyResultSubject, applyArgAncestry, writer.envWalk,
       walk.size())` — the per-arg evolved state hash the design's
       principle #3 requires for sibling discrimination. Null on
       non-apply-result wrappers (= navigation children). */
    std::optional<Subject> applyResultSubject;
    Hash applyArgAncestry{HashAlgorithm::SHA256};

    /* Per-invocation observation context shared with the cb-arg
       AmbientObject's queryFn and propagated to derived children
       via shared_ptr. */
    std::shared_ptr<ApplyContext> applyContext;

    /* Compute the wrapper's evolved state hash live from
       applyContext->observations. */
    std::string evolvedQueryFrom() const;

    void pushObservation(const std::string & fromHex, const Hash & queryHash, const Hash & responseHash);

    /* Memoized WHNF observation. First call to any of getType / getInt /
       getString / etc. fires `whnf()`, which records ONE QueryGetWHNF
       observation against this Object's identity carrying the type
       discriminator plus the type-determined payload. Subsequent calls
       on this Object decode the cached result without re-recording. */
    std::optional<trace::ResultWHNF> cachedWHNF;
    trace::ResultWHNF & whnf();

    TracingObject(ref<Object> inner, TracingWriter & writer, ValueHandle valueNum, std::optional<TriePosition> triePos);

public:
    static ref<TracingObject> create(
        ref<Object> inner,
        TracingWriter & writer,
        ValueHandle valueNum,
        std::optional<TriePosition> triePos = std::nullopt);

    /** Set the proxy's argCell. Returns *this for chaining. */
    TracingObject & withArgCell(std::shared_ptr<const ArgCell> argScope_)
    {
        argCell = std::move(argScope_);
        return *this;
    }

    /** Attach the apply-result structural identity — for apply-result
        wrappers, so subsequent child queries emit at the evolved state hash.
        Mirrors TracingReplayObject's machinery. */
    TracingObject & withApplyResultSubject(Subject subject, Hash argAncestry)
    {
        applyResultSubject = std::move(subject);
        applyArgAncestry = std::move(argAncestry);
        return *this;
    }

    TracingObject & withApplyContext(std::shared_ptr<ApplyContext> ctx)
    {
        applyContext = std::move(ctx);
        return *this;
    }

    std::shared_ptr<ApplyContext> getApplyContext() const { return applyContext; }

    /** Expose the apply-result structural Subject when this wrapper
        is itself an apply result (= curried fn for the next apply, or
        target of further queries). Surfacing the Subject lets the
        next apply build `ApplyResultSubject{fn=this.subject, ...}`
        with constituents whose state hashes *evolve* via subject-id own-loop,
        instead of falling back to `PostulatedIdempotentRead{this.state hash}` which
        freezes the state hash at construction time. Non-apply-result
        wrappers (= fresh from evalFile, navigation children)
        legitimately have no Subject — for those, the PostulatedIdempotentRead
        fallback in callers describes an atom whose state hash is fully
        determined and not subject to observation-driven evolution. */
    const Subject * getSubject() const override
    {
        return applyResultSubject ? &*applyResultSubject : nullptr;
    }

    /** Inherited argAncestry for `stateHashAt(getSubject(), getArgAncestry(), …)`.
        For apply-result wrappers it's the cb-apply boundary's argAncestry
        baked at construction. */
    Hash getArgAncestry() const override { return applyArgAncestry; }

    std::shared_ptr<const ArgCell> getProxyArgCell() const override { return argCell; }

    /** Get the query hash string for trie identity, if available. */
    std::optional<std::string> getQueryHashStr() const
    {
        return triePos ? std::optional{triePos->queryHashStr} : std::nullopt;
    }

    std::optional<std::string> getStateHashHex() const override { return getQueryHashStr(); }

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
