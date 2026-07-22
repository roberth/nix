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

/* Populate `query`'s per-arg fields (from, path, fromStateHashes)
   so its reqHash matches what the recorder produced. Multi-root
   applies fill fromStateHashes[] with multiple leaf-root state
   hashes; the canonical `from` field carries fromStateHashes[0].
   Returns the first-root state hash for callers. */
template <typename Q>
static Hash stampPerArgFields(
    Q & query,
    const Subject & subject,
    const Hash & argAncestry,
    const std::vector<ObservationSet> & walkFacts,
    size_t step)
{
    /* Contra-arg roots (`Arg{depth}`) don't have an evolving state
       hash. Their `from` field is their structural id
       (`SHA("positional-<depth>") XOR argAncestry`) — computed at
       empty history, invariant across probes. Cold and warm stamp
       identically at any moment; matches cold's obsSet queryHashes.
       `walkFacts` / `step` retained in the signature for call-site
       compatibility, not used here. */
    (void) walkFacts;
    (void) step;
    auto par = pathAndRootsFromSubject(subject);
    std::vector<trace::QueryLeaf> fromStateHashes;
    fromStateHashes.reserve(par.roots.size());
    Hash fromStateHash(HashAlgorithm::SHA256);
    for (size_t i = 0; i < par.roots.size(); ++i) {
        auto cid = stateHashAfter(par.roots[i], argAncestry, {});
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

/* Look up the recorded payload for `query` in the obsSet map the
   CallbackApply consumer populated at dispatch time. The map is
   keyed by requestHash; miss is a real error (there's no
   secondary source under the #103 redesign). */
template<typename Q>
static nlohmann::json readResponse(
    TracingDecisionGraph & dg, const Q & query,
    const std::shared_ptr<std::map<Hash, std::string>> & obsSetResponses = {})
{
    auto reqHash = TracingDecisionGraph::computeQueryHash(query);
    tracingCacheLog(
        "rlo: read %s from=%s reqHash=%s",
        Q::tag, query.from.isStateHash() ? query.from.stateHash().substr(0, 12) : "<?>",
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
        Q::tag, query.from.isStateHash() ? query.from.stateHash() : "<no-state-hash>");
}

/* Append the just-probed fact to `walkFacts` so the next probe's
   `stampPerArgFields` sees its own-loop contribution. Per-arg state
   hash evolution relies on the history extending in lockstep with
   the recorder. */
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

std::shared_ptr<Object> ReplayCallbackArg::maybeGetAttr(const std::string & name)
{
    trace::QueryGetAttr query{name, std::string{}};
    auto fromStateHash = stampPerArgFields(query, subject, argAncestry, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query, obsSetResponses);
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
        std::move(childSubject), argAncestry, walkFacts,
        decisionGraph, rootFSRoot, state);
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
    auto rJson = readResponse(decisionGraph, query, obsSetResponses);
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
    auto rJson = readResponse(decisionGraph, query, obsSetResponses);
    appendFactToWalk(query, fromStateHash, rJson, *walkFacts);
    Subject childSubject{DerivedSubject{
        .parent = std::make_shared<const Subject>(subject),
        .kind = DerivedSubject::Kind::GetListElem,
        .index = index,
    }};
    auto child = std::make_shared<ReplayCallbackArg>(
        std::move(childSubject), argAncestry, walkFacts,
        decisionGraph, rootFSRoot, state);
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
    auto subjectSaved = subject;
    auto walkFactsSaved = walkFacts;
    auto applyDepthSaved = applyDepth;
    auto applyArgAncestrySaved = applyArgAncestry;
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
                     walkFactsSaved, initialWalkFactsSize,
                     applyDepthSaved, applyArgAncestrySaved,
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
                    registerOuterResolverProxy(
                        *resolverSaved, std::move(argSubject),
                        *applyArgAncestrySaved, std::move(outerArgObj));
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

                /* Stamp the recursive apply Fact into localWalkFacts:
                   subject = ApplyResultSubject{fn, arg} =
                   syntheticSubject; argAncestry = mergedApplyScope;
                   step = walkFactsSaved->size() (= the apply Fact's
                   position in the writer's history, AFTER the
                   ReplayCallbackArg's surface probes). Extends the
                   synthetic's history so its `from` stamping picks up
                   at the right edge. */
                {
                    size_t step = walkFactsSaved->size();
                    Hash applyArgAncestry = mergedApplyScope;

                    /* Polymorphic dispatch: subjectSaved can be a
                       DerivedSubject when the outer accesses a fn-typed
                       attribute of the callback arg (e.g. `arg.someFn 42`).
                       Mirrors the writer's stamping in
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
                }

                /* Synthetic shares the LOCAL history so its probes
                   don't pollute the ReplayCallbackArg's persistent
                   state. Scope = mergedApplyScope — matches writer's
                   `TracingCallbackApplyResult` which carries this same
                   Merkle argAncestry for its downstream observations. */
                auto synthetic = std::make_shared<ReplayCallbackArg>(
                    std::move(syntheticSubject), mergedApplyScope,
                    localWalkFacts,
                    *dg, rootFSRootSaved, &state);
                /* Propagate apply context so a nested cb-higher-order
                   case (= the apply result is itself a function whose
                   `toValueOrProxy` builds another `<replay-local-lambda>`
                   primop) composes the right depth/argAncestry downstream. */
                synthetic->withApplyContext(*applyDepthSaved, *applyArgAncestrySaved);

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
    trace::QueryGetFunctionInfo query{std::string{}};
    auto fromStateHash = stampPerArgFields(query, subject, argAncestry, *walkFacts, walkFacts->size());
    auto rJson = readResponse(decisionGraph, query, obsSetResponses);
    appendFactToWalk(query, fromStateHash, rJson, *walkFacts);
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
