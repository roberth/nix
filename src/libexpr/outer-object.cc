#include "nix/expr/outer-object.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/expr-from-object.hh"
#include "nix/expr/object-type.hh"
#include "nix/expr/tracing-object.hh"
#include "nix/util/error.hh"
#include "nix/util/source-accessor.hh"

namespace nix {

OuterObject::OuterObject(
    std::function<ref<const trace::Selector>()> producer_, std::shared_ptr<Object> outerObj_, OuterQueryFn queryFn, ref<SourceRoot> outerRootFSRoot, EvalState & outerState, trace::SelectorPool & selectorPool_, std::shared_ptr<ArgCell> argCell_, OuterApplyFn applyFn)
    : producer(std::move(producer_))
    , outerObj(std::move(outerObj_))
    , queryFn(std::move(queryFn))
    , applyFn(std::move(applyFn))
    , outerRootFSRoot(std::move(outerRootFSRoot))
    , outerState(outerState)
    , argCell(std::move(argCell_))
    , selectorPool(selectorPool_)
{
}

std::shared_ptr<Object> OuterObject::maybeGetAttr(const std::string & name)
{
    try {
    /* Existence is projected from parent WHNFAttrs.names; only if
       present do we issue the pure-retrieval SelectorGetAttr. */
    auto & w = whnf();
    auto * ap = std::get_if<trace::WHNFAttrs>(&w.payload);
    if (!ap)
        /* Not an attrs — delegate so the outer's inner throws its
           usual "getAttr on non-set" error. */
        return outerObj->maybeGetAttr(name);
    if (std::find(ap->names.begin(), ap->names.end(), name) == ap->names.end())
        return nullptr;
    /* Force the child value first so any callback firing's obs
       accumulate on our cell BEFORE we snapshot the producer for
       this getAttr's `from` field. Same reason as whnf()'s
       force-then-snapshot: producer() on a callback-produced
       parent samples the current runningObsSet, and the child
       value's evaluation is what populates the obs the walker
       will need to reproduce this probe. Second dispatch inside
       queryFn is cheap (child WHNF caches). */
    auto childProbe = outerObj->maybeGetAttr(name);
    if (!childProbe)
        return nullptr;
    auto preHex = getSelectorHashHex().value_or(std::string{});
    (void) computeWHNFFromObject(*childProbe, outerState);
    auto parentSel = producer();  // post-force snapshot
    auto parentQHex = parentSel->cachedHash.toHex();
    tracingCacheLog(
        "OO::maybeGetAttr '%s' preHex=%s postHex=%s (%s)",
        name.c_str(),
        preHex.substr(0, 12).c_str(),
        parentQHex.substr(0, 12).c_str(),
        preHex == parentQHex ? "SAME" : "CHANGED");
    auto qSel = selectorPool.intern(trace::SelectorGetAttr{name, parentSel});
    auto qr = queryFn(outerObj, qSel, parentSel, argCell);
    auto * r = std::get_if<trace::ResultWHNF>(&qr.result);
    if (!r)
        panic("OuterObject::maybeGetAttr: queryFn returned non-ResultWHNF");
    if (!qr.child)
        panic("OuterObject::maybeGetAttr: queryFn returned null child");
    /* Child's producer re-derives from OUR producer on every call, so
       that descendant probes fired later see the current SCA obsSet
       snapshot rather than the frozen one we computed for this probe.
       Enables "each OVR self-viable" per the emit-at-result-construction
       design: each descendant probe's parent chain reflects the
       runningObsSet as of THAT probe's moment. */
    auto parentProducer = producer;
    auto & poolRef = selectorPool;
    auto childProducer = [parentProducer, name, &poolRef]() -> ref<const trace::Selector> {
        auto freshParent = parentProducer();
        return poolRef.intern(trace::SelectorGetAttr{name, freshParent});
    };
    auto child = std::make_shared<OuterObject>(
        std::move(childProducer),
        qr.child, queryFn, outerRootFSRoot, outerState, selectorPool, argCell, applyFn);
    child->cachedWHNF = *r;
    return child;
    } catch (Error & e) {
        /* OuterObject sits on the recording side — the cache-bridged
           proxy for a value that lives across the boundary. Stamp
           so a stack trace distinguishes an OO dispatch from a TRO
           (walker replay) or RCA (callback-arg replay) dispatch. */
        auto parentHex = getSelectorHashHex().value_or(std::string{"?"});
        e.addTrace(nullptr, HintFmt("while dispatching cached attr '%s' via OuterObject (recording, parent Q=%s)",
                                    name, parentHex.substr(0, 12)), TracePrint::Always);
        throw;
    }
}

trace::ResultWHNF & OuterObject::whnf()
{
    if (cachedWHNF)
        return *cachedWHNF;
    /* For callback-produced OuterObjects the producer callable snapshots
       the callback firing's runningObsSet. Force the body first (which
       runs the callback and populates runningObsSet), THEN snapshot the
       producer so the CBApply's obsSet reflects the observations the
       body made — walker's dispatch materialises a ReplayCallbackArg
       backed by this obsSet and can serve the same probes.
       Forcing outerObj's WHNF is cheap on the second call: computeWHNF
       caches, and queryFn's identity dispatch (SelectorApply / CBApply
       cases in dispatchOuterQuery) just re-reads the cached WHNF. */
    (void) computeWHNFFromObject(*outerObj, outerState);
    auto p = producer();
    auto qr = queryFn(outerObj, p, p, argCell);
    auto * r = std::get_if<trace::ResultWHNF>(&qr.result);
    if (!r)
        panic("OuterObject::whnf: queryFn returned non-ResultWHNF");
    cachedWHNF = std::move(*r);
    return *cachedWHNF;
}

std::vector<std::string> OuterObject::getAttrNames()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFAttrs>(&w.payload);
    if (!p)
        throw Error("outer getAttrNames: WHNF payload not attrs (type %s)", w.type);
    return p->names;
}

