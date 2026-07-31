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
            return readResponse(decisionGraph, q, obsSetResponses);
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

    auto * primOp = new
#if NIX_USE_BOEHMGC
        (GC)
#endif
        PrimOp{
            .name = "<replay-local-lambda>",
            .args = {"args"},
            .arity = 1,
            .impl = [](EvalState &, const PosIdx, Value **, Value &) {
                /* Higher-order callback application — the reconstructed
                   primop being applied to another argument — is not
                   currently supported. When the design lights up (see
                   the cb-higher-order test family), reinstate the arg
                   registration + synthetic-Apply materialisation this
                   stub used to house. */
                throw Error(
                    "tracing eval-cache: applying a function reached "
                    "through a callback's contra-arg is not currently "
                    "supported");
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
