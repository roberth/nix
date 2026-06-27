#pragma once
/**
 * @file
 * ReplayLocalObject: serves recorded responses for a local arg
 * during replay-time apply invocation.
 *
 * The OUTER's covariant callback (e.g. `f x` where `f` is an outer
 * lambda and `x` is an inner-supplied arg) lets the outer access
 * inner-side data. On the recording side the inner wraps the arg in
 * TracingLocalObject so the outer's accesses land in the inner's
 * factSet as Facts. On replay the inner isn't running, so its arg
 * isn't reconstructable as a live Object — but its CONTENT was
 * persisted in LocalResponseMap. ReplayLocalObject reads that
 * content back so the outer can invoke its callback against a
 * deterministic frozen image of the recorded arg.
 *
 * This is what makes covariant-callback caching actually validate:
 * with this object in place, `resolveCdiId` for an apply tag
 * can invoke `fn->queryApply(replayArg)` live, and downstream
 * apply-result Facts get dispatched against the live outer's
 * response. If the outer changed (different lambda body) the
 * response differs from the recorded → walk falls through. Without
 * it the dispatcher would just serve the recorded response, hiding
 * outer-side changes from the validation chain.
 */

#include "nix/expr/arg-scope.hh"
#include "nix/expr/content-identity-via-asks.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/trace-ids.hh"
#include "nix/util/hash.hh"

namespace nix {

class TracingDecisionGraph;

class ReplayLocalObject : public Object
{
    /* Full structural identity. Combined with `scope` and the shared
       `walkFacts`, `cidasks::contentIdAt` computes this proxy's cdi
       at any walk position. The recorder's cidasks substitution at
       flush uses the same evaluation, so walker and recorder agree
       on per-probe `from` fields without snapshot/lazy hacks — even
       when a child's structural component depends on a parent's
       evolving cdi.

       For root (cb-apply) locals the subject is constructed by the
       walker as `OpaqueContentSubject{localId}` with scope = 0, so
       the structural part at edge 0 equals localId itself (= what
       the recorder computed as `contentIdAfter(PositionalSeed{D},
       callScope, {})`).

       For children minted by maybeGetAttr/getListElem the subject is
       `DerivedSubject{parent.subject, ...}` — `contentIdAt`
       recursively re-evaluates the parent's cdi at the child's
       current edge index, so children don't need to snapshot parent
       state at creation. */
    cidasks::Subject subject;
    Hash scope;
    /* Initial cdi (= contentIdAt(subject, scope, {}, 0)) — kept for
       legacy id-string consumers (e.g. defeatCache's recursive
       apply construction). */
    AmbientId localId;
    /* Shared walk across all proxies in one cb apply. Each validated
       probe appends a Fact (one fact per edge, matching the writer's
       multi-edge AmbientAsks structure). `contentIdAt` reads this
       to compute each proxy's evolved cdi.

       Backed as a shared single-fact-edge sequence: each entry is
       wrapped in a single-fact Edge so the walk's edge indices match
       the recorder's flush walk. */
    std::shared_ptr<std::vector<cidasks::Edge>> walkFacts;
    /* Shared chain cursor across all proxies in one cb apply. Each
       validated probe advances `*chainCursor` to the matched edge's
       toFactSet. */
    std::shared_ptr<Hash> chainCursor;
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
       the recorded AmbientAsks edges from ∅ — the depth-2 per-probe
       check (= "did the outer probe the local in a recorded way?").
       Set on the cb-apply local that crosses the boundary. The
       primop's recursive synthetic (= apply result reconstruction)
       has this false because its facts live in depth-1, not in
       AmbientAsks. */
    bool validateAgainstAmbientAsks = false;
    /* Optimistic type cache. The recorder's TracingLocalObject *does*
       log a getType fact on every child the first time the outer
       probes it (= via the new TLO wrapping the child), so the
       walker must probe at least once on the child too — knownType
       only short-circuits SECOND and later calls. Set when the
       parent's maybeGetAttr / getListElem response embeds a type
       string. Cleared after the first probe so subsequent calls
       return it without re-probing. */
    std::optional<ObjectType> knownType;
    /* True once a getType probe has been dispatched against this
       proxy. Until then `getType` always probes (regardless of
       knownType) so the walker's chain advances in lockstep with
       the recorder's TLO. */
    bool getTypeProbed = false;

    /* Argument-scope cell. Navigation children carry the same cell
       as their parent; the top-level (cb-arg) Local carries the
       apply's cell. Cell's own `parent` field gives ancestor chain. */
    std::shared_ptr<const ArgScopeCell> argScope;

public:
    /* Constructor for a root cb-apply local. Wraps the recorded
       localId as an `OpaqueContentSubject` with scope=0 so the
       walker's structuralAt at edge 0 reproduces the recorder's
       `contentIdAfter(PositionalSeed{D}, callScope, {})` (= localId)
       without needing to know the original depth/scope. */
    ReplayLocalObject(AmbientId localId, TracingDecisionGraph & dg, ref<SourceRoot> rootFSRoot, EvalState * state = nullptr)
        : subject{cidasks::OpaqueContentSubject{localId}}
        , scope(HashAlgorithm::SHA256)
        , localId(localId)
        , walkFacts(std::make_shared<std::vector<cidasks::Edge>>())
        , chainCursor(std::make_shared<Hash>(HashAlgorithm::SHA256))
        , decisionGraph(dg), rootFSRoot(std::move(rootFSRoot)), state(state) {}

