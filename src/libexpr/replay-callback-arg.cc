#include "nix/expr/replay-callback-arg.hh"
#include "nix/expr/subject-id.hh"
#include "nix/expr/expr-from-object.hh"
#include "nix/expr/interpreter-object.hh"
#include "nix/expr/object-type.hh"
#include "nix/expr/primops.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/expr/trace-types.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/error.hh"

namespace nix {

ReplayCallbackArg & ReplayCallbackArg::withChainStart(Hash root)
{
    *chainCursor = std::move(root);
    if (validateAgainstAmbientAsks) {
        auto outgoing = decisionGraph.getAmbientAsks(*chainCursor);
        if (outgoing.empty())
            validateAgainstAmbientAsks = false;
    }
    return *this;
}

/* Populate `query`'s per-arg fields (from, path, fromStateHashes) so its
   reqHash matches what the writer flushed for the corresponding
   recorder probe. Multi-root applies fill fromStateHashes[] with multiple
   leaf-root state hashes; the canonical `from` field carries fromStateHashes[0].
   Returns the first-root state hash for callers (= used to log/diagnose
   and for the AmbientAsks chain advance). */
template <typename Q>
static Hash stampPerArgFields(
    Q & query,
    const Subject & subject,
    const Hash & argAncestry,
    const std::vector<ObservationSet> & walkFacts,
    size_t step)
{
    auto par = pathAndRootsFromSubject(subject);
    std::vector<trace::QueryLeaf> fromStateHashes;
    fromStateHashes.reserve(par.roots.size());
    Hash fromStateHash(HashAlgorithm::SHA256);
    for (size_t i = 0; i < par.roots.size(); ++i) {
        auto cid = stateHashAt(par.roots[i], argAncestry, walkFacts, step);
        if (i == 0)
            fromStateHash = cid;
        fromStateHashes.emplace_back(cid.to_string(HashFormat::Base16, false));
    }
    query.from = fromStateHashes.empty()
        ? trace::QueryLeaf{std::string{}}
        : fromStateHashes[0];
    query.path = std::move(par.path);
    query.fromStateHashes = std::move(fromStateHashes);
    return fromStateHash;
}

/* Look up the recorded payload for `query` in InnerValueResponse.
   The map is keyed by requestHash and that's sound at ambient layer
   because reqHash is `SHA-256(query{from = subject-id-evolved state hash})`
   — a pure function of (subject, argAncestry, prior chain facts). Two
   recordings reaching the same reqHash necessarily observed the
   same history; a deterministic env then produces the same
   response, so first-writer-wins in the map can't return the
   wrong payload. */
template<typename Q>
static nlohmann::json readResponse(TracingDecisionGraph & dg, const Q & query, const Hash & outerContext)
{
    auto reqHash = TracingDecisionGraph::computeQueryHash(query);
    tracingCacheLog(
        "rlo: read %s from=%s reqHash=%s outerCtx=%s",
        Q::tag, query.from.isStateHash() ? query.from.stateHash().substr(0, 12) : "<?>",
        reqHash.to_string(HashFormat::Base16, false).substr(0, 12),
        outerContext.to_string(HashFormat::Base16, false).substr(0, 12));
    auto payload = dg.getInnerValueResponsePayload(reqHash, outerContext);
    if (!payload)
        throw Error("ReplayCallbackArg: no recorded response for %s on local %s",
            Q::tag, query.from.isStateHash() ? query.from.stateHash() : "<ambient>");
    return cborStringToJson(*payload);
}

/* Multi-edge AmbientAsks walker: dispatch and validate one probe at
   a time. Per the design's "Replay (ambient layer)" section, each probe
   (a) composes with `from = hex(stateHashAt(subject, argAncestry,
   walkFacts, walkFacts.size()))` so its reqHash matches what the
   recorder wrote at this point in the chain, (b) is looked up as a
   singleton-requestSet edge from `*chainCursor → toFactSet`, and
   (c) on a match advances the shared chain cursor and appends the
   fact to the shared history so subsequent probes compose against the
   correctly evolved state hashes. On mismatch we throw a divergence signal
   which the surrounding walker layer turns into a miss → env layer
   fallback handles re-eval. */
/* Append the just-probed fact to `walkFacts` so the next probe's
   `stampPerArgFields` sees its own-loop contribution. Whether or not
   validation against AmbientAsks runs, the per-arg state hash evolution
   relies on the history extending in lockstep with the recorder — so
   this needs to fire on every probe, not just validated ones. */
template<typename Q>
static void appendFactToWalk(
    const Q & query, const Hash & fromStateHash, const nlohmann::json & responseJson,
    std::vector<ObservationSet> & walkFacts)
{
    auto reqHash = TracingDecisionGraph::computeQueryHash(query);
    auto responsePayload = jsonToCborString(responseJson);
    auto responseHash = TracingDecisionGraph::computeResponseHash(responsePayload);
    auto elementHash = TracingDecisionGraph::xorFactIntoHash(
        Hash(HashAlgorithm::SHA256), reqHash, responseHash);
    ObservationSet edge;
    edge.observations.push_back({fromStateHash, elementHash});
    walkFacts.push_back(std::move(edge));
}

template<typename Q>
static void advanceChainAndAppendFact(
    TracingDecisionGraph & dg, const Q & query, const Hash & fromStateHash,
    const nlohmann::json & responseJson,
    std::vector<ObservationSet> & walkFacts, Hash & chainCursor)
{
    auto reqHash = TracingDecisionGraph::computeQueryHash(query);
    tracingCacheLog(
        "history: probe %s from=%s reqHash=%s cursor=%s walkSize=%zu",
        Q::tag, fromStateHash.to_string(HashFormat::Base16, false).substr(0, 12),
        reqHash.to_string(HashFormat::Base16, false).substr(0, 12),
        chainCursor.to_string(HashFormat::Base16, false).substr(0, 12),
        walkFacts.size());
    auto edges = dg.getAmbientAsks(chainCursor);
    for (auto & [requestSetHash, toFactSet] : edges) {
        auto requestSet = dg.getRequestSet(requestSetHash);
        if (!requestSet)
            continue;
        if (std::find(requestSet->begin(), requestSet->end(), reqHash) == requestSet->end())
            continue;
        appendFactToWalk(query, fromStateHash, responseJson, walkFacts);
        /* Advance chainCursor by XOR-folding the live
           (reqHash, responseHash) rather than reading cold's
           `toFactSet`. Cold's AmbientAsks schema `(from, rs) → to`
           with INSERT OR IGNORE loses chain B when two apply
           invocations share a (from, rs) key: XOR-fold lets the
           walker compute each invocation's unique chainCursor from
           the same `from` position without schema widening. Live
           and cold agree when cold's stored
           `toFactSet == from XOR H(reqHash, respHash)` (writer's
           ambient stampAndEmit), so the substitution is exact. */
        auto responsePayload = jsonToCborString(responseJson);
        auto responseHash = TracingDecisionGraph::computeResponseHash(responsePayload);
        chainCursor = TracingDecisionGraph::xorFactIntoHash(
            chainCursor, reqHash, responseHash);
        (void) toFactSet;
        return;
    }
    tracingCacheLog(
        "ambient layer divergence: probe %s reqHash=%s no AmbientAsks edge from %s",
        Q::tag, reqHash.to_string(HashFormat::Base16, false).substr(0, 12),
        chainCursor.to_string(HashFormat::Base16, false).substr(0, 12));
    throw Error(
        "ambient layer divergence: probe %s on local has no AmbientAsks edge from current factSet",
        Q::tag);
}

std::shared_ptr<Object> ReplayCallbackArg::maybeGetAttr(const std::string & name)
{
    trace::QueryGetAttr query{name, std::string{}};
    auto fromStateHash = stampPerArgFields(query, subject, argAncestry, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query, outerContext);
    if (validateAgainstAmbientAsks)
        advanceChainAndAppendFact(decisionGraph, query, fromStateHash, rJson, *walkFacts, *chainCursor);
    else
        appendFactToWalk(query, fromStateHash, rJson, *walkFacts);
    trace::ResultMaybeType r = rJson;
    if (!r.type)
        return nullptr;
    /* Child Subject is DerivedSubject of THIS subject — `stateHashAt`
       on the child will recompute parent's state hash at the child's
       current edge index, so any further parent observations are
       reflected automatically. Pass shared history/cursor. */
    Subject childSubject{DerivedSubject{
        .parent = std::make_shared<const Subject>(subject),
        .kind = DerivedSubject::Kind::GetAttr,
        .name = name,
    }};
    auto child = std::make_shared<ReplayCallbackArg>(
        std::move(childSubject), argAncestry, walkFacts, chainCursor,
        outerContext, decisionGraph, rootFSRoot, state);
    /* Children inherit per-probe validation if the parent has it —
       they're observed within the same cb apply's recorded chain. */
    if (validateAgainstAmbientAsks)
        child->withAmbientAsksValidation();
    /* Navigation child inherits parent's argCell cell directly. */
    child->withArgCell(argCell);
    /* Inherit cb-arg apply context — derived navigation stays within
       the same cb-arg's depth/argAncestry (= the nested apply's positional
       depth is one deeper than the cb-arg's, regardless of how many
       getAttr/getListElem steps deep the apply happens). */
    if (applyDepth && applyArgAncestry)
        child->withApplyContext(*applyDepth, *applyArgAncestry);
    return child;
}

const trace::ResultWHNF & ReplayCallbackArg::whnf()
{
    if (cachedWHNF)
        return *cachedWHNF;
    trace::QueryGetWHNF query{std::string{}};
    auto fromStateHash = stampPerArgFields(query, subject, argAncestry, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query, outerContext);
    if (validateAgainstAmbientAsks)
        advanceChainAndAppendFact(decisionGraph, query, fromStateHash, rJson, *walkFacts, *chainCursor);
    else
        appendFactToWalk(query, fromStateHash, rJson, *walkFacts);
    cachedWHNF = rJson.get<trace::ResultWHNF>();
    return *cachedWHNF;
}

std::vector<std::string> ReplayCallbackArg::getAttrNames()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFAttrs>(&w.payload);
    if (!p)
        throw Error("rlo getAttrNames: WHNF payload not attrs (type %s)", w.type);
    return p->names;
}

