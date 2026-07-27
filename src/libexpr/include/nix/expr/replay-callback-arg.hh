#pragma once
/**
 * @file
 * ReplayCallbackArg: serves recorded responses for an inner-supplied
 * callback arg during replay-time apply invocation.
 *
 * The outer's covariant callback (e.g. `f x` where `f` is an outer
 * lambda and `x` is an inner-supplied arg) lets the outer access
 * inner-side data. On the recording side the inner wraps the arg in
 * TracingCallbackArg so the outer's accesses land in the enclosing
 * CallbackCell's runningObsSet, which is later snapshotted into a
 * SelectorCallbackApply request's referenced ObservationSet. On replay
 * the inner isn't running, so the arg isn't reconstructable as a
 * live Object — but its content was persisted by value inside that
 * recorded obsSet. ReplayCallbackArg reads probes back from the
 * obsSet so the outer can invoke its callback against a
 * deterministic frozen image of the recorded arg.
 *
 * This is what makes covariant-callback caching actually validate:
 * with this object in place, dispatching a recorded
 * SelectorCallbackApply materialises a ReplayCallbackArg backed by the
 * recorded obsSet, invokes `fn->queryApply(replayArg)` live, and
 * compares the response against the recording. If the outer changed
 * (different lambda body) the response differs and the walker
 * misses cleanly.
 */

#include "nix/expr/arg-cell.hh"
#include "nix/expr/subject-id.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/trace-ids.hh"
#include "nix/util/hash.hh"

namespace nix {

class TracingDecisionGraph;

class ReplayCallbackArg : public Object
{
    /* Full structural identity. Combined with `argAncestry` and the shared
       `walkFacts`, `stateHashAt` computes this proxy's state hash
       at any history position. The recorder's subject-id substitution at
       flush uses the same evaluation, so walker and recorder agree
       on per-probe `from` fields without snapshot/lazy hacks — even
       when a child's structural component depends on a parent's
       evolving state hash.

       For root (cb-apply) locals the subject is `Arg{depth}`
       with the recorded callArgAncestry (per the localArg sidecar), so the
       walker reproduces the recorder's
       `subjectId(Arg{D}, callArgAncestry)` directly.

       For children minted by maybeGetAttr/getListElem the subject is
       `DerivedSubject{parent.subject, ...}` — `stateHashAt`
       recursively re-evaluates the parent's state hash at the child's
       current edge index, so children don't need to snapshot parent
       state at creation. */
    trace::SelectorVariant producer;
    /* Initial state hash (= computeSelectorHash(producer)) — kept for
       legacy id-string consumers (e.g. defeatCache's recursive apply
       construction). */
    OuterId localId;
    /* Shared history across all proxies in one cb apply. Each validated
       probe appends a Fact (one fact per edge). `stateHashAt` reads
       this to compute each proxy's evolved state hash.

       Backed as a shared single-fact-edge sequence: each entry is
       wrapped in a single-fact ObservationSet so the history's edge indices match
       the recorder's flush history. */
    std::shared_ptr<std::vector<ObservationSet>> walkFacts;
    TracingDecisionGraph & decisionGraph;
    ref<SourceRoot> rootFSRoot;
    /* EvalState used for primop construction in `defeatCache`. The
       outer's EvalState (= where the primop will be applied) is the
       right one in principle; in practice any live EvalState works
       because primop construction allocates a Value off the shared
       gc heap and the lambda capture holds onto the construction
       arguments by value. Threaded from `materialiseLocalStandin`. */
    EvalState * state;

    /* obsSet response source: method responses are looked up in
       this map by selectorHash. Populated by the walker's
       callbackApply dispatch from the SelectorCallbackApply's
       referenced observation set — each entry is (selectorHash →
       CBOR response payload). */
    std::shared_ptr<std::map<Hash, std::string>> obsSetResponses;

    /* Memoized WHNF response. The recorder logs ONE SelectorGetWHNF
       observation per value force; the walker must reuse the cached
       response on any subsequent call. Without this, when
       `dispatchQueryRequest::navigatePath` invokes `queryApply`
       multiple times against the same ReplayCallbackArg (= once per fact
       dispatched on the apply result), each Apply Value's force
       re-fires the ReplayCallbackArg's surface probes and pushes a fresh fact
       past where the recorder stopped recording — the next lookup at
       `walkFacts.size() > recorded_size` then misses and the walker
       fails. */
    std::optional<trace::ResultWHNF> cachedWHNF;
    /** Read recorded WHNF for this proxy. Memoized; subsequent calls
        return the same result without re-probing. Returns the cached
        WHNF as a const reference so callers can decode the payload
        by alternative. */
    const trace::ResultWHNF & whnf();

