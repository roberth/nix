#include "nix/expr/replay-local-object.hh"
#include "nix/expr/content-identity-via-asks.hh"
#include "nix/expr/expr-from-object.hh"
#include "nix/expr/object-type.hh"
#include "nix/expr/primops.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/expr/trace-types.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/error.hh"

namespace nix {

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
        auto cid = cidasks::contentIdAt(par.roots[i], scope, walkFacts, edgeIndex);
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
   because reqHash is `SHA-256(query{from = cidasks-evolved cdi})`
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
   (a) composes with `from = hex(contentIdAt(subject, scope,
   walkFacts, walkFacts.size()))` so its reqHash matches what the
   recorder wrote at this point in the chain, (b) is looked up as a
   singleton-requestSet edge from `*chainCursor → toFactSet`, and
   (c) on a match advances the shared chain cursor and appends the
   fact to the shared walk so subsequent probes compose against the
   correctly evolved cdis. On mismatch we throw a divergence signal
   which the surrounding walker layer turns into a miss → depth-1
   fallback handles re-eval. */
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
        auto responsePayload = jsonToCborString(responseJson);
        auto responseHash = TracingDecisionGraph::computeResponseHash(responsePayload);
        auto elementHash = TracingDecisionGraph::xorFactIntoHash(
            Hash(HashAlgorithm::SHA256), reqHash, responseHash);
        cidasks::Edge edge;
        edge.observations.push_back({fromCdi, elementHash});
        walkFacts.push_back(std::move(edge));
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
    trace::ResultMaybeType r = rJson;
    if (!r.type)
        return nullptr;
    /* Child Subject is DerivedSubject of THIS subject — `contentIdAt`
       on the child will recompute parent's cdi at the child's
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
    return child;
}

std::vector<std::string> ReplayLocalObject::getAttrNames()
{
    trace::QueryGetAttrNames query{std::string{}};
    auto fromCdi = stampPerArgFields(query, subject, scope, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query);
    if (validateAgainstAmbientAsks)
        advanceChainAndAppendFact(decisionGraph, query, fromCdi, rJson, *walkFacts, *chainCursor);
    trace::ResultListOfStrings r = rJson;
    return r.values;
}

std::string ReplayLocalObject::getStringIgnoreContext()
{
    trace::QueryGetString query{std::string{}};
    auto fromCdi = stampPerArgFields(query, subject, scope, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query);
    if (validateAgainstAmbientAsks)
        advanceChainAndAppendFact(decisionGraph, query, fromCdi, rJson, *walkFacts, *chainCursor);
    trace::ResultString r = rJson;
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
    trace::ResultPath r = rJson;
    return RootedPath{rootFSRoot, CanonPath{r.path}};
}

bool ReplayLocalObject::getBool(std::string_view)
{
    trace::QueryGetBool query{std::string{}};
    auto fromCdi = stampPerArgFields(query, subject, scope, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query);
    if (validateAgainstAmbientAsks)
        advanceChainAndAppendFact(decisionGraph, query, fromCdi, rJson, *walkFacts, *chainCursor);
    trace::ResultBool r = rJson;
    return r.value;
}

NixInt ReplayLocalObject::getInt(std::string_view)
{
    trace::QueryGetInt query{std::string{}};
    auto fromCdi = stampPerArgFields(query, subject, scope, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query);
    if (validateAgainstAmbientAsks)
        advanceChainAndAppendFact(decisionGraph, query, fromCdi, rJson, *walkFacts, *chainCursor);
    trace::ResultInt r = rJson;
    return NixInt{r.value};
}

NixFloat ReplayLocalObject::getFloat(std::string_view)
{
    trace::QueryGetFloat query{std::string{}};
    auto fromCdi = stampPerArgFields(query, subject, scope, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query);
    if (validateAgainstAmbientAsks)
        advanceChainAndAppendFact(decisionGraph, query, fromCdi, rJson, *walkFacts, *chainCursor);
    trace::ResultFloat r = rJson;
    return r.value;
}

size_t ReplayLocalObject::getListSize()
{
    trace::QueryGetListSize query{std::string{}};
    auto fromCdi = stampPerArgFields(query, subject, scope, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query);
    if (validateAgainstAmbientAsks)
        advanceChainAndAppendFact(decisionGraph, query, fromCdi, rJson, *walkFacts, *chainCursor);
    trace::ResultListSize r = rJson;
    return r.size;
}

std::shared_ptr<Object> ReplayLocalObject::getListElem(size_t index)
{
    trace::QueryGetListElem query{std::string{}, index};
    auto fromCdi = stampPerArgFields(query, subject, scope, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query);
    if (validateAgainstAmbientAsks)
        advanceChainAndAppendFact(decisionGraph, query, fromCdi, rJson, *walkFacts, *chainCursor);
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
    /* Per the via-Asks design's depth-2 replay section, a lambda
       LocalObject (= an inner-supplied function reaching back across
       the cb boundary) reconstructs as a primop. Its `impl`
       consults the `AmbientAsks` trie for a recorded edge matching
       the live arg's evolved content id, either reproducing the
       recorded apply result via downstream depth-1 facts or throwing
       a depth-2 divergence signal.

       Today's MVP: dispatch each recorded probe of the depth-2 edge
       (= edges from ∅ in AmbientAsks for this local's factSet at ∅)
       against `this` live, fold into a running factSet, and require
       it to reach the recorded `toFactSet`. On match, build a
       synthetic `ReplayLocalObject` keyed by the recursive apply's
       qH and let ExprFromObject convert it to a Value (= the recorded
       apply result flows through depth-1 facts about the recursive
       apply). On mismatch, throw a divergence signal that surrounding
       walker layers catch as a walker miss (= depth-1 fallback). */

