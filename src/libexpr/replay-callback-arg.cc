#include "nix/expr/replay-callback-arg.hh"
#include "nix/expr/observation-set.hh"
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

/* Look up the recorded payload for `query` in the obsSet map the
   CallbackApply consumer populated at dispatch time. The map is
   keyed by requestHash; miss is a real error (there's no
   secondary source under the #103 redesign). */
template<typename Q>
static nlohmann::json readResponse(
    TracingDecisionGraph & dg, const Q & query,
    const std::shared_ptr<std::map<Hash, std::string>> & obsSetResponses = {})
{
    auto reqHash = TracingDecisionGraph::computeSelectorHash(query);
    tracingCacheLog(
        "rlo: read %s from=%s reqHash=%s",
        Q::tag, true ? std::string{}.substr(0, 12) : "<?>",
        reqHash.to_string(HashFormat::Base16, false).substr(0, 12));
    /* Under the #103 redesign, every outer probe's response is
       carried in the CallbackApply query's `argObsSet` — the
       consumer at dispatch time populates `obsSetResponses` with
       that CAS content. No secondary storage. Miss here is a real
       error. */
    (void) dg;
    if (obsSetResponses) {
        auto it = obsSetResponses->find(reqHash);
        if (it != obsSetResponses->end()) {
            tracingCacheLog(
                "rlo: obsSet HIT reqHash=%s",
                reqHash.to_string(HashFormat::Base16, false).substr(0, 12));
            return cborStringToJson(it->second);
        }
    }
    throw Error("ReplayCallbackArg: no recorded response for %s on local %s",
        Q::tag, true ? std::string{} : "<no-state-hash>");
}

/* Append the just-probed fact to `walkFacts`. Per-arg state hash
   evolution relies on the history extending in lockstep with the
   recorder. */
template<typename Q>
static void appendFactToWalk(
    const Q & query, const nlohmann::json & responseJson,
    std::vector<ObservationSet> & walkFacts)
{
    auto reqHash = TracingDecisionGraph::computeSelectorHash(query);
    auto responsePayload = jsonToCborString(responseJson);
    auto responseHash = TracingDecisionGraph::computeResponseHash(responsePayload);
    auto elementHash = TracingDecisionGraph::xorFactIntoHash(
        Hash(HashAlgorithm::SHA256), reqHash, responseHash);
    ObservationSet edge;
    edge.observations.push_back({Hash(HashAlgorithm::SHA256), elementHash});
    walkFacts.push_back(std::move(edge));
}

std::shared_ptr<Object> ReplayCallbackArg::maybeGetAttr(const std::string & name)
{
    /* Existence projects from parent WHNFAttrs.names (via whnf()
       cache lookup); only when present do we consume the recorded
       SelectorGetAttr response. */
    auto & w = whnf();
    auto * ap = std::get_if<trace::WHNFAttrs>(&w.payload);
    if (!ap)
        return nullptr;
    if (std::find(ap->names.begin(), ap->names.end(), name) == ap->names.end())
        return nullptr;
    auto childSel = decisionGraph.selectorPool.intern(trace::SelectorGetAttr{name, producer});
    auto & query = std::get<trace::SelectorGetAttr>(childSel->node);
    auto rJson = readResponse(decisionGraph, query, obsSetResponses);
    appendFactToWalk(query, rJson, *walkFacts);
    auto child = std::make_shared<ReplayCallbackArg>(
        childSel, walkFacts,
        decisionGraph, rootFSRoot, state);
    child->cachedWHNF = rJson.get<trace::ResultWHNF>();
    /* Derived children probe within the same callback firing, so
       the same obsSet serves their responses too. */
    if (obsSetResponses)
        child->withObsSetResponses(obsSetResponses);
    /* Navigation child inherits parent's argCell cell directly. */
    child->withArgCell(argCell);
    /* Inherit cb-arg apply context — derived navigation stays within
       the same cb-arg's depth/argAncestry (= the nested apply's positional
       depth is one deeper than the cb-arg's, regardless of how many
       getAttr/getListElem steps deep the apply happens). */
    if (applyDepth)
        child->withApplyContext(*applyDepth);
    return child;
}