std::string ReplayCallbackArg::getStringIgnoreContext()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFString>(&w.payload);
    if (!p)
        throw Error("rlo getStringIgnoreContext: WHNF payload not string (type %s)", w.type);
    return p->value;
}

std::string ReplayCallbackArg::getStringWithoutContext()
{
    return getStringIgnoreContext();
}

std::pair<std::string, NixStringContext> ReplayCallbackArg::getStringWithContext()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFString>(&w.payload);
    if (!p)
        throw Error("rlo getStringWithContext: WHNF payload not string (type %s)", w.type);
    NixStringContext ctx;
    for (auto & s : p->context)
        ctx.insert(NixStringContextElem::parse(s));
    return {p->value, std::move(ctx)};
}

RootedPath ReplayCallbackArg::getPath()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFPath>(&w.payload);
    if (!p)
        throw Error("rlo getPath: WHNF payload not path (type %s)", w.type);
    return RootedPath{rootFSRoot, CanonPath{p->path}};
}

bool ReplayCallbackArg::getBool(std::string_view)
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFBool>(&w.payload);
    if (!p)
        throw Error("rlo getBool: WHNF payload not bool (type %s)", w.type);
    return p->value;
}

NixInt ReplayCallbackArg::getInt(std::string_view)
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFInt>(&w.payload);
    if (!p)
        throw Error("rlo getInt: WHNF payload not int (type %s)", w.type);
    return NixInt{p->value};
}

