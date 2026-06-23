#include "nix/expr/replay-local-object.hh"
#include "nix/expr/expr-from-object.hh"
#include "nix/expr/object-type.hh"
#include "nix/expr/primops.hh"
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

template<typename Q>
static AmbientId replayDerivedLocalId(const Q & query)
{
    return TracingDecisionGraph::computeQueryHash(query);
}

template<typename Q>
static nlohmann::json readResponse(TracingDecisionGraph & dg, const Q & query)
{
    auto reqHash = TracingDecisionGraph::computeQueryHash(query);
    auto payload = dg.getResponsePayload(reqHash);
    if (!payload)
        throw Error("ReplayLocalObject: no recorded response for %s on local %s",
            Q::tag, query.from.isContent() ? query.from.contentHash() : "<ambient>");
    return cborStringToJson(*payload);
}

std::shared_ptr<Object> ReplayLocalObject::maybeGetAttr(const std::string & name)
{
    trace::QueryGetAttr query{name, replayFromOf(localId)};
    auto rJson = readResponse(decisionGraph, query);
    trace::ResultMaybeType r = rJson;
    if (!r.type)
        return nullptr;
    /* Propagate the child's type via in-band knownType so the
       dispatcher's getAttr branch can answer child->getType()
       without a separate pool lookup that the recorder never wrote. */
    auto child = std::make_shared<ReplayLocalObject>(
        replayDerivedLocalId(query), decisionGraph, rootFSRoot, stringToObjectType(*r.type), state);
    /* Navigation child inherits parent's argScope cell directly. */
    child->withScope(argScope);
    return child;
}

std::vector<std::string> ReplayLocalObject::getAttrNames()
{
    auto rJson = readResponse(decisionGraph, trace::QueryGetAttrNames{replayFromOf(localId)});
    trace::ResultListOfStrings r = rJson;
    return r.values;
}

std::string ReplayLocalObject::getStringIgnoreContext()
{
    auto rJson = readResponse(decisionGraph, trace::QueryGetString{replayFromOf(localId)});
    trace::ResultString r = rJson;
    return r.value;
}

std::string ReplayLocalObject::getStringWithoutContext()
{
    return getStringIgnoreContext();
}

std::pair<std::string, NixStringContext> ReplayLocalObject::getStringWithContext()
{
    auto rJson = readResponse(decisionGraph, trace::QueryGetStringWithContext{replayFromOf(localId)});
    trace::ResultStringWithContext r = rJson;
    /* Context strings were serialised as raw strings; rebuild a
       (possibly-empty) NixStringContext placeholder. The replayed
       outer doesn't need to interpret context elements beyond their
       string form. */
    NixStringContext ctx;
    for (auto & s : r.context)
        ctx.insert(NixStringContextElem::parse(s));
    return {r.value, std::move(ctx)};
}

RootedPath ReplayLocalObject::getPath()
{
    auto rJson = readResponse(decisionGraph, trace::QueryGetPath{replayFromOf(localId)});
    trace::ResultPath r = rJson;
    return RootedPath{rootFSRoot, CanonPath{r.path}};
}

bool ReplayLocalObject::getBool(std::string_view)
{
    auto rJson = readResponse(decisionGraph, trace::QueryGetBool{replayFromOf(localId)});
    trace::ResultBool r = rJson;
    return r.value;
}

NixInt ReplayLocalObject::getInt(std::string_view)
{
    auto rJson = readResponse(decisionGraph, trace::QueryGetInt{replayFromOf(localId)});
    trace::ResultInt r = rJson;
    return NixInt{r.value};
}

NixFloat ReplayLocalObject::getFloat(std::string_view)
{
    auto rJson = readResponse(decisionGraph, trace::QueryGetFloat{replayFromOf(localId)});
    trace::ResultFloat r = rJson;
    return r.value;
}

size_t ReplayLocalObject::getListSize()
{
    auto rJson = readResponse(decisionGraph, trace::QueryGetListSize{replayFromOf(localId)});
    trace::ResultListSize r = rJson;
    return r.size;
}

std::shared_ptr<Object> ReplayLocalObject::getListElem(size_t index)
{
    trace::QueryGetListElem query{replayFromOf(localId), index};
    /* The recorded response only carries the child's type, not
       value. We still derive an id for the child so the outer can
       chain further accesses on it. Propagate the type in-band so
       the dispatcher's child->getType() resolves without needing
       a separate getType fact the recorder never emits. */
    auto rJson = readResponse(decisionGraph, query);
    trace::ResultType r = rJson;
    auto child = std::make_shared<ReplayLocalObject>(
        replayDerivedLocalId(query), decisionGraph, rootFSRoot, stringToObjectType(r.type));
    /* Navigation child inherits parent's argScope cell directly. */
    child->withScope(argScope);
    return child;
}

ObjectType ReplayLocalObject::getType()
{
    if (knownType)
        return *knownType;
    auto rJson = readResponse(decisionGraph, trace::QueryGetType{replayFromOf(localId)});
    trace::ResultType r = rJson;
    return stringToObjectType(r.type);
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
                /* Dispatch each recorded probe of the depth-2 edge
                   against the bridged `this` (= via a synthetic
                   ReplayLocalObject reconstructed at args[0]'s
                   site). The bridged `this` reads recorded responses
                   from the Responses pool, so cur factSet matches
                   recorded toFactSet on a same-shape recording. The
                   divergence path (= live arg differs) surfaces at
                   the recursive apply's depth-1 dispatch chain
                   downstream — not here.

                   For the MVP, we trust the recording: the apply's
                   recursive depth-1 facts (= about the apply
                   RESULT) live in v13's trie keyed at the recursive
                   apply's qH, which we compute below. ExprFromObject
                   bridges the synthetic for the apply result. */

                auto fromHex = localIdSaved.to_string(HashFormat::Base16, false);

                /* args[0]'s content id at the recursive cb apply
                   boundary is positional (PositionalSeed at the
                   newly opened cell's depth). We don't have a cell
                   chain here, so use OpaqueContentSubject with the
                   zero hash — the recorded apply's argId at flush
                   was also computed without proper depth at this
                   level, so they match by construction. (Future
                   work: thread depth through the apply chain so
                   sibling recursive applies disambiguate.) */
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
    auto rJson = readResponse(decisionGraph, trace::QueryGetFunctionInfo{replayFromOf(localId)});
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