const trace::ResultWHNF & ReplayCallbackArg::whnf()
{
    if (cachedWHNF)
        return *cachedWHNF;
    /* #186: mirror cold's TracingCallbackArg::whnf — the obsSet entry
       is keyed on the value's own Selector (SelectorArg for a
       positional arg, SelectorGetAttr for a nav descendant, etc.). */
    auto rJson = std::visit(
        [&](const auto & q) -> nlohmann::json {
            auto r = readResponse(decisionGraph, q, obsSetResponses);
            appendFactToWalk(q, r, *walkFacts);
            return r;
        },
        producer->node);
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
    /* Bounds project from parent WHNFList.size; retrieval consumes
       the recorded SelectorGetListElem response. */
    auto & w = whnf();
    auto * lp = std::get_if<trace::WHNFList>(&w.payload);
    if (!lp || index >= lp->size)
        throw Error("rlo getListElem: parent WHNF is %s, index %zu invalid", w.type, index);
    auto childSel = decisionGraph.selectorPool.intern(trace::SelectorGetListElem{index, producer});
    auto & query = std::get<trace::SelectorGetListElem>(childSel->node);
    auto rJson = readResponse(decisionGraph, query, obsSetResponses);
    appendFactToWalk(query, rJson, *walkFacts);
    auto child = std::make_shared<ReplayCallbackArg>(
        childSel, walkFacts,
        decisionGraph, rootFSRoot, state);
    child->cachedWHNF = rJson.get<trace::ResultWHNF>();
    child->withArgCell(argCell);
    if (applyDepth)
        child->withApplyContext(*applyDepth);
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
    /* The walker materialises the callback arg as a live Nix Value
       tree, lazily produced from the recorded obsSet. The shape
       depends on the recorded type:

       - `nFunction` (an inner-supplied lambda): reconstruct as a
         primop whose impl invokes the wrapped ReplayCallbackArg at
         apply time, serving the recorded response for the arg's
         current state.

       - Other types (attrset / list / scalars): return a thunk
         wrapping `ExprFromObject(self)` so the consumer materialises
         the value tree lazily via Object methods, each call reading
         the corresponding recorded response from the obsSet. */
    auto type = getType();
    if (type != nFunction) {
        auto * thunk = evalState.allocValue();
        auto * expr = new ExprFromObject(shared_from_this(), nullptr, std::move(resolver));
        evalState.mkThunk_(*thunk, expr);
        return allocRootValue(thunk);
    }

    auto * dg = &decisionGraph;
    auto rootFSRootSaved = rootFSRoot;
    auto producerSaved = producer;
    auto walkFactsSaved = walkFacts;
    auto applyDepthSaved = applyDepth;
    /* Capture the resolver so the primop can register the live arg
       it receives (args[0]) as an outer-direction proxy. The OUTER
       walker dispatches env facts whose `from` references the cb-arg
       arg's initial state hash (= what the inner-side queryFn closure
       captured at cold); without this registration the walker's
       resolveIdentity falls through "outer-arg by elimination" and the
       fact's dispatch fails. May be nullptr in unit-test paths that
       construct a ReplayCallbackArg without a resolver — registration is
       skipped then. */
    auto resolverSaved = resolver;
    auto initialWalkFactsSize = walkFacts->size();

    auto * primOp = new
#if NIX_USE_BOEHMGC
        (GC)
#endif
        PrimOp{
            .name = "<replay-local-lambda>",
            .args = {"args"},
            .arity = 1,
            .impl = [dg, rootFSRootSaved, producerSaved,
                     walkFactsSaved, initialWalkFactsSize,
                     applyDepthSaved,
                     resolverSaved](
                EvalState & state, const PosIdx pos, Value ** args, Value & v) {
                /* Applying a callback-produced value (e.g. `g.foo 10`
                   inside an outer callback body) is unsupported. */
                throw Error(
                    "tracing eval-cache: applying a function reached "
                    "through a callback's contra-arg is not currently "
                    "supported");
                /* Publish the live arg under the cb-arg arg's
                   structural identity so the OUTER walker's
                   `resolveIdentity` can resolve env facts whose `from`
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
                    /* Contra-arg identity: hardcoded sentinel matching
                       writer's OuterApply::run and walker's CallbackApply
                       dispatch. Scoped by the enclosing
                       SelectorCallbackApply. */
                    trace::SelectorArg argProducer{0};
                    auto outerArgObj = std::make_shared<InterpreterObject>(
                        state, allocRootValue(args[0]));
                    registerOuterResolverProxy(
                        *resolverSaved, trace::SelectorNode{argProducer},
                        std::move(outerArgObj));
                }
                /* Each primop firing replays the ReplayCallbackArg's
                   synthetic-probe sequence on a LOCAL copy of walkFacts
                   so the ReplayCallbackArg's persistent shared state
                   isn't polluted across firings.

                   The ReplayCallbackArg (materialised by
                   `materialiseLocalStandin` and cached in
                   `ResolutionContext::memo`) is reused when the walker
                   dispatches multiple env facts whose resolution paths
                   force the same ReplayCallbackArg's primop. Without a
                   copy, walkFacts would accumulate entries from prior
                   firings and the synthetic's `stampPerArgFields` would
                   compute its `from` at a later edge index than what
                   the recorded probe used, breaking the obsSet-map
                   lookup.

                   localWalkFacts copies just the ReplayCallbackArg's
                   surface-probe portion (= entries pushed before any
                   primop firing), trimming any contributions from
                   prior firings. */
                auto localWalkFacts = std::make_shared<std::vector<ObservationSet>>(
                    walkFactsSaved->begin(),
                    walkFactsSaved->begin() + std::min(initialWalkFactsSize, walkFactsSaved->size()));
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
                auto syntheticSel = dg->selectorPool.intern(trace::SelectorApply{producerSaved});
                auto synthetic = std::make_shared<ReplayCallbackArg>(
                    syntheticSel,
                    localWalkFacts,
                    *dg, rootFSRootSaved, &state);
                /* Propagate apply context so a nested cb-higher-order
                   case (= the apply result is itself a function whose
                   `toValueOrProxy` builds another `<replay-local-lambda>`
                   primop) composes the right depth downstream. */
                synthetic->withApplyContext(*applyDepthSaved);

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

std::optional<FunctionInfo> ReplayCallbackArg::getFunctionInfo()
{
    auto qSel = decisionGraph.selectorPool.intern(trace::SelectorGetFunctionInfo{producer});
    auto & query = std::get<trace::SelectorGetFunctionInfo>(qSel->node);
    auto rJson = readResponse(decisionGraph, query, obsSetResponses);
    appendFactToWalk(query, rJson, *walkFacts);
    trace::ResultFunctionInfo r = rJson;
    if (!r.hasInfo)
        return std::nullopt;
    return FunctionInfo{r.formals, r.ellipsis};
}

std::shared_ptr<Object> ReplayCallbackArg::queryApply(std::shared_ptr<Object> /*argObj*/)
{
    /* An apply on a recorded frozen local can't be validated
       without reconstructing its value structure. Throw a
       recognizable signal — callers catch this as a walker miss. */
    throw Error(
        "ReplayCallbackArg::queryApply: cannot validate apply on a recorded "
        "frozen local without reconstructing its value structure");
}

} // namespace nix