    /* cb-arg apply context. `applyDepth` = `localCell->depth` at the
       recorder's OuterResolver::cb-apply. Used by the lambda primop
       to compose nested apply-result subjects matching the recorder's
       encoding (= `ApplyResultSubject{fn=this.subject, arg=Arg{depth+1}}`).
       Inherited unchanged through derived children (= the nested
       apply's positional depth is one deeper than the cb-arg's,
       regardless of attr/list navigation within the cb-arg's structure). */
    std::optional<int> applyDepth;

    /* Argument-argAncestry cell. Navigation children carry the same cell
       as their parent; the top-level (cb-arg) Local carries the
       apply's cell. Cell's own `parent` field gives ancestor chain. */
    std::shared_ptr<const ArgCell> argCell;

public:
    /* Constructor for derived children. Subject is built by the
       parent's maybeGetAttr / getListElem as `DerivedSubject{parent,
       ...}`. Inherits parent's shared walkFacts so the child's
       state hash evaluation rides on the same per-cb-apply history. */
    ReplayCallbackArg(
        trace::SelectorVariant producer_,
        std::shared_ptr<std::vector<ObservationSet>> walkFacts_,
        TracingDecisionGraph & dg,
        ref<SourceRoot> rootFSRoot,
        EvalState * state = nullptr)
        : producer(std::move(producer_))
        , localId(TracingDecisionGraph::computeSelectorHash(producer))
        , walkFacts(std::move(walkFacts_))
        , decisionGraph(dg), rootFSRoot(std::move(rootFSRoot)), state(state) {}

    /** Set the proxy's argCell. Returns *this for chaining. */
    ReplayCallbackArg & withArgCell(std::shared_ptr<const ArgCell> argScope_)
    {
        argCell = std::move(argScope_);
        return *this;
    }

    /** Set the cb-arg apply context (depth + argAncestry) so the lambda
        primop on this ReplayCallbackArg (or its derived children) can compose the
        nested apply-result's synthetic subject as
        `ApplyResultSubject{fn=this.subject, arg=Arg{depth+1}}`
        with the proper argAncestry. Derived children inherit the
        parent's applyContext via the same setter. */
    ReplayCallbackArg & withApplyContext(int depth_)
    {
        applyDepth = depth_;
        return *this;
    }

    /** Attach an obsSet response source. Each probe on this
        ReplayCallbackArg (or its derived children, if the
        shared_ptr is passed through) looks up its selectorHash in
        this map and decodes the CBOR payload as the response
        Result. Enables live outer validation from the recorded
        obsSet content. */
    ReplayCallbackArg & withObsSetResponses(
        std::shared_ptr<std::map<Hash, std::string>> map)
    {
        obsSetResponses = std::move(map);
        return *this;
    }

    std::shared_ptr<std::map<Hash, std::string>> getObsSetResponses() const
    {
        return obsSetResponses;
    }

    std::optional<int> getApplyDepth() const { return applyDepth; }

    std::shared_ptr<const ArgCell> getProxyArgCell() const override { return argCell; }

    /** Content-defined identity is the localId (= the cb-apply local
        arg's state hash hash recorded at write time). Lets evaluator.apply
        compute the apply Request hash when this ReplayCallbackArg is the arg. */
    std::optional<std::string> getSelectorHashHex() const override
    {
        return localId.to_string(HashFormat::Base16, false);
    }

    /** Symmetric to TracingCallbackArg: expose the ReplayCallbackArg's structural
        Subject so a subsequent apply on this ReplayCallbackArg (= the cb-arg
        ReplayCallbackArg used as `arg` in `<replay-local-lambda>`'s recursive
        apply) composes ApplyResultSubject with this ReplayCallbackArg's
        evolving Subject. */
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
    /** `toValueOrProxy` is the principled entry point for callers that
        want a Value-shaped representation of this recorded local —
        e.g., `Interpreter::apply` constructing an `mkApp` thunk where
        this Object is the fn. The current implementation delegates to
        `defeatCache` for behaviour parity; the structural-fix follow-up
        (= task #5) reimplements it to produce a primop with the correct
        `ApplyResultSubject` encoding so the synthetic ReplayCallbackArg's reads
        match what the recorder wrote (= avoids the cb-higher-order
        recursion). */
    RootValue toValueOrProxy(EvalState & state, std::shared_ptr<OuterResolver> resolver) override;
    std::optional<FunctionInfo> getFunctionInfo() override;
    /** Recorded frozen callback args can't be applied without
        reconstructing the function body from value-structure atoms
        or comparing the live arg's content to the recorded arg's
        content. Until either lands, an apply on a ReplayCallbackArg
        is undecidable — we don't know whether the recorded result
        still applies for the current live arg. Throw a recognizable
        signal that callers can interpret as "walker miss, fall
        through to live re-eval."

        Today no caller routes here: the apply chain still goes
        through `Object::defeatCache` + value-level `callFunction`,
        not `Object::queryApply`. The override exists so that when
        callers are restructured to call `queryApply` uniformly, this
        is the entry that fires. */
    std::shared_ptr<Object> queryApply(std::shared_ptr<Object> argObj) override;
};

} // namespace nix