NixFloat ReplayCallbackArg::getFloat(std::string_view)
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFFloat>(&w.payload);
    if (!p)
        throw Error("rlo getFloat: WHNF payload not float (type %s)", w.type);
    return p->value;
}

size_t ReplayCallbackArg::getListSize()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFList>(&w.payload);
    if (!p)
        throw Error("rlo getListSize: WHNF payload not list (type %s)", w.type);
    return p->size;
}

std::shared_ptr<Object> ReplayCallbackArg::getListElem(size_t index)
{
    trace::QueryGetListElem query{std::string{}, index};
    auto fromStateHash = stampPerArgFields(query, subject, argAncestry, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query, outerContext);
    if (validateAgainstAmbientAsks)
        advanceChainAndAppendFact(decisionGraph, query, fromStateHash, rJson, *walkFacts, *chainCursor);
    else
        appendFactToWalk(query, fromStateHash, rJson, *walkFacts);
    Subject childSubject{DerivedSubject{
        .parent = std::make_shared<const Subject>(subject),
        .kind = DerivedSubject::Kind::GetListElem,
        .index = index,
    }};
    auto child = std::make_shared<ReplayCallbackArg>(
        std::move(childSubject), argAncestry, walkFacts, chainCursor,
        outerContext, decisionGraph, rootFSRoot, state);
    if (validateAgainstAmbientAsks)
        child->withAmbientAsksValidation();
    child->withArgCell(argCell);
    if (applyDepth && applyArgAncestry)
        child->withApplyContext(*applyDepth, *applyArgAncestry);
    return child;
}

