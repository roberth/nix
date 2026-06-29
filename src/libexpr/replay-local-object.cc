#include "nix/expr/replay-local-object.hh"
#include "nix/expr/content-identity-via-asks.hh"
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

ReplayLocalObject & ReplayLocalObject::withChainStart(Hash root)
{
    *chainCursor = std::move(root);
    if (validateAgainstAmbientAsks) {
        auto outgoing = decisionGraph.getAmbientAsks(*chainCursor);
        if (outgoing.empty())
            validateAgainstAmbientAsks = false;
    }
    return *this;
}

/* These mirror the same-named helpers in tracing-local-object.cc.
   Both translation units are unity-built into libnixexpr, so the
   helpers must have distinct names to avoid ODR collisions. */
static std::string replayFromOf(AmbientId id)
{
    return id.to_string(HashFormat::Base16, false);
}

static std::string replayFromHex(const Hash & h)
{
    return h.to_string(HashFormat::Base16, false);
}

template<typename Q>
static AmbientId replayDerivedLocalId(const Q & query)
{
    return TracingDecisionGraph::computeQueryHash(query);
}

/* Populate `query`'s per-arg fields (from, path, fromCIDs) so its
   reqHash matches what the writer flushed for the corresponding
   recorder probe. Multi-root applies fill fromCIDs[] with multiple
   leaf-root CDIs; the canonical `from` field carries fromCIDs[0].
   Returns the first-root CDI for callers (= used to log/diagnose
   and for the AmbientAsks chain advance). */
template <typename Q>
static Hash stampPerArgFields(
    Q & query,
    const cidasks::Subject & subject,
    const Hash & scope,
    const std::vector<cidasks::Edge> & walkFacts,
    size_t edgeIndex)
{
    auto par = cidasks::pathAndRootsFromSubject(subject);
    std::vector<trace::QueryLeaf> fromCIDs;
    fromCIDs.reserve(par.roots.size());
    Hash fromCdi(HashAlgorithm::SHA256);
    for (size_t i = 0; i < par.roots.size(); ++i) {
        auto cid = cidasks::scopeStateIdAt(par.roots[i], scope, walkFacts, edgeIndex);
        if (i == 0)
            fromCdi = cid;
        fromCIDs.emplace_back(cid.to_string(HashFormat::Base16, false));
    }
    query.from = fromCIDs.empty()
        ? trace::QueryLeaf{std::string{}}
        : fromCIDs[0];
    query.path = std::move(par.path);
    query.fromCIDs = std::move(fromCIDs);
    return fromCdi;
}

/* Look up the recorded payload for `query` in LocalResponseMap.
   The map is keyed by requestHash and that's sound at depth-2
   because reqHash is `SHA-256(query{from = cidasks-evolved scopeStateId})`
   — a pure function of (subject, scope, prior chain facts). Two
   recordings reaching the same reqHash necessarily observed the
   same history; a deterministic env then produces the same
   response, so first-writer-wins in the map can't return the
   wrong payload. */
template<typename Q>
static nlohmann::json readResponse(TracingDecisionGraph & dg, const Q & query)
{
    auto reqHash = TracingDecisionGraph::computeQueryHash(query);
    tracingCacheLog(
        "rlo: read %s from=%s reqHash=%s",
        Q::tag, query.from.isContent() ? query.from.contentHash().substr(0, 12) : "<?>",
        reqHash.to_string(HashFormat::Base16, false).substr(0, 12));
    auto payload = dg.getLocalResponsePayload(reqHash);
    if (!payload)
        throw Error("ReplayLocalObject: no recorded response for %s on local %s",
            Q::tag, query.from.isContent() ? query.from.contentHash() : "<ambient>");
    return cborStringToJson(*payload);
}