    if (!state)
        throw Error(
            "ReplayLocalObject::defeatCache: no EvalState wired in for primop construction "
            "(walker integration is incomplete)");

    auto localIdSaved = localId;
    auto * dg = &decisionGraph;
    auto rootFSRootSaved = rootFSRoot;

    auto * primOp = new
#if NIX_USE_BOEHMGC
        (GC)
#endif
        PrimOp{
            .name = "<replay-local-lambda>",
            .args = {"args"},
            .arity = 1,
            .impl = [localIdSaved, dg, rootFSRootSaved](
                EvalState & state, const PosIdx pos, Value ** args, Value & v) {
                /* AmbientAsks structural check: this local must have
                   at least one recorded depth-2 edge from ∅. If not,
                   the local wasn't recorded by the cb apply we're
                   replaying — that's a divergence we must surface.
                   (Walker's surrounding try/catch turns this into a
                   miss; depth-1 fallback handles re-eval.) */
                auto emptySet = TracingDecisionGraph::emptySetHash();
                auto edges = dg->getAmbientAsks(emptySet);
                if (edges.empty())
                    throw Error(
                        "ReplayLocalObject primop: no depth-2 AmbientAsks edges from ∅ "
                        "for this local — recording is missing or doesn't apply (divergence)");
                /* Each recorded edge's requestSet names probes the
                   outer made on the local during the recorded cb
                   apply. For now we only check that an edge exists;
                   per-probe validation against a live arg requires
                   the outer to drive the probes through the
                   reconstructed value tree (see design doc's "Replay
                   (depth-2)" section). MVP cut: trust the edge and
                   reconstruct the apply result from depth-1 facts. */

                auto fromHex = localIdSaved.to_string(HashFormat::Base16, false);

                /* args[0]'s content id at the recursive cb apply
                   boundary is positional (PositionalSeed at the
                   newly opened cell's depth). We don't have a cell
                   chain here, so use the zero hex — the recorded
                   apply's argId at flush was also computed without
                   proper depth at this level, so they match by
                   construction. (Future work: thread depth through
                   the apply chain so sibling recursive applies
                   disambiguate.) */
                std::string argIdHex(64, '0');

                trace::QueryApply applyQuery{fromHex, argIdHex};
                auto applyResultId = TracingDecisionGraph::computeQueryHash(applyQuery);

                /* Reconstruct the recursive apply result as a
                   synthetic ReplayLocalObject; its methods read
                   recorded responses with from=applyResultIdHex. */
                auto synthetic = std::make_shared<ReplayLocalObject>(
                    applyResultId, *dg, rootFSRootSaved, &state);

                /* Convert to a Value. ExprFromObject probes
                   synthetic for type/scalar value and constructs the
                   matching Value. */
                ExprFromObject(synthetic, nullptr, nullptr).eval(state, state.baseEnv, v);
            },
        };
    auto * val = state->allocValue();
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
