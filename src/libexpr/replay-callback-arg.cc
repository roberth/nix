#include "nix/expr/replay-callback-arg.hh"
#include "nix/expr/expr-from-object.hh"
#include "nix/expr/interpreter-object.hh"
#include "nix/expr/object-type.hh"
#include "nix/expr/primops.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-object.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/expr/trace-types.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/error.hh"

namespace nix {

/* TODO: panic or resolve architecturally.
   Diagnostic bail flag set by RCA::queryApply when it hits null
   obsSetResponses. Walker catches check the flag and rethrow so the
   throw propagates to Nix's top-level and produces a stack trace via
   `--show-trace`. */
thread_local bool rcaBailFlag = false;


/* Look up the recorded payload for `query` in the obsSet map the
   CallbackApply consumer populated at dispatch time. The map is
   keyed by requestHash; miss is a real error (there's no
   secondary source under the #103 redesign). */
template<typename Q>
static nlohmann::json readResponse(
    const Q & query,
    const std::shared_ptr<std::map<Hash, std::string>> & obsSetResponses = {})
{
    auto reqHash = TracingDecisionGraph::computeSelectorHash(query);
    tracingCacheLog(
        "rlo: read %s reqHash=%s",
        Q::tag,
        reqHash.to_string(HashFormat::Base16, false).substr(0, 12));
    /* Under the #103 redesign, every outer probe's response is
       carried in the CallbackApply query's `argObsSet` — the
       consumer at dispatch time populates `obsSetResponses` with
       that CAS content. No secondary storage. Miss here is a real
       error. */
    if (obsSetResponses) {
        auto it = obsSetResponses->find(reqHash);
        if (it != obsSetResponses->end()) {
            tracingCacheLog(
                "rlo: obsSet HIT reqHash=%s",
                reqHash.to_string(HashFormat::Base16, false).substr(0, 12));
            return cborStringToJson(it->second);
        }
    }
    throw Error("ReplayCallbackArg: no recorded response for %s", Q::tag);
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
    auto rJson = readResponse(query, obsSetResponses);
    auto child = std::make_shared<ReplayCallbackArg>(
        childSel, decisionGraph, rootFSRoot, state);
    child->cachedWHNF = rJson.get<trace::ResultWHNF>();
    /* Derived children probe within the same callback firing, so
       the same obsSet serves their responses too. */
    if (obsSetResponses)
        child->withObsSetResponses(obsSetResponses);
    /* Navigation child inherits parent's argCell cell directly. */
    child->withArgCell(argCell);
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
            return readResponse(q, obsSetResponses);
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
    auto rJson = readResponse(query, obsSetResponses);
    auto child = std::make_shared<ReplayCallbackArg>(
        childSel, decisionGraph, rootFSRoot, state);
    child->cachedWHNF = rJson.get<trace::ResultWHNF>();
    child->withArgCell(argCell);
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
    /* A ReplayCallbackArg IS the cache for a frozen local arg whose
       original Value isn't live during replay. There's nothing to
       bypass to — callers must use `toValueOrProxy`. Reaching here is
       a bug in the caller (or in ExprFromObject::eval routing). */
    panic("ReplayCallbackArg::defeatCache: no live source — use toValueOrProxy");
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

    /* #217 M3: thin primop delegating to RCA::queryApply — symmetric to
       TCA::toValueOrProxy. Recording is in TCA::queryApply, replay is
       in RCA::queryApply, both keep primop wrappers minimal. */
    auto self = std::static_pointer_cast<ReplayCallbackArg>(shared_from_this());
    auto * primOp = new
#if NIX_USE_BOEHMGC
        (GC)
#endif
        PrimOp{
            .name = "<cb-replay>",
            .args = {"arg"},
            .arity = 1,
            .impl = [self, resolver](EvalState & state, const PosIdx, Value ** args, Value & v) {
                auto argObj = std::make_shared<InterpreterObject>(
                    state, allocRootValue(args[0]));
                auto resultObj = self->queryApply(argObj);
                ExprFromObject(resultObj, nullptr, resolver).eval(state, state.baseEnv, v);
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
    auto rJson = readResponse(query, obsSetResponses);
    trace::ResultFunctionInfo r = rJson;
    if (!r.hasInfo)
        return std::nullopt;
    return FunctionInfo{r.formals, r.ellipsis};
}

std::shared_ptr<Object> ReplayCallbackArg::queryApply(std::shared_ptr<Object> argObj)
{
    /* #217 higher-order callback replay, Object-level. Symmetric to
       TCA::queryApply: recording lives in the cold-side wrapper's
       queryApply, replay lives in the warm-side wrapper's queryApply.
       Serves the applyResult from a recorded SelectorCallbackApply in
       obsSetResponses whose argObsSet's probes match live-arg dispatch.

       Iterate obsSetResponses for SCA entries with fn matching this
       proxy's producer. For each candidate, replay recorded probes on
       argObj; on all-match, return an Object wrapping the recorded
       applyResult WHNF. On no match, throw — walker catches and
       treats as miss. */
    if (!obsSetResponses) {
        /* TODO: panic or resolve architecturally. Currently a throw
           with a bail flag so `nix eval --show-trace` gives a Nix
           stack trace. Walker's `catch(const std::exception&)` sites
           check the flag and rethrow to prevent retry loops. */
        extern thread_local bool rcaBailFlag;
        rcaBailFlag = true;
        throw Error("RCA::queryApply: no obsSetResponses (TODO: panic or resolve architecturally)");
    }

    for (const auto & [scaHash, recordedResp] : *obsSetResponses) {
        auto scaOpt = decisionGraph.selectorPool.find(scaHash);
        if (!scaOpt) continue;
        auto * sca = std::get_if<trace::SelectorCallbackApply>(&(*scaOpt)->node);
        if (!sca) continue;
        if (sca->parent->cachedHash != producer->cachedHash) continue;

        auto layer2Obs = decisionGraph.getObservationSet(sca->argObsSet);
        if (!layer2Obs) continue;

        bool allMatch = true;
        for (const auto & recordedProbe : *layer2Obs) {
            auto probeSelOpt = decisionGraph.selectorPool.find(recordedProbe.reqHash);
            if (!probeSelOpt) { allMatch = false; break; }
            trace::ResultVariant liveResult;
            try {
                /* O17: probe-replay type-dispatch. For SelectorCallbackApply
                   queries, recursively materialise a nested RCA from the
                   probe's own argObsSet and invoke argObj->queryApply on
                   it — this reproduces the nested apply's applyResult
                   WHNF that cold recorded. For other Selector kinds,
                   dispatchOuterQuery's identityWHNF / getter behaviour
                   is correct. */
                if (auto * nestedSca = std::get_if<trace::SelectorCallbackApply>(
                        &(*probeSelOpt)->node)) {
                    auto nestedObsSet = decisionGraph.getObservationSet(
                        nestedSca->argObsSet);
                    if (!nestedObsSet) { allMatch = false; break; }
                    auto nestedObsMap = std::make_shared<std::map<Hash, std::string>>();
                    for (const auto & obs : *nestedObsSet)
                        nestedObsMap->emplace(obs.reqHash, obs.responsePayload);
                    auto nestedRca = std::make_shared<ReplayCallbackArg>(
                        nestedSca->parent, decisionGraph, rootFSRoot, state);
                    nestedRca->withObsSetResponses(nestedObsMap);
                    auto nestedResultObj = argObj->queryApply(nestedRca);
                    if (!nestedResultObj) { allMatch = false; break; }
                    liveResult = computeWHNFFromObject(*nestedResultObj);
                } else {
                    auto qr = dispatchOuterQuery(argObj, (*probeSelOpt)->node);
                    liveResult = qr.result;
                }
            } catch (const std::exception &) { extern thread_local bool rcaBailFlag; if (rcaBailFlag) throw; /* rca-bail-diagnostic */ allMatch = false; break; }
            nlohmann::json liveJson = std::visit(
                [](const auto & r) -> nlohmann::json { return r; }, liveResult);
            auto livePayload = jsonToCborString(liveJson);
            if (livePayload != recordedProbe.responsePayload) {
                allMatch = false;
                break;
            }
        }
        if (allMatch) {
            auto matchedWhnf = cborStringToJson(recordedResp).get<trace::ResultWHNF>();
            tracingCacheLog(
                "RCA::queryApply: HIT via SCA=%s (obsSet=%s, %zu probes)",
                scaHash.to_string(HashFormat::Base16, false).substr(0, 12).c_str(),
                sca->argObsSet.to_string(HashFormat::Base16, false).substr(0, 12).c_str(),
                layer2Obs->size());
            /* Return a fresh RCA representing the applyResult. Its
               producer is this SCA; its whnf is the recorded response;
               its obsSet is empty (any further probes on the apply-result
               would need their own recorded chain). */
            auto childRca = std::make_shared<ReplayCallbackArg>(
                *scaOpt, decisionGraph, rootFSRoot, state);
            childRca->cachedWHNF = matchedWhnf;
            return childRca;
        }
    }
    throw Error("RCA::queryApply: no recorded SCA matches live arg (divergence)");
}

} // namespace nix