/* Multi-edge AmbientAsks walker: dispatch and validate one probe at
   a time. Per the design's "Replay (depth-2)" section, each probe
   (a) composes with `from = hex(scopeStateIdAt(subject, scope,
   walkFacts, walkFacts.size()))` so its reqHash matches what the
   recorder wrote at this point in the chain, (b) is looked up as a
   singleton-requestSet edge from `*chainCursor → toFactSet`, and
   (c) on a match advances the shared chain cursor and appends the
   fact to the shared walk so subsequent probes compose against the
   correctly evolved scopeStateIds. On mismatch we throw a divergence signal
   which the surrounding walker layer turns into a miss → depth-1
   fallback handles re-eval. */
/* Append the just-probed fact to `walkFacts` so the next probe's
   `stampPerArgFields` sees its own-loop contribution. Whether or not
   validation against AmbientAsks runs, the per-arg scopeStateId evolution
   relies on the walk extending in lockstep with the recorder — so
   this needs to fire on every probe, not just validated ones. */
template<typename Q>
static void appendFactToWalk(
    const Q & query, const Hash & fromCdi, const nlohmann::json & responseJson,
    std::vector<cidasks::Edge> & walkFacts)
{
    auto reqHash = TracingDecisionGraph::computeQueryHash(query);
    auto responsePayload = jsonToCborString(responseJson);
    auto responseHash = TracingDecisionGraph::computeResponseHash(responsePayload);
    auto elementHash = TracingDecisionGraph::xorFactIntoHash(
        Hash(HashAlgorithm::SHA256), reqHash, responseHash);
    cidasks::Edge edge;
    edge.observations.push_back({fromCdi, elementHash});
    walkFacts.push_back(std::move(edge));
}

template<typename Q>
static void advanceChainAndAppendFact(
    TracingDecisionGraph & dg, const Q & query, const Hash & fromCdi,
    const nlohmann::json & responseJson,
    std::vector<cidasks::Edge> & walkFacts, Hash & chainCursor)
{
    auto reqHash = TracingDecisionGraph::computeQueryHash(query);
    tracingCacheLog(
        "walk: probe %s from=%s reqHash=%s cursor=%s walkSize=%zu",
        Q::tag, fromCdi.to_string(HashFormat::Base16, false).substr(0, 12),
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
        appendFactToWalk(query, fromCdi, responseJson, walkFacts);
        chainCursor = toFactSet;
        return;
    }
    tracingCacheLog(
        "depth-2 divergence: probe %s reqHash=%s no AmbientAsks edge from %s",
        Q::tag, reqHash.to_string(HashFormat::Base16, false).substr(0, 12),
        chainCursor.to_string(HashFormat::Base16, false).substr(0, 12));
    throw Error(
        "depth-2 divergence: probe %s on local has no AmbientAsks edge from current factSet",
        Q::tag);
}

std::shared_ptr<Object> ReplayLocalObject::maybeGetAttr(const std::string & name)
{
    trace::QueryGetAttr query{name, std::string{}};
    auto fromCdi = stampPerArgFields(query, subject, scope, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query);
    if (validateAgainstAmbientAsks)
        advanceChainAndAppendFact(decisionGraph, query, fromCdi, rJson, *walkFacts, *chainCursor);
    else
        appendFactToWalk(query, fromCdi, rJson, *walkFacts);
    trace::ResultMaybeType r = rJson;
    if (!r.type)
        return nullptr;
    /* Child Subject is DerivedSubject of THIS subject — `scopeStateIdAt`
       on the child will recompute parent's scopeStateId at the child's
       current edge index, so any further parent observations are
       reflected automatically. Pass shared walk/cursor. */
    cidasks::Subject childSubject{cidasks::DerivedSubject{
        .parent = std::make_shared<const cidasks::Subject>(subject),
        .kind = cidasks::DerivedSubject::Kind::GetAttr,
        .name = name,
    }};
    auto child = std::make_shared<ReplayLocalObject>(
        std::move(childSubject), scope, walkFacts, chainCursor,
        decisionGraph, rootFSRoot, stringToObjectType(*r.type), state);
    /* Children inherit per-probe validation if the parent has it —
       they're observed within the same cb apply's recorded chain. */
    if (validateAgainstAmbientAsks)
        child->withAmbientAsksValidation();
    /* Navigation child inherits parent's argScope cell directly. */
    child->withScope(argScope);
    /* Inherit cb-arg apply context — derived navigation stays within
       the same cb-arg's depth/scope (= the nested apply's positional
       depth is one deeper than the cb-arg's, regardless of how many
       getAttr/getListElem steps deep the apply happens). */
    if (applyDepth && applyScope)
        child->withApplyContext(*applyDepth, *applyScope);
    return child;
}

