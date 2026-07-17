#pragma once
/**
 * @file
 * ReplayCallbackArg: serves recorded responses for a local arg
 * during replay-time apply invocation.
 *
 * The OUTER's covariant callback (e.g. `f x` where `f` is an outer
 * lambda and `x` is an inner-supplied arg) lets the outer access
 * inner-side data. On the recording side the inner wraps the arg in
 * TracingCallbackArg so the outer's accesses land in the inner's
 * factSet as Facts. On replay the inner isn't running, so its arg
 * isn't reconstructable as a live Object — but its CONTENT was
 * persisted in InnerValueResponse. ReplayCallbackArg reads that
 * content back so the outer can invoke its callback against a
 * deterministic frozen image of the recorded arg.
 *
 * This is what makes covariant-callback caching actually validate:
 * with this object in place, `resolveStateHash` for an apply tag
 * can invoke `fn->queryApply(replayArg)` live, and downstream
 * apply-result Facts get dispatched against the live outer's
 * response. If the outer changed (different lambda body) the
 * response differs from the recorded → history falls through. Without
 * it the dispatcher would just serve the recorded response, hiding
 * outer-side changes from the validation chain.
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
       `stateHashAfter(Arg{D}, callArgAncestry, {})` directly.

       For children minted by maybeGetAttr/getListElem the subject is
       `DerivedSubject{parent.subject, ...}` — `stateHashAt`
       recursively re-evaluates the parent's state hash at the child's
       current edge index, so children don't need to snapshot parent
       state at creation. */
    Subject subject;
    Hash argAncestry;
    /* Initial state hash (= stateHashAt(subject, argAncestry, {}, 0)) — kept for
       legacy id-string consumers (e.g. defeatCache's recursive
       apply construction). */
    OuterId localId;
    /* Shared history across all proxies in one cb apply. Each validated
       probe appends a Fact (one fact per edge, matching the writer's
       multi-edge AmbientAsks structure). `stateHashAt` reads this
       to compute each proxy's evolved state hash.

       Backed as a shared single-fact-edge sequence: each entry is
       wrapped in a single-fact ObservationSet so the history's edge indices match
       the recorder's flush history. */
    std::shared_ptr<std::vector<ObservationSet>> walkFacts;
    /* Shared chain cursor across all proxies in one cb apply. Each
       validated probe advances `*chainCursor` to the matched edge's
       toFactSet. */
    std::shared_ptr<Hash> chainCursor;
    /* Walker's outer env fact-set state at the moment this ReplayCallbackArg
       was constructed (= walker's cur before entering this cb-apply
       boundary). Used as the InnerValueResponse lookup context so two
       cb-applies of the same abstract fn+arg within one cached body
       resolve to their respective recorded responses (cb-repeated-
       cb-apply-diff-args's fix). Cold's insertInnerValueResponse writes
       with writer.envFactSetHash at the matching moment; walker at
       ReplayCallbackArg construction time receives its own outer
       cur which — by lockstep growth of walker.envWalk with
       writer.envWalk via the subject-evolution fast-path — equals cold's writer cur. */
    Hash outerContext;
    TracingDecisionGraph & decisionGraph;
    ref<SourceRoot> rootFSRoot;
    /* EvalState used for primop construction in `defeatCache`. The
       outer's EvalState (= where the primop will be applied) is the
       right one in principle; in practice any live EvalState works
       because primop construction allocates a Value off the shared
       gc heap and the lambda capture holds onto the construction
       arguments by value. Threaded from `materialiseLocalStandin`. */
    EvalState * state;
    /* When true, each Object-method call validates the probe against
       the recorded AmbientAsks edges from ∅ — the ambient layer per-probe
       check (= "did the outer probe the local in a recorded way?").
       Set on the cb-apply local that crosses the boundary. The
       primop's recursive synthetic (= apply result reconstruction)
       has this false because its facts live in env layer, not in
       AmbientAsks. */
    bool validateAgainstAmbientAsks = false;

    /* Optional obsSet response source (task #103). When set, method
       responses are looked up in this map by queryHash instead of
       (or before) InnerValueResponse. Populated by the walker's
       callbackApply dispatch from the CallbackApply's referenced
       observation set — each entry is (queryHash → CBOR response
       payload). Independent of the boundary/contextHash mechanism. */
    std::shared_ptr<std::map<Hash, std::string>> obsSetResponses;

    /* Memoized WHNF response. The recorder logs ONE QueryGetWHNF ambient
       observation per value force; the walker must advance
       `chainCursor` once to stay in lockstep but reuse the cached
       response on any subsequent call. Without this, when
       `dispatchAmbientQuery::navigatePath` invokes `queryApply`
       multiple times against the same ReplayCallbackArg (= once per fact
       dispatched on the apply result), each Apply Value's force
       re-fires the ReplayCallbackArg's surface probes and pushes a fresh fact
       past where the recorder stopped recording — the next lookup at
       `walkFacts.size() > recorded_size` then misses
       InnerValueResponse and the walker fails. */
    std::optional<trace::ResultWHNF> cachedWHNF;
    /** Read recorded WHNF for this proxy (= one QueryGetWHNF read +
        chain advance). Memoized; subsequent calls return the same
        result without re-probing. Returns the cached WHNF as a const
        reference so callers can decode the payload by alternative. */
    const trace::ResultWHNF & whnf();

    /* cb-arg apply context, sourced from the writer's localArg
       sidecar. `applyDepth` = `localCell->depth` at the recorder's
       OuterResolver::cb-apply. `applyArgAncestry` = the resolver's
       callArgAncestry. Used by the lambda primop to compose nested
       apply-result subjects matching the recorder's encoding (=
       `ApplyResultSubject{fn=this.subject, arg=Arg{depth+1}}`
       at `applyArgAncestry`). Inherited unchanged through derived
       children (= the nested apply's positional depth is one
       deeper than the cb-arg's, regardless of attr/list navigation
       within the cb-arg's structure). */
    std::optional<int> applyDepth;
    std::optional<Hash> applyArgAncestry;

    /* Argument-argAncestry cell. Navigation children carry the same cell
       as their parent; the top-level (cb-arg) Local carries the
       apply's cell. Cell's own `parent` field gives ancestor chain. */
    std::shared_ptr<const ArgCell> argCell;

