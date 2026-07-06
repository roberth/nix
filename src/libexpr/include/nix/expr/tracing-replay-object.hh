#pragma once

#include "nix/expr/arg-cell.hh"
#include "nix/expr/subject-id.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/tracing-writer.hh"

#include <functional>

namespace nix {

class Store;
class TracingReplayEvaluator;

/**
 * Object that returns cached results from the trie index, with lazy fallback
 * to the inner evaluator when cache misses occur.
 *
 * Uses cascading lookup: temporal children → structural children → shortcuts.
 * Objects are only created for validated trie positions. Child lookups
 * perform incremental validation from the child back to the current position.
 */
class TracingReplayObject : public Object
{
    TracingReplayEvaluator & evaluator;
    TriePosition triePos;

    std::function<ref<Object>()> getInner;
    mutable std::optional<ref<Object>> inner;

    /* Argument-argAncestry cell. Apply-result proxies open a fresh cell
       rooted at the fn's cell; navigation children carry the same
       cell as their parent. Cell's own `parent` field gives the
       ancestor chain. */
    std::shared_ptr<const ArgCell> argCell;

    /* Per-cb-apply observation context for the apply that produced
       this object. Set on apply-result wrappers by
       TracingReplayEvaluator::apply when the arg was a cb-arg
       OuterObject carrying one. Retained for the
       finalised-on-ensureInner side-channel that other code paths
       still inspect; not used for evolvedQueryFrom under the
       option-2 encoding (which routes through the evaluator's
       cumulative envWalk). */
    std::shared_ptr<ApplyContext> applyContext;
    /* When apply-result, the ApplyResultSubject identifying it
       structurally + the inherited argAncestry (= state hash(Q)). Used together
       with the evaluator's envWalk to compute the evolved state hash
       at lookup time via the same formula the writer's TracingObject
       uses. */
    std::optional<Subject> applyResultSubject;
    Hash applyArgAncestry{HashAlgorithm::SHA256};

    ref<Object> ensureInner() const;

    std::string evolvedQueryFrom() const;
    /** Candidate parent hashes for child Q construction. First is the
        full evolved hash (= stateHashAt over current applyContext
        observations); if `postWHNFObservationCount` is set and points
        to a smaller fact set, a second candidate is the parent hash
        at that prefix (= the parent's "initial discovery state,"
        matching sibling-attr recordings in independent warmups).
        IDs are always consistent with their fact set; querying at a
        smaller fact set aligns with recordings made at that state. */
    std::vector<std::string> parentHashCandidates() const;
    void pushObservation(const std::string & fromHex, const Hash & queryHash, const Hash & responseHash);

    /**
     * Cascading lookup for leaf results. Returns the parsed R plus
     * the recorded resultHash so callers can push an observation
     * onto the per-invocation walk for chain symmetry with the writer.
     */
    template<typename Q, typename R>
    std::optional<std::pair<R, Hash>> lookupResult(const Q & query) const;

    /**
     * Cascading lookup for structural children (getAttr, getListElem).
     * Returns the result payload and child TriePosition for further traversal.
     */
    template<typename Q, typename R>
    std::optional<std::pair<R, TriePosition>> lookupStructuralChild(const Q & query) const;

    /* Memoized WHNF lookup. First call to any of the WHNF-subsumed
       getters (getType / getInt / getString / etc.) fires `whnf()`
       which looks up the recorded QueryGetWHNF response. */
    mutable std::optional<trace::ResultWHNF> cachedWHNF;
    std::optional<const trace::ResultWHNF *> whnf();

    /* Snapshot of `applyContext->observations.size()` right after
       this TracingReplayObject's own `whnf()` push. The "parent's initial discovery
       state" — sibling attr probes recorded in independent sessions
       were stamped with parent hash = stateHashAt at THIS prefix.
       parentHashCandidates uses it as a second lookup candidate
       (= "query at a smaller fact set so IDs align with recordings
       made before sibling probes accumulated"). */
    mutable std::optional<size_t> postWHNFObservationCount;

public:
    TracingReplayObject(
        TracingReplayEvaluator & evaluator, TriePosition triePos, std::function<ref<Object>()> getInner);

    /** Set the proxy's argCell. Returns *this for chaining. */
    TracingReplayObject & withArgCell(std::shared_ptr<const ArgCell> argScope_)
    {
        argCell = std::move(argScope_);
        return *this;
    }

    /** Attach the per-apply observation context — for apply-result
        wrappers, so subsequent queries can compute the evolved
        state hash via subject-id. */
    TracingReplayObject & withApplyContext(
        std::shared_ptr<ApplyContext> ctx, Subject resultSubject)
    {
        applyContext = std::move(ctx);
        applyResultSubject = std::move(resultSubject);
        if (applyContext)
            applyArgAncestry = applyContext->argAncestry;
        return *this;
    }

    /** Attach the apply-result Subject + argAncestry without going through
        ApplyContext. Mirrors the writer-side
        `TracingObject::withApplyResultSubject`. */
    TracingReplayObject & withApplyResultSubject(Subject subject, Hash argAncestry)
    {
        applyResultSubject = std::move(subject);
        applyArgAncestry = std::move(argAncestry);
        return *this;
    }

    /** Attach just the ApplyContext (for the finalised side-channel),
        leaving applyResultSubject/applyArgAncestry alone. Used by
        TracingReplayEvaluator::apply after it has already set the
        Subject via withApplyResultSubject. */
    TracingReplayObject & withApplyContextOnly(std::shared_ptr<ApplyContext> ctx)
    {
        applyContext = std::move(ctx);
        return *this;
    }

    std::shared_ptr<ApplyContext> getApplyContext() const { return applyContext; }

    std::shared_ptr<const ArgCell> getProxyArgCell() const override { return argCell; }

    /** Symmetric to `TracingObject::getSubject()`: surface the apply-
        result Subject when this wrapper is an apply result so the
        next apply / further queries build `ApplyResultSubject{...}`
        constituents whose state hashes evolve via subject-id own-loop, instead
        of falling back to `PostulatedIdempotentRead{this.state hash}`. */
    const Subject * getSubject() const override
    {
        return applyResultSubject ? &*applyResultSubject : nullptr;
    }

    Hash getArgAncestry() const override { return applyArgAncestry; }

    const TriePosition & getTriePos() const
    {
        return triePos;
    }

    std::optional<std::string> getStateHashHex() const override { return triePos.queryHashStr; }

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
    /** Object-method apply entry. Delegates to the
        TracingReplayEvaluator's apply (= walker lookup + cached
        TracingReplayObject wrapping with the lazy callback to
        inner), then returns the result as a shared_ptr<Object>.
        This is the Object-method counterpart of
        TracingReplayEvaluator::apply, letting callers route apply
        through queryApply uniformly. */
    std::shared_ptr<Object> queryApply(std::shared_ptr<Object> argObj) override;
};

} // namespace nix