std::vector<std::string> ReplayLocalObject::getAttrNames()
{
    trace::QueryGetAttrNames query{std::string{}};
    auto fromCdi = stampPerArgFields(query, subject, scope, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query);
    if (validateAgainstAmbientAsks)
        advanceChainAndAppendFact(decisionGraph, query, fromCdi, rJson, *walkFacts, *chainCursor);
    else
        appendFactToWalk(query, fromCdi, rJson, *walkFacts);
    trace::ResultListOfStrings r = rJson;
    return r.values;
}

std::string ReplayLocalObject::getStringIgnoreContext()
{
    if (knownStringIgnoreContext) return *knownStringIgnoreContext;
    trace::QueryGetString query{std::string{}};
    auto fromCdi = stampPerArgFields(query, subject, scope, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query);
    if (validateAgainstAmbientAsks)
        advanceChainAndAppendFact(decisionGraph, query, fromCdi, rJson, *walkFacts, *chainCursor);
    else
        appendFactToWalk(query, fromCdi, rJson, *walkFacts);
    trace::ResultString r = rJson;
    knownStringIgnoreContext = r.value;
    return r.value;
}

std::string ReplayLocalObject::getStringWithoutContext()
{
    return getStringIgnoreContext();
}

std::pair<std::string, NixStringContext> ReplayLocalObject::getStringWithContext()
{
    trace::QueryGetStringWithContext query{std::string{}};
    auto fromCdi = stampPerArgFields(query, subject, scope, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query);
    if (validateAgainstAmbientAsks)
        advanceChainAndAppendFact(decisionGraph, query, fromCdi, rJson, *walkFacts, *chainCursor);
    else
        appendFactToWalk(query, fromCdi, rJson, *walkFacts);
    trace::ResultStringWithContext r = rJson;
    NixStringContext ctx;
    for (auto & s : r.context)
        ctx.insert(NixStringContextElem::parse(s));
    return {r.value, std::move(ctx)};
}

RootedPath ReplayLocalObject::getPath()
{
    trace::QueryGetPath query{std::string{}};
    auto fromCdi = stampPerArgFields(query, subject, scope, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query);
    if (validateAgainstAmbientAsks)
        advanceChainAndAppendFact(decisionGraph, query, fromCdi, rJson, *walkFacts, *chainCursor);
    else
        appendFactToWalk(query, fromCdi, rJson, *walkFacts);
    trace::ResultPath r = rJson;
    return RootedPath{rootFSRoot, CanonPath{r.path}};
}

bool ReplayLocalObject::getBool(std::string_view)
{
    if (knownBool) return *knownBool;
    trace::QueryGetBool query{std::string{}};
    auto fromCdi = stampPerArgFields(query, subject, scope, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query);
    if (validateAgainstAmbientAsks)
        advanceChainAndAppendFact(decisionGraph, query, fromCdi, rJson, *walkFacts, *chainCursor);
    else
        appendFactToWalk(query, fromCdi, rJson, *walkFacts);
    trace::ResultBool r = rJson;
    knownBool = r.value;
    return r.value;
}

NixInt ReplayLocalObject::getInt(std::string_view)
{
    if (knownInt) return *knownInt;
    trace::QueryGetInt query{std::string{}};
    auto fromCdi = stampPerArgFields(query, subject, scope, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query);
    if (validateAgainstAmbientAsks)
        advanceChainAndAppendFact(decisionGraph, query, fromCdi, rJson, *walkFacts, *chainCursor);
    else
        appendFactToWalk(query, fromCdi, rJson, *walkFacts);
    trace::ResultInt r = rJson;
    knownInt = NixInt{r.value};
    return *knownInt;
}