ObjectType ReplayCallbackArg::getType()
{
    return stringToObjectType(whnf().type);
}

ObjectType ReplayCallbackArg::getTypeLazy()
{
    return getType();
}

RootValue ReplayCallbackArg::defeatCache()
{
    /* `defeatCache` means "bypass the cache and force the original
       expression to get the actual Value" — but a ReplayCallbackArg
       IS the cache for a frozen local arg whose original Value isn't
       live during replay. There's nothing to bypass to. Callers that
       want a Value-shaped handle for `mkApp` should use
       `toValueOrProxy` instead. */
    throw Error(
        "ReplayCallbackArg::defeatCache: cannot bypass the cache on a "
        "frozen local — use toValueOrProxy to obtain a primop replay");
}

RootValue ReplayCallbackArg::toValueOrProxy(EvalState & evalState, std::shared_ptr<OuterResolver> resolver)
{
    /* Per via-Asks Replay (ambient layer): the walker reconstructs the
       LocalObject as a live Nix Value tree, lazily produced from
       CAS atoms. The shape depends on the recorded type:

       - `nFunction` (= an inner-supplied lambda LocalObject):
         reconstruct as a primop whose impl consults `AmbientAsks`
         at apply-time for a recorded edge matching the live arg's
         evolved state hash, and reproduces the recorded apply
         result. Per the via-Asks doc's "Lambda LocalObjects don't
         need their body stored" — the application behavior lives
         in the recorded ambient chain, not in a stored body.

       - Other types (attrset / list / scalars): return a thunk
         wrapping `ExprFromObject(self)` so the consumer materialises
         the value tree lazily via Object methods, each call reading
         the corresponding recorded response from CAS. */
    auto type = getType();
    if (type != nFunction) {
        auto * thunk = evalState.allocValue();
        auto * expr = new ExprFromObject(shared_from_this(), nullptr, std::move(resolver));
        evalState.mkThunk_(*thunk, expr);
        return allocRootValue(thunk);
    }

    auto * dg = &decisionGraph;
    auto rootFSRootSaved = rootFSRoot;
    auto subjectSaved = subject;
    auto walkFactsSaved = walkFacts;
    auto chainCursorSaved = chainCursor;
    auto applyDepthSaved = applyDepth;
    auto applyArgAncestrySaved = applyArgAncestry;
    auto outerContextSaved = outerContext;
    /* Capture the resolver so the primop can register the live arg
       it receives (args[0]) as an outer-direction proxy. The OUTER
       walker dispatches env facts whose `from` references the cb-arg
       arg's initial state hash (= what the inner-side queryFn closure
       captured at cold); without this registration the walker's
       resolveStateHash falls through "outer-arg by elimination" and the
       fact's dispatch fails. May be nullptr in unit-test paths that
       construct a ReplayCallbackArg without a resolver — registration is
       skipped then. */
    auto resolverSaved = resolver;
    /* Capture the ReplayCallbackArg's chainCursor at primop-construction time
       (= AFTER ExprFromObject(replayObj).eval's `obj->getType()` call
       fires `ReplayCallbackArg.getType` and advances chainCursor via
       `advanceChainAndAppendFact`, but BEFORE any primop firing has
       added apply Fact / synthetic probes). This is the chain root
       for each primop firing's local advance — resetting localChainCursor
       to this at every firing ensures multiple firings (= when the
       cached ReplayCallbackArg's primop is invoked more than once) each
       reproduce the same cold-side AmbientResult instead of
       accumulating XOR contributions across firings. */
    auto initialChainCursor = std::make_shared<Hash>(*chainCursor);
    auto initialWalkFactsSize = walkFacts->size();

    auto * primOp = new
#if NIX_USE_BOEHMGC
        (GC)
#endif
        PrimOp{
            .name = "<replay-local-lambda>",
            .args = {"args"},
            .arity = 1,
            .impl = [dg, rootFSRootSaved, subjectSaved,
                     walkFactsSaved, chainCursorSaved,
                     initialChainCursor, initialWalkFactsSize,
                     applyDepthSaved, applyArgAncestrySaved,
                     outerContextSaved,
                     resolverSaved](
                EvalState & state, const PosIdx pos, Value ** args, Value & v) {
                /* Publish the live arg under the cb-arg arg's
                   structural identity so the OUTER walker's
                   `resolveStateHash` can resolve env facts whose `from`
                   is the arg's subject-id-evolved state hash at any
                   history-edge index. Registration carries the
                   subject + argAncestry (= `Arg{applyDepth+1}`
                   at `applyArgAncestry`), matching what
                   `makeCachedFnPrimOp`'s impl uses for its
                   `argSubject` / `callArgAncestry` at cold; the walker
                   iterates `envWalk` to find the matching edge.
                   Wraps args[0] in an `InterpreterObject` so the
                   walker can call getType / getInt / etc. live
                   against outer's actual Value. */
                if (resolverSaved) {
                    Subject argSubject{
                        Arg{*applyDepthSaved + 1}};
                    auto outerArgObj = std::make_shared<InterpreterObject>(
                        state, allocRootValue(args[0]));
                    registerAmbientResolverProxy(
                        *resolverSaved, std::move(argSubject),
                        *applyArgAncestrySaved, std::move(outerArgObj));
                }
                /* Each primop firing replays the ReplayCallbackArg's chain
                   advance (apply Fact + synthetic probes) on a LOCAL
                   copy of walkFacts/chainCursor so the ReplayCallbackArg's
                   persistent shared state isn't polluted across
                   firings.

                   Why this is needed: the ReplayCallbackArg (materialised by
                   `materialiseLocalStandin` and cached in
                   `ResolutionContext::memo`) is reused when the
                   walker dispatches multiple env facts whose
                   resolution paths force the same ReplayCallbackArg's primop.
                   Without a copy, walkFacts would accumulate
                   entries from prior firings and the synthetic's
                   `stampPerArgFields` would compute its `from` at a
                   later edge index than the writer's
                   `flushAmbient` stamped, breaking the
                   InnerValueResponse lookup.

                   localWalkFacts copies just the ReplayCallbackArg's
                   surface-probe portion (= entries pushed before
                   any primop firing), trimming any contributions
                   from prior firings. localChainCursor resets to
                   the snapshot taken at primop-construction time
                   (= post-surface-probe). This makes each firing's
                   chain advance independent of prior firings while
                   still starting from the right position in the
                   recorded chain. */
                auto localWalkFacts = std::make_shared<std::vector<ObservationSet>>(
                    walkFactsSaved->begin(),
                    walkFactsSaved->begin() + std::min(initialWalkFactsSize, walkFactsSaved->size()));
                auto localChainCursor = std::make_shared<Hash>(*initialChainCursor);
                /* Compose the recursive apply result's subject to
                   match what the recorder built at cold via
                   `OuterObject::queryApply` (= outer-object.cc
                   line ~280):
                     ApplyResultSubject{
                       fn  = this OuterObject's subject,
                       arg = Arg{localCell.depth},
                     }
                   where `localCell.depth = callerScope.depth + 1`.

                   This lambda primop fires on the ReplayCallbackArg that
                   represents the fn of the nested apply; its
                   `subject` IS the recorder's "this OuterObject's
                   subject". The arg subject is Arg{depth+1}
                   at applyArgAncestry, with `depth` threaded in through the
                   localArg sidecar. The ReplayCallbackArg's construction (in
                   dispatchApplyLive) requires the sidecar to carry
                   depth+argAncestry, so the optionals are always set
                   here. */
                Subject argSubject{
                    Arg{*applyDepthSaved + 1}};
                Subject syntheticSubject{ApplyResultSubject{
                    .fn = std::make_shared<const Subject>(subjectSaved),
                    .arg = std::make_shared<const Subject>(std::move(argSubject)),
                }};

                /* Apply argAncestry: Merkle(fn.argAncestry, arg.argAncestry). The arg
                   crosses the boundary as a fresh positional arg
                   (argAncestry=0); fn carries applyArgAncestrySaved (= callArgAncestry
                   from sidecar). Used for stamping the apply Fact AND
                   for the synthetic's downstream probes — both
                   mirror the writer's `TracingCallbackApplyResult` whose
                   argAncestry is the same Merkle. */
                Hash mergedApplyScope = combineArgAncestries(
                    *applyArgAncestrySaved, Hash{HashAlgorithm::SHA256});

                /* Advance the ReplayCallbackArg's chainCursor by the recorded
                   apply Fact's elementHash. Mirrors the writer's ambient
                   stamping in flushAmbient: subject =
                   ApplyResultSubject{fn, arg} = syntheticSubject;
                   argAncestry = mergedApplyScope; step =
                   walkFactsSaved->size() (= the apply Fact's position
                   in the writer's boundary facts list, AFTER the
                   ReplayCallbackArg's surface probes).

                   Walker's env dispatch of ε reads this updated
                   chainCursor as the AmbientResult. */
                {
                    size_t step = walkFactsSaved->size();
                    Hash applyArgAncestry = mergedApplyScope;

                    /* Polymorphic dispatch: subjectSaved can be a
                       DerivedSubject when the outer accesses a fn-typed
                       attribute of the callback arg (e.g. `arg.someFn 42`).
                       Mirrors the writer's ambient stamping in
                       TracingEvaluator::apply. */
                    auto fnSubjHex = stateHashAtSubject(
                        subjectSaved, applyArgAncestry, *walkFactsSaved, step)
                        .to_string(HashFormat::Base16, false);
                    Subject argSubjLocal{
                        Arg{*applyDepthSaved + 1}};
                    auto argSubjHex = stateHashAt(
                        argSubjLocal, applyArgAncestry, *walkFactsSaved, step)
                        .to_string(HashFormat::Base16, false);

                    /* Generic stamping via syntheticSubject. */
                    auto [path, roots] = pathAndRootsFromSubject(syntheticSubject);
                    std::vector<trace::QueryLeaf> fromStateHashes;
                    fromStateHashes.reserve(roots.size());
                    for (auto & root : roots) {
                        auto cid = stateHashAt(
                            root, applyArgAncestry, *walkFactsSaved, step);
                        fromStateHashes.emplace_back(cid.to_string(HashFormat::Base16, false));
                    }

                    trace::QueryApply stampedQ{fnSubjHex, argSubjHex};
                    nlohmann::json stampedJson = stampedQ;
                    if (!path.steps.empty())
                        stampedJson["params"]["path"] = path;
                    if (!fromStateHashes.empty())
                        stampedJson["params"]["fromStateHashes"] = fromStateHashes;
                    auto stampedReqHash = hashString(HashAlgorithm::SHA256, stampedJson.dump());

                    tracingCacheLog(
                        "walker primop applyFact: subject=%s applyArgAncestry=%s step=%zu fnHex=%s argHex=%s stampedReqHash=%s",
                        describe(syntheticSubject),
                        applyArgAncestry.to_string(HashFormat::Base16, false).substr(0, 12),
                        step,
                        fnSubjHex.substr(0, 12),
                        argSubjHex.substr(0, 12),
                        stampedReqHash.to_string(HashFormat::Base16, false).substr(0, 12));

                    nlohmann::json respJson = trace::ResultType{"apply"};
                    auto respPayload = jsonToCborString(respJson);
                    auto respHash = TracingDecisionGraph::computeResponseHash(respPayload);
                    auto elementHash = TracingDecisionGraph::xorFactIntoHash(
                        Hash(HashAlgorithm::SHA256), stampedReqHash, respHash);

                    Hash fromStateHash = fromStateHashes.empty()
                        ? Hash(HashAlgorithm::SHA256)
                        : Hash::parseNonSRIUnprefixed(
                              fromStateHashes[0].stateHash(), HashAlgorithm::SHA256);

                    ObservationSet edge;
                    edge.observations.push_back({fromStateHash, elementHash});
                    localWalkFacts->push_back(std::move(edge));
                    *localChainCursor = TracingDecisionGraph::xorHashes(
                        *localChainCursor, elementHash);
                }

                /* Synthetic shares the LOCAL history/cursor so its
                   probes don't pollute the ReplayCallbackArg's persistent
                   state. Scope = mergedApplyScope — matches writer's
                   `TracingCallbackApplyResult` which carries this same
                   Merkle argAncestry for its downstream observations. */
                auto synthetic = std::make_shared<ReplayCallbackArg>(
                    std::move(syntheticSubject), mergedApplyScope,
                    localWalkFacts, localChainCursor,
                    outerContextSaved, *dg, rootFSRootSaved, &state);
                /* Enable per-probe AmbientAsks validation. After the
                   `TracingCallbackApplyResult` writer change, the
                   apply-result observations live in the ambient chain
                   (= same boundary as the recursive apply Fact above),
                   so the synthetic's `getType` / `getInt` etc. must
                   history one AmbientAsks edge per probe to (a) keep
                   `chainCursor` aligned with the cold AmbientResult
                   (= principle 6 lockstep) and (b) detect divergence
                   when the outer's behaviour changed. The ReplayCallbackArg's
                   primop has already pushed the recursive apply Fact
                   to `walkFacts` and advanced `chainCursor`, so the
                   first synthetic probe stamps at `walkFacts.size() == 1`
                   — matching the writer's flushAmbient ambient
                   loop at index 1 (= position after `logAmbientApplyFact`'s
                   fact in the boundary). */
                synthetic->withAmbientAsksValidation();
                /* Propagate apply context so a nested cb-higher-order
                   case (= the apply result is itself a function whose
                   `toValueOrProxy` builds another `<replay-local-lambda>`
                   primop) composes the right depth/argAncestry downstream. */
                synthetic->withApplyContext(*applyDepthSaved, *applyArgAncestrySaved);

                /* Convert to a Value. ExprFromObject probes
                   synthetic for type/scalar value and constructs the
                   matching Value. */
                ExprFromObject(synthetic, nullptr, nullptr).eval(state, state.baseEnv, v);

                /* Propagate the firing's final chainCursor to the
                   ReplayCallbackArg's persistent state. `dispatchApplyLive`
                   reads this as the AmbientResult for the cb-apply
                   Fact. Each firing's local chain advance produces
                   the same final cursor by construction, so this
                   assignment is deterministic across multiple
                   firings of the same ReplayCallbackArg. */
                *chainCursorSaved = *localChainCursor;
            },
        };
    auto * val = evalState.allocValue();
    val->mkPrimOp(primOp);
    return allocRootValue(val);
}