std::string OuterObject::getStringIgnoreContext()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFString>(&w.payload);
    if (!p)
        throw Error("outer getStringIgnoreContext: WHNF payload not string (type %s)", w.type);
    return p->value;
}

std::string OuterObject::getStringWithoutContext()
{
    return getStringIgnoreContext();
}

std::pair<std::string, NixStringContext> OuterObject::getStringWithContext()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFString>(&w.payload);
    if (!p)
        throw Error("outer getStringWithContext: WHNF payload not string (type %s)", w.type);
    NixStringContext ctx;
    for (auto & s : p->context)
        ctx.insert(NixStringContextElem::parse(s));
    return {p->value, std::move(ctx)};
}

RootedPath OuterObject::getPath()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFPath>(&w.payload);
    if (!p)
        throw Error("outer getPath: WHNF payload not path (type %s)", w.type);
    /* Reconstruct the SourceRoot from the wire's sourceRootId (stamped
       at record time by stableRootIdentifier). Correct-or-miss: an
       identifier absent from the wire, or a lookup that misses this
       process's rootByIdentity, means we can't honestly serve the
       path — the caller must fall back to live inner rather than
       substitute a stand-in root. */
    if (!p->sourceRootId)
        throw Error("outer getPath: WHNFPath has no sourceRootId — anonymous "
                    "SourceRoots not yet supported across the cache boundary");
    auto root = outerState.getRootByIdentity(*p->sourceRootId);
    if (!root)
        throw Error(
            "outer getPath: sourceRootId '%s' not admitted in this process — "
            "the SourceRoot's producer (fetchTree, mkPath, etc.) needs to have "
            "run before this path can be reconstructed",
            *p->sourceRootId);
    return RootedPath{*root, CanonPath(p->path)};
}

bool OuterObject::getBool(std::string_view)
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFBool>(&w.payload);
    if (!p)
        throw Error("outer getBool: WHNF payload not bool (type %s)", w.type);
    return p->value;
}

NixInt OuterObject::getInt(std::string_view)
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFInt>(&w.payload);
    if (!p)
        throw Error("outer getInt: WHNF payload not int (type %s)", w.type);
    return NixInt{p->value};
}

NixFloat OuterObject::getFloat(std::string_view)
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFFloat>(&w.payload);
    if (!p)
        throw Error("outer getFloat: WHNF payload not float (type %s)", w.type);
    return p->value;
}

size_t OuterObject::getListSize()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFList>(&w.payload);
    if (!p)
        throw Error("outer getListSize: WHNF payload not list (type %s)", w.type);
    return p->size;
}