NixFloat ReplayLocalObject::getFloat(std::string_view)
{
    if (knownFloat) return *knownFloat;
    trace::QueryGetFloat query{std::string{}};
    auto fromCdi = stampPerArgFields(query, subject, scope, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query);
    if (validateAgainstAmbientAsks)
        advanceChainAndAppendFact(decisionGraph, query, fromCdi, rJson, *walkFacts, *chainCursor);
    else
        appendFactToWalk(query, fromCdi, rJson, *walkFacts);
    trace::ResultFloat r = rJson;
    knownFloat = r.value;
    return r.value;
}

size_t ReplayLocalObject::getListSize()
{
    if (knownListSize) return *knownListSize;
    trace::QueryGetListSize query{std::string{}};
    auto fromCdi = stampPerArgFields(query, subject, scope, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query);
    if (validateAgainstAmbientAsks)
        advanceChainAndAppendFact(decisionGraph, query, fromCdi, rJson, *walkFacts, *chainCursor);
    else
        appendFactToWalk(query, fromCdi, rJson, *walkFacts);
    trace::ResultListSize r = rJson;
    knownListSize = r.size;
    return r.size;
}

std::shared_ptr<Object> ReplayLocalObject::getListElem(size_t index)
{
    trace::QueryGetListElem query{std::string{}, index};
    auto fromCdi = stampPerArgFields(query, subject, scope, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query);
    if (validateAgainstAmbientAsks)
        advanceChainAndAppendFact(decisionGraph, query, fromCdi, rJson, *walkFacts, *chainCursor);
    else
        appendFactToWalk(query, fromCdi, rJson, *walkFacts);
    trace::ResultType r = rJson;
    cidasks::Subject childSubject{cidasks::DerivedSubject{
        .parent = std::make_shared<const cidasks::Subject>(subject),
        .kind = cidasks::DerivedSubject::Kind::GetListElem,
        .index = index,
    }};
    auto child = std::make_shared<ReplayLocalObject>(
        std::move(childSubject), scope, walkFacts, chainCursor,
        decisionGraph, rootFSRoot, stringToObjectType(r.type), state);
    if (validateAgainstAmbientAsks)
        child->withAmbientAsksValidation();
    child->withScope(argScope);
    if (applyDepth && applyScope)
        child->withApplyContext(*applyDepth, *applyScope);
    return child;
}

ObjectType ReplayLocalObject::getType()
{
    /* Subsequent calls return cached. */
    if (getTypeProbed && knownType)
        return *knownType;
    trace::QueryGetType query{std::string{}};
    auto fromCdi = stampPerArgFields(query, subject, scope, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query);
    if (validateAgainstAmbientAsks)
        advanceChainAndAppendFact(decisionGraph, query, fromCdi, rJson, *walkFacts, *chainCursor);
    else
        appendFactToWalk(query, fromCdi, rJson, *walkFacts);
    trace::ResultType r = rJson;
    auto type = stringToObjectType(r.type);
    knownType = type;
    getTypeProbed = true;
    return type;
}

ObjectType ReplayLocalObject::getTypeLazy()
{
    return getType();
}

RootValue ReplayLocalObject::defeatCache()
{
    /* `defeatCache` means "bypass the cache and force the original
       expression to get the actual Value" — but a ReplayLocalObject
       IS the cache for a frozen local arg whose original Value isn't
       live during replay. There's nothing to bypass to. Callers that
       want a Value-shaped handle for `mkApp` should use
       `toValueOrProxy` instead. */
    throw Error(
        "ReplayLocalObject::defeatCache: cannot bypass the cache on a "
        "frozen local — use toValueOrProxy to obtain a primop standin");
}

