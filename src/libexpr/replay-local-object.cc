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
    auto scopeSaved = scope;
    auto walkFactsSaved = walkFacts;
    auto chainCursorSaved = chainCursor;
    auto applyDepthSaved = applyDepth;
    auto applyScopeSaved = applyScope;

    auto * primOp = new
#if NIX_USE_BOEHMGC
        (GC)
#endif
        PrimOp{
            .name = "<replay-local-lambda>",
            .args = {"args"},
            .arity = 1,
            .impl = [dg, rootFSRootSaved, subjectSaved, scopeSaved,
                     walkFactsSaved, chainCursorSaved,
                     applyDepthSaved, applyScopeSaved](
                EvalState & state, const PosIdx pos, Value ** args, Value & v) {
                /* Reconstruct the recursive apply result's subject to
                   match what the recorder built at cold via
                   AmbientObject::queryApply (= line ~280 of
                   ambient-object.cc):
                     ApplyResultSubject{
                       fn  = this AmbientObject's subject,
                       arg = PositionalSeed{localCell.depth},
                     }
                   where `localCell.depth = callerScope.depth + 1`.

                   The lambda primop fires on this RLO (= the fn of
                   the nested apply); its `subject` IS the recorder's
                   "this AmbientObject's subject". The arg subject is
                   PositionalSeed{applyDepth + 1} at applyScope, with
                   applyDepth = the cb-arg standin's seedCell depth
                   threaded in through the localArg sidecar.

                   Without applyContext (= legacy traces predating
                   the sidecar fields), fall back to the OpaqueContent
                   encoding which won't match the recorder; the
                   ensuing CAS-read miss is then the divergence
                   signal (= surrounding try/catch turns into a miss
                   → depth-1 fallback). */
                cidasks::Subject syntheticSubject;
                Hash syntheticScope = scopeSaved;
                if (applyDepthSaved && applyScopeSaved) {
                    cidasks::Subject argSubject{
                        cidasks::PositionalSeed{*applyDepthSaved + 1}};
                    syntheticSubject = cidasks::Subject{cidasks::ApplyResultSubject{
                        .fn = std::make_shared<const cidasks::Subject>(subjectSaved),
                        .arg = std::make_shared<const cidasks::Subject>(std::move(argSubject)),
                    }};
                    syntheticScope = *applyScopeSaved;
                } else {
                    /* Legacy path: opaque-content encoding (= won't
                       match the recorder's ApplyResultSubject
                       encoding, but doesn't regress anything that
                       worked before the sidecar fields landed). */
                    auto fromHex = cidasks::structuralAddressAfter(subjectSaved, scopeSaved, *walkFactsSaved)
                                       .to_string(HashFormat::Base16, false);
                    trace::QueryApply applyQuery{fromHex, std::string(64, '0')};
                    auto applyResultId = TracingDecisionGraph::computeQueryHash(applyQuery);
                    syntheticSubject = cidasks::Subject{cidasks::OpaqueContentSubject{applyResultId}};
                }

                /* Synthetic shares the parent's walk/cursor so
                   continued probing inside ExprFromObject::eval
                   advances the same d=2 chain. */
                auto synthetic = std::make_shared<ReplayLocalObject>(
                    std::move(syntheticSubject), syntheticScope,
                    walkFactsSaved, chainCursorSaved,
                    *dg, rootFSRootSaved, /*type=*/ nThunk, &state);

                /* Convert to a Value. ExprFromObject probes
                   synthetic for type/scalar value and constructs the
                   matching Value. */
                ExprFromObject(synthetic, nullptr, nullptr).eval(state, state.baseEnv, v);
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