    ReplayLocalObject(AmbientId localId, TracingDecisionGraph & dg, ref<SourceRoot> rootFSRoot, ObjectType type, EvalState * state = nullptr)
        : subject{cidasks::OpaqueContentSubject{localId}}
        , scope(HashAlgorithm::SHA256)
        , localId(localId)
        , walkFacts(std::make_shared<std::vector<cidasks::Edge>>())
        , chainCursor(std::make_shared<Hash>(HashAlgorithm::SHA256))
        , decisionGraph(dg), rootFSRoot(std::move(rootFSRoot)), state(state), knownType(type) {}

    /* Constructor for derived children. Subject is built by the
       parent's maybeGetAttr / getListElem as `DerivedSubject{parent,
       ...}`. Inherits parent's shared walk/cursor so the child's
       cdi evaluation rides on the same per-cb-apply chain. */
    ReplayLocalObject(
        cidasks::Subject subject_,
        Hash scope_,
        std::shared_ptr<std::vector<cidasks::Edge>> walkFacts_,
        std::shared_ptr<Hash> chainCursor_,
        TracingDecisionGraph & dg,
        ref<SourceRoot> rootFSRoot,
        ObjectType type,
        EvalState * state = nullptr)
        : subject(std::move(subject_))
        , scope(std::move(scope_))
        , localId(cidasks::structuralAddress(subject, scope, *walkFacts_, 0))
        , walkFacts(std::move(walkFacts_))
        , chainCursor(std::move(chainCursor_))
        , decisionGraph(dg), rootFSRoot(std::move(rootFSRoot)), state(state), knownType(type) {}

    /** Set the proxy's argScope. Returns *this for chaining. */
    ReplayLocalObject & withScope(std::shared_ptr<const ArgScopeCell> argScope_)
    {
        argScope = std::move(argScope_);
        return *this;
    }

    /** Opt into depth-2 per-probe validation. Set on the cb-apply
        local (= the standin materialised at the cb apply boundary,
        whose surface probes were recorded in AmbientAsks). */
    ReplayLocalObject & withAmbientAsksValidation()
    {
        validateAgainstAmbientAsks = true;
        return *this;
    }

    /** Set the d=2 chain's starting cursor. Each cb-apply's d=2
        chain is rooted at its applyReqHash (= the natural hash of
        the cb-apply payload) — different cb-applies' chains live in
        disjoint subtrees of AmbientAsks. The walker passes the
        apply_qH it's resolving here so the standin's per-probe walk
        starts at the right root. */
    ReplayLocalObject & withChainStart(Hash root)
    {
        *chainCursor = std::move(root);
        return *this;
    }

    /** Whether per-probe validation is enabled for this proxy. */
    bool hasAmbientAsksValidation() const { return validateAgainstAmbientAsks; }

    /** Current value of the d=2 chain cursor. Read after the outer
        has finished probing the standin (= after fn->queryApply
        returns) to obtain the chain's terminal — that's the
        AmbientResult fed back as the cb-apply Request's d=1
        respHash. */
    Hash getChainCursor() const { return *chainCursor; }

    std::shared_ptr<const ArgScopeCell> getProxyArgScope() const override { return argScope; }

    /** Content-defined identity is the localId (= the cb-apply local
        arg's CDI hash recorded at write time). Lets evaluator.apply
        compute the apply Request hash when this standin is the arg. */
    std::optional<std::string> getCdiHex() const override
    {
        return localId.to_string(HashFormat::Base16, false);
    }

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
        `ApplyResultSubject` encoding so the synthetic standin's reads
        match what the recorder wrote (= avoids the cb-higher-order
        recursion). */
    RootValue toValueOrProxy(EvalState & state, std::shared_ptr<AmbientResolver> resolver) override;
    std::optional<FunctionInfo> getFunctionInfo() override;
    /** Recorded LocalObjects (frozen images) can't be applied without
        either reconstructing the function body from value-structure
        atoms (task #75) or comparing the live arg's content to the
        recorded arg's content (task #74's depth-2 walker). Until one
        of those lands, an apply on a ReplayLocalObject is undecidable
        — we don't know whether the recorded result still applies for
        the current live arg. Throw a recognizable signal that
        callers can interpret as "walker miss, fall through to live
        re-eval."

        Today no caller routes here: the apply chain still goes
        through `Object::defeatCache` + value-level `callFunction`,
        not `Object::queryApply`. The override exists so that when
        callers are restructured to call `queryApply` uniformly, this
        is the entry that fires. The provenance tag in
        `AmbientRegistry` keeps the existing-callers path correct
        until the restructure lands. */
    std::shared_ptr<Object> queryApply(std::shared_ptr<Object> argObj) override;
};

} // namespace nix