RootValue ReplayLocalObject::toValueOrProxy(EvalState & evalState, std::shared_ptr<AmbientResolver> resolver)
{
    /* Per via-Asks Replay (depth-2): the walker reconstructs the
       LocalObject as a live Nix Value tree, lazily produced from
       CAS atoms. The shape depends on the recorded type:

       - `nFunction` (= an inner-supplied lambda LocalObject):
         reconstruct as a primop whose impl consults `AmbientAsks`
         at apply-time for a recorded edge matching the live arg's
         evolved content id, and reproduces the recorded apply
         result. Per the via-Asks doc's "Lambda LocalObjects don't
         need their body stored" — the application behavior lives
         in the recorded d=2 chain, not in a stored body.

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
    auto applyScopeSaved = applyScope;
    /* Capture the resolver so the primop can register the live arg
       it receives (args[0]) as an outer-direction proxy. The OUTER
       walker dispatches d=1 facts whose `from` references the cb-arg
       seed's initial CDI (= what the inner-side queryFn closure
       captured at cold); without this registration the walker's
       resolveCdiId falls through "outer-seed by elimination" and the
       fact's dispatch fails. May be nullptr in unit-test paths that
       construct a standin without a resolver — registration is
       skipped then. */
    auto resolverSaved = resolver;
    /* Capture the standin's chainCursor at primop-construction time
       (= AFTER ExprFromObject(standin).eval's `obj->getType()` call
       fires `standin.getType` and advances chainCursor via
       `advanceChainAndAppendFact`, but BEFORE any primop firing has
       added apply Fact / synthetic probes). This is the chain root
       for each primop firing's local advance — resetting localChainCursor
       to this at every firing ensures multiple firings (= when the
       cached standin's primop is invoked more than once) each
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
                     applyDepthSaved, applyScopeSaved,
                     resolverSaved](
                EvalState & state, const PosIdx pos, Value ** args, Value & v) {
                /* Publish the live arg under the cb-arg seed's
                   structural identity so the OUTER walker's
                   `resolveCdiId` can resolve d=1 facts whose `from`
                   is the seed's cidasks-evolved CDI at any
                   walk-edge index. Registration carries the
                   subject + scope (= `PositionalSeed{applyDepth+1}`
                   at `applyScope`), matching what
                   `makeCachedFnPrimOp`'s impl uses for its
                   `seedSubject` / `callScope` at cold; the walker
                   iterates `cidasksWalk` to find the matching edge.
                   Wraps args[0] in an `InterpreterObject` so the
                   walker can call getType / getInt / etc. live
                   against outer's actual Value. */
                if (resolverSaved) {
                    cidasks::Subject seedSubject{
                        cidasks::PositionalSeed{*applyDepthSaved + 1}};
                    auto outerArgObj = std::make_shared<InterpreterObject>(
                        state, allocRootValue(args[0]));
                    registerAmbientResolverProxy(
                        *resolverSaved, std::move(seedSubject),
                        *applyScopeSaved, std::move(outerArgObj));
                }
                /* Each primop firing replays the standin's chain
                   advance (apply Fact + synthetic probes) on a LOCAL
                   copy of walkFacts/chainCursor so the standin's
                   persistent shared state isn't polluted across
                   firings.

                   Why this is needed: the standin (materialised by
                   `materialiseLocalStandin` and cached in
                   `ResolutionContext::memo`) is reused when the
                   walker dispatches multiple d=1 facts whose
                   resolution paths force the same standin's primop.
                   Without a copy, walkFacts would accumulate
                   entries from prior firings and the synthetic's
                   `stampPerArgFields` would compute its `from` at a
                   later edge index than the writer's
                   `flushPendingAmbient` stamped, breaking the
                   LocalResponseMap lookup.

                   localWalkFacts copies just the standin's
                   surface-probe portion (= entries pushed before
                   any primop firing), trimming any contributions
                   from prior firings. localChainCursor resets to
                   the snapshot taken at primop-construction time
                   (= post-surface-probe). This makes each firing's
                   chain advance independent of prior firings while
                   still starting from the right position in the
                   recorded chain. */
                auto localWalkFacts = std::make_shared<std::vector<cidasks::Edge>>(
                    walkFactsSaved->begin(),
                    walkFactsSaved->begin() + std::min(initialWalkFactsSize, walkFactsSaved->size()));
                auto localChainCursor = std::make_shared<Hash>(*initialChainCursor);
                /* Compose the recursive apply result's subject to
                   match what the recorder built at cold via
                   `AmbientObject::queryApply` (= ambient-object.cc
                   line ~280):
                     ApplyResultSubject{
                       fn  = this AmbientObject's subject,
                       arg = PositionalSeed{localCell.depth},
                     }
                   where `localCell.depth = callerScope.depth + 1`.

                   This lambda primop fires on the RLO that
                   represents the fn of the nested apply; its
                   `subject` IS the recorder's "this AmbientObject's
                   subject". The arg subject is PositionalSeed{depth+1}
                   at applyScope, with `depth` threaded in through the
                   localArg sidecar. The standin's construction (in
                   dispatchApplyLive) requires the sidecar to carry
                   depth+scope, so the optionals are always set
                   here. */
                cidasks::Subject argSubject{
                    cidasks::PositionalSeed{*applyDepthSaved + 1}};
                cidasks::Subject syntheticSubject{cidasks::ApplyResultSubject{
                    .fn = std::make_shared<const cidasks::Subject>(subjectSaved),
                    .arg = std::make_shared<const cidasks::Subject>(std::move(argSubject)),
                }};

                /* Advance the standin's chainCursor by the recorded
                   apply Fact's elementHash — matching the writer's
                   d=2 fact in markApplyBoundary's enclosing-chain
                   path. The writer used subject=OpaqueContent{applyReqHash}
                   and result=ResultType{"apply"}; reproduce both sides
                   here so the cumulativeFactSet evolution matches.

                   The walker's d=1 dispatch of ε reads this updated
                   chainCursor as the AmbientResult; without this
                   advance, ε's response would be the pre-apply
                   cursor and the d=1 walk would derail. */
                {
                    /* Use edge 0 scopeStateIds (= initial structural-address
                       values) for fn/arg. Mirrors the recorder side:
                       `TracingEvaluator::apply` records
                       `QueryApply{fnId, argId}` where `fnId` /
                       `argId` come from `Object::getScopeStateIdHex()` at
                       construction-time (= `structuralAddressAfter`
                       with empty walk = `scopeStateIdAt(.., .., {}, 0)`).
                       The recursive apply Fact's identity is fixed
                       at IT::apply-time; observations recorded
                       between then and now should NOT shift its
                       reqHash, or the walker's stampedReqHash
                       diverges from what the writer's
                       `flushPendingAmbient` stamped for the
                       `logDepth2ApplyFact` entry. */
                    auto fnCdi = cidasks::scopeStateIdAt(
                        subjectSaved, *applyScopeSaved, *walkFactsSaved, 0);
                    auto argCdi = cidasks::scopeStateIdAt(
                        cidasks::Subject{cidasks::PositionalSeed{*applyDepthSaved + 1}},
                        *applyScopeSaved, *walkFactsSaved, 0);
                    trace::QueryApply applyQ{
                        fnCdi.to_string(HashFormat::Base16, false),
                        argCdi.to_string(HashFormat::Base16, false),
                    };
                    nlohmann::json applyJson = applyQ;
                    auto applyReqHash = hashString(HashAlgorithm::SHA256, applyJson.dump());

                    /* Match the writer's stamping in flushPendingAmbient's
                       d=2 loop for subject=OpaqueContent{applyReqHash}:
                       pathAndRootsFromSubject returns ({}, [OpaqueContent]);
                       fromCIDs[0] = OpaqueContent.hash = applyReqHash;
                       path stays empty; rewriteFromInQuery is a no-op
                       for QueryApply (which has no `from` field). The
                       only stamping effect on QueryApply is populating
                       fromCIDs in the JSON payload. */
                    trace::QueryApply stampedQ{applyQ.fn, applyQ.arg};
                    stampedQ.fromCIDs = {trace::QueryLeaf{
                        applyReqHash.to_string(HashFormat::Base16, false)}};
                    nlohmann::json stampedJson = stampedQ;
                    auto stampedReqHash = hashString(HashAlgorithm::SHA256, stampedJson.dump());

                    nlohmann::json respJson = trace::ResultType{"apply"};
                    auto respPayload = jsonToCborString(respJson);
                    auto respHash = TracingDecisionGraph::computeResponseHash(respPayload);
                    auto elementHash = TracingDecisionGraph::xorFactIntoHash(
                        Hash(HashAlgorithm::SHA256), stampedReqHash, respHash);

                    cidasks::Edge edge;
                    edge.observations.push_back({applyReqHash, elementHash});
                    localWalkFacts->push_back(std::move(edge));
                    *localChainCursor = TracingDecisionGraph::xorHashes(
                        *localChainCursor, elementHash);
                }

                /* Synthetic shares the LOCAL walk/cursor so its
                   probes don't pollute the standin's persistent
                   state. */
                auto synthetic = std::make_shared<ReplayLocalObject>(
                    std::move(syntheticSubject), *applyScopeSaved,
                    localWalkFacts, localChainCursor,
                    *dg, rootFSRootSaved, /*type=*/ nThunk, &state);
                /* Enable per-probe AmbientAsks validation. After the
                   `LambdaApplyResultObject` writer change, the
                   apply-result observations live in the d=2 chain
                   (= same boundary as the recursive apply Fact above),
                   so the synthetic's `getType` / `getInt` etc. must
                   walk one AmbientAsks edge per probe to (a) keep
                   `chainCursor` aligned with the cold AmbientResult
                   (= principle 6 lockstep) and (b) detect divergence
                   when the outer's behaviour changed. The standin's
                   primop has already pushed the recursive apply Fact
                   to `walkFacts` and advanced `chainCursor`, so the
                   first synthetic probe stamps at `walkFacts.size() == 1`
                   — matching the writer's flushPendingAmbient d=2
                   loop at index 1 (= position after `logDepth2ApplyFact`'s
                   fact in the boundary). */
                synthetic->withAmbientAsksValidation();
                /* Propagate apply context so a nested cb-higher-order
                   case (= the apply result is itself a function whose
                   `toValueOrProxy` builds another `<replay-local-lambda>`
                   primop) composes the right depth/scope downstream. */
                synthetic->withApplyContext(*applyDepthSaved, *applyScopeSaved);

                /* Convert to a Value. ExprFromObject probes
                   synthetic for type/scalar value and constructs the
                   matching Value. */
                ExprFromObject(synthetic, nullptr, nullptr).eval(state, state.baseEnv, v);

                /* Propagate the firing's final chainCursor to the
                   standin's persistent state. `dispatchApplyLive`
                   reads this as the AmbientResult for the cb-apply
                   Fact. Each firing's local chain advance produces
                   the same final cursor by construction, so this
                   assignment is deterministic across multiple
                   firings of the same standin. */
                *chainCursorSaved = *localChainCursor;
            },
        };
    auto * val = evalState.allocValue();
    val->mkPrimOp(primOp);
    return allocRootValue(val);
}