std::optional<FunctionInfo> ReplayCallbackArg::getFunctionInfo()
{
    trace::QueryGetFunctionInfo query{std::string{}};
    auto fromStateHash = stampPerArgFields(query, subject, argAncestry, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query, outerContext);
    if (validateAgainstAmbientAsks)
        advanceChainAndAppendFact(decisionGraph, query, fromStateHash, rJson, *walkFacts, *chainCursor);
    else
        appendFactToWalk(query, fromStateHash, rJson, *walkFacts);
    trace::ResultFunctionInfo r = rJson;
    if (!r.hasInfo)
        return std::nullopt;
    return FunctionInfo{r.formals, r.ellipsis};
}

std::shared_ptr<Object> ReplayCallbackArg::queryApply(std::shared_ptr<Object> /*argObj*/)
{
    /* See header comment. Until ambient layer walker integration (task #74)
       or value-structure-atom reconstruction (task #75) lands, an
       apply on a recorded LocalObject can't be validated. Throw a
       recognizable signal — callers that route here will catch this
       and treat it as a walker miss. No caller routes here yet
       (the chain still goes through defeatCache); this is groundwork
       for the uniform-queryApply restructure. */
    throw Error(
        "ReplayCallbackArg::queryApply: cannot validate apply on a recorded "
        "frozen local without reconstructing its value structure (ambient layer "
        "walker not yet integrated)");
}

} // namespace nix