std::shared_ptr<Object> OuterObject::getListElem(size_t index)
{
    /* Bounds are projected from parent WHNFList.size; retrieval is
       SelectorGetListElem returning the child's WHNF. */
    auto & w = whnf();
    auto * lp = std::get_if<trace::WHNFList>(&w.payload);
    if (!lp || index >= lp->size)
        /* Not a list, or index out of bounds — delegate so the
           outer's inner throws the source-positioned error. */
        return outerObj->getListElem(index);
    auto parentSel = producer();
    auto qSel = selectorPool.intern(trace::SelectorGetListElem{index, parentSel});
    auto qr = queryFn(outerObj, qSel, parentSel, argCell);
    auto * r = std::get_if<trace::ResultWHNF>(&qr.result);
    if (!r)
        panic("OuterObject::getListElem: queryFn returned non-ResultWHNF");
    if (!qr.child)
        panic("OuterObject::getListElem: queryFn returned null child");
    /* Same re-derive pattern as maybeGetAttr: child's producer re-samples
       parent on each call so descendant probes see the current SCA
       obsSet snapshot. */
    auto parentProducer = producer;
    auto & poolRef = selectorPool;
    auto childProducer = [parentProducer, index, &poolRef]() -> ref<const trace::Selector> {
        auto freshParent = parentProducer();
        return poolRef.intern(trace::SelectorGetListElem{index, freshParent});
    };
    auto child = std::make_shared<OuterObject>(
        std::move(childProducer),
        qr.child, queryFn, outerRootFSRoot, outerState, selectorPool, argCell, applyFn);
    child->cachedWHNF = *r;
    return child;
}

ObjectType OuterObject::getTypeLazy()
{
    return getType();
}

ObjectType OuterObject::getType()
{
    return stringToObjectType(whnf().type);
}

RootValue OuterObject::defeatCache()
{
    throw Error("outer defeatCache: not supported on virtual values");
}

RootValue OuterObject::toValueOrProxy(EvalState & state, std::shared_ptr<OuterResolver> resolver)
{
    /* The virtual-value path: build a thunk that, when forced, evaluates
       an `ExprFromObject` proxy against this Object — same construction
       `Interpreter::apply` used to do in its `defeatCache` try/catch
       fallback, just relocated to where the dispatch belongs. */
    auto * thunk = state.allocValue();
    auto * expr = new ExprFromObject(ref<Object>(shared_from_this()), nullptr, std::move(resolver));
    state.mkThunk_(*thunk, expr);
    return allocRootValue(thunk);
}

Value * OuterObject::materialiseAsFunctionValue(
    EvalState & state,
    std::shared_ptr<OuterResolver> resolver,
    std::shared_ptr<Evaluator> /* innerEvaluator */)
{
    auto * v = state.allocValue();
    v->mkPrimOp(makeOuterFnPrimOp(shared_from_this(), std::move(resolver)));
    return v;
}

std::optional<FunctionInfo> OuterObject::getFunctionInfo()
{
    auto parentSel = producer();
    auto qSel = selectorPool.intern(trace::SelectorGetFunctionInfo{parentSel});
    auto qr = queryFn(outerObj, qSel, parentSel, argCell);
    auto * r = std::get_if<trace::ResultFunctionInfo>(&qr.result);
    if (!r || !r->hasInfo)
        return std::nullopt;
    return FunctionInfo{.formals = r->formals, .ellipsis = r->ellipsis};
}

PosIdx OuterObject::getPos()
{
    return noPos;
}

std::optional<std::vector<std::string>> OuterObject::getAttrPath()
{
    return std::nullopt;
}

std::shared_ptr<Object> OuterObject::queryApply(std::shared_ptr<Object> argObj)
{
    if (!applyFn)
        throw Error("outer apply: no apply callback");
    auto callerScope = effectiveArgCell(*this);
    /* #261: apply cell creation is lifted into `applyFn` — that's the
       site that knows whether the apply is a callback firing (needs a
       RecordingCallbackArgCell) or a plain outer apply (RegularArgCell). We
       just hand it the caller's scope as the parent. */
    auto ar = applyFn(outerObj, producer(), std::move(argObj), callerScope);
    /* The wrapping OuterObject uses:
       - `ar.producerFn` as its producer — for callback applies this
         constructs `SelectorCallbackApply{fn, argObsSet=<snapshot>}` on
         demand, so probes on this apply-result yield compositional
         `SelectorGetAttr(name, from=SelectorCallbackApply(...))`.
       - `callerScope` as its argCell — probes on this apply-result
         attribute to the caller's cell, so the outer probes flow into
         the enclosing scope's factset (e.g., the primop's seedCell for
         a callback firing inside the primop's body). The apply cell
         itself (created inside applyFn) carries the callback firing
         state — the producerFn captures it directly. */
    auto result = std::make_shared<OuterObject>(
        std::move(ar.producerFn),
        std::move(ar.applyResult), queryFn, outerRootFSRoot, outerState, selectorPool, callerScope, applyFn);
    return result;
}

} // namespace nix