public:
    /* Constructor for derived children. Subject is built by the
       parent's maybeGetAttr / getListElem as `DerivedSubject{parent,
       ...}`. Inherits parent's shared history/cursor so the child's
       state hash evaluation rides on the same per-cb-apply chain. */
    ReplayCallbackArg(
        Subject subject_,
        Hash scope_,
        std::shared_ptr<std::vector<ObservationSet>> walkFacts_,
        std::shared_ptr<Hash> chainCursor_,
        Hash outerContext_,
        TracingDecisionGraph & dg,
        ref<SourceRoot> rootFSRoot,
        EvalState * state = nullptr)
        : subject(std::move(subject_))
        , argAncestry(std::move(scope_))
        , localId(stateHashAtSubject(subject, argAncestry, *walkFacts_, 0))
        , walkFacts(std::move(walkFacts_))
        , chainCursor(std::move(chainCursor_))
        , outerContext(std::move(outerContext_))
        , decisionGraph(dg), rootFSRoot(std::move(rootFSRoot)), state(state) {}

    /** Set the proxy's argCell. Returns *this for chaining. */
    ReplayCallbackArg & withArgCell(std::shared_ptr<const ArgCell> argScope_)
    {
        argCell = std::move(argScope_);
        return *this;
    }

    /** Opt into ambient layer per-probe validation. Set on the cb-apply
        local (= the ReplayCallbackArg materialised at the cb cb-apply,
        whose surface probes were recorded in AmbientAsks). */
    ReplayCallbackArg & withAmbientAsksValidation()
    {
        validateAgainstAmbientAsks = true;
        return *this;
    }

    /** Set the ambient chain's starting cursor. Each cb-apply's ambient
        chain is rooted at its applyReqHash (= the natural hash of
        the cb-apply payload) — different cb-applies' chains live in
        disjoint subtrees of AmbientAsks. The walker passes the
        apply_qH it's resolving here so the ReplayCallbackArg's per-probe history
        starts at the right root.

        Side-effect: if the chain at this root is empty (= no
        AmbientAsks rows), demote `validateAgainstAmbientAsks` to
        false. This handles the late-d2-obs option (b) case where
        the recorder's first finalize pass for this boundary saw
        probes=0 (= the inner body didn't force the local) and the
        actual probes only arrived later — by then those probes
        were inserted into `InnerValueResponse` but NOT into
        `AmbientAsks` (= extending the chain would corrupt
        dispatchApplyLive's AmbientResult and break the env
        per-Q cur propagation). The cb-apply local can still
        readResponse the late probes; we just can't per-probe
        validate them against AmbientAsks because the chain row
        wasn't recorded. */
    ReplayCallbackArg & withChainStart(Hash root);

    /** Set the cb-arg apply context (depth + argAncestry) so the lambda
        primop on this ReplayCallbackArg (or its derived children) can compose the
        nested apply-result's synthetic subject as
        `ApplyResultSubject{fn=this.subject, arg=Arg{depth+1}}`
        with the proper argAncestry. Sourced from the writer's localArg
        sidecar at the ReplayCallbackArg's localId. Derived children inherit
        the parent's applyContext via the same setter. */
    ReplayCallbackArg & withApplyContext(int depth_, Hash scope_)
    {
        applyDepth = depth_;
        applyArgAncestry = std::move(scope_);
        return *this;
    }

    /** Attach an obsSet response source (task #103). Each probe on
        this ReplayCallbackArg (or its derived children, if the
        shared_ptr is passed through) will look up its queryHash in
        this map first, decoding the CBOR payload as the response
        Result. Falls back to InnerValueResponse if the queryHash
        isn't in the map. Enables the CallbackApply walker's live
        outer validation without the boundary/contextHash
        machinery. */
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
    std::optional<Hash> getApplyScope() const { return applyArgAncestry; }

    /** Whether per-probe validation is enabled for this proxy. */
    bool hasAmbientAsksValidation() const { return validateAgainstAmbientAsks; }

    /** Current value of the ambient chain cursor. Read after the outer
        has finished probing the ReplayCallbackArg (= after fn->queryApply
        returns) to obtain the chain's terminal — that's the
        AmbientResult fed back as the cb-apply Request's env
        respHash. */
    Hash getChainCursor() const { return *chainCursor; }

    std::shared_ptr<const ArgCell> getProxyArgCell() const override { return argCell; }

    /** Content-defined identity is the localId (= the cb-apply local
        arg's state hash hash recorded at write time). Lets evaluator.apply
        compute the apply Request hash when this ReplayCallbackArg is the arg. */
    std::optional<std::string> getStateHashHex() const override
    {
        return localId.to_string(HashFormat::Base16, false);
    }

    /** Symmetric to TracingCallbackArg: expose the ReplayCallbackArg's structural
        Subject so a subsequent apply on this ReplayCallbackArg (= the cb-arg
        ReplayCallbackArg used as `arg` in `<replay-local-lambda>`'s recursive
        apply) composes ApplyResultSubject with this ReplayCallbackArg's
        evolving Subject. */
    const Subject * getSubject() const override { return &subject; }

    Hash getArgAncestry() const override { return argAncestry; }

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
    /** Recorded LocalObjects (frozen images) can't be applied without
        either reconstructing the function body from value-structure
        atoms (task #75) or comparing the live arg's content to the
        recorded arg's content (task #74's ambient layer walker). Until one
        of those lands, an apply on a ReplayCallbackArg is undecidable
        — we don't know whether the recorded result still applies for
        the current live arg. Throw a recognizable signal that
        callers can interpret as "walker miss, fall through to live
        re-eval."

        Today no caller routes here: the apply chain still goes
        through `Object::defeatCache` + value-level `callFunction`,
        not `Object::queryApply`. The override exists so that when
        callers are restructured to call `queryApply` uniformly, this
        is the entry that fires. */
    std::shared_ptr<Object> queryApply(std::shared_ptr<Object> argObj) override;
};

} // namespace nix