std::optional<FunctionInfo> ReplayLocalObject::getFunctionInfo()
{
    trace::QueryGetFunctionInfo query{std::string{}};
    auto fromCdi = stampPerArgFields(query, subject, scope, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query);
    if (validateAgainstAmbientAsks)
        advanceChainAndAppendFact(decisionGraph, query, fromCdi, rJson, *walkFacts, *chainCursor);
    else
        appendFactToWalk(query, fromCdi, rJson, *walkFacts);
    trace::ResultFunctionInfo r = rJson;
    if (!r.hasInfo)
        return std::nullopt;
    return FunctionInfo{r.formals, r.ellipsis};
}

std::shared_ptr<Object> ReplayLocalObject::queryApply(std::shared_ptr<Object> /*argObj*/)
{
    /* See header comment. Until depth-2 walker integration (task #74)
       or value-structure-atom reconstruction (task #75) lands, an
       apply on a recorded LocalObject can't be validated. Throw a
       recognizable signal — callers that route here will catch this
       and treat it as a walker miss. No caller routes here yet
       (the chain still goes through defeatCache); this is groundwork
       for the uniform-queryApply restructure. */
    throw Error(
        "ReplayLocalObject::queryApply: cannot validate apply on a recorded "
        "frozen local without reconstructing its value structure (depth-2 "
        "walker not yet integrated)");
}

} // namespace nix
