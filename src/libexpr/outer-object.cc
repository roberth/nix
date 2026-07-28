#include "nix/expr/outer-object.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/expr-from-object.hh"
#include "nix/expr/object-type.hh"
#include "nix/expr/tracing-object.hh"
#include "nix/util/error.hh"
#include "nix/util/source-accessor.hh"

namespace nix {

OuterObject::OuterObject(
    std::function<trace::SelectorVariant()> producer_, std::shared_ptr<Object> outerObj_, OuterQueryFn queryFn, ref<SourceRoot> outerRootFSRoot, OuterApplyFn applyFn)
    : producer(std::move(producer_))
    , outerObj(std::move(outerObj_))
    , queryFn(std::move(queryFn))
    , applyFn(std::move(applyFn))
    , outerRootFSRoot(std::move(outerRootFSRoot))
{
}

std::shared_ptr<Object> OuterObject::maybeGetAttr(const std::string & name)
{
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
    (void) computeWHNFFromObject(*childProbe);
    /* Child producer = SelectorGetAttr{name, from=parent's Q hash hex}
       — parent hex computed here reflects the now-populated obsSet. */
    auto parentQHex = getSelectorHashHex().value_or(std::string{});
    trace::SelectorGetAttr q{name, parentQHex};
    auto qr = queryFn(outerObj, q, producer(), argCell);
    auto * r = std::get_if<trace::ResultWHNF>(&qr.result);
    if (!r)
        throw Error("outer maybeGetAttr: queryFn returned unexpected result type");
    if (!qr.child)
        throw Error("outer maybeGetAttr: queryFn didn't return a child Object");
    auto child = std::make_shared<OuterObject>(
        [q]() { return trace::SelectorVariant{q}; },
        qr.child, queryFn, outerRootFSRoot, applyFn);
    /* Navigation child inherits parent's argCell cell directly. */
    child->withArgCell(argCell);
    child->cachedWHNF = *r;
    return child;
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
    (void) computeWHNFFromObject(*outerObj);
    auto p = producer();
    auto qr = queryFn(outerObj, p, p, argCell);
    auto * r = std::get_if<trace::ResultWHNF>(&qr.result);
    if (!r)
        throw Error("outer getWHNF: unexpected result type");
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
    /* lazy-paths: reuse the outer EvalState's `rootFSRoot` so the
       SourceRoot outlives the Value the outer evaluator constructs
       from this path. A one-off `SourceRoot::make` here would be
       freed when the returned RootedPath drops, leaving Value's raw
       SourceRoot pointer dangling. */
    return RootedPath{outerRootFSRoot, CanonPath(p->path)};
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
    /* Child producer = SelectorGetListElem{from=parent's Q hash hex, index}. */
    auto parentQHex = getSelectorHashHex().value_or(std::string{});
    trace::SelectorGetListElem q{parentQHex, index};
    auto qr = queryFn(outerObj, q, producer(), argCell);
    auto * r = std::get_if<trace::ResultWHNF>(&qr.result);
    if (!r)
        throw Error("outer getListElem: queryFn returned unexpected result type");
    if (!qr.child)
        throw Error("outer getListElem: queryFn didn't return a child Object");
    auto child = std::make_shared<OuterObject>(
        [q]() { return trace::SelectorVariant{q}; },
        qr.child, queryFn, outerRootFSRoot, applyFn);
    /* Navigation child inherits parent's argCell cell directly. */
    child->withArgCell(argCell);
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
    auto * expr = new ExprFromObject(shared_from_this(), nullptr, std::move(resolver));
    state.mkThunk_(*thunk, expr);
    return allocRootValue(thunk);
}

std::optional<FunctionInfo> OuterObject::getFunctionInfo()
{
    /* #183: q.from = parent's Q-space identity. */
    trace::SelectorGetFunctionInfo q{getSelectorHashHex().value_or(std::string{})};
    auto qr = queryFn(outerObj, q, producer(), argCell);
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
    /* #188: one cell per apply. Create the apply's cell here and thread
       it into applyFn so OuterApply::run reuses it as its localCell. */
    auto applyCell = ArgCell::make(callerScope, argObj);
    auto ar = applyFn(outerObj, producer(), std::move(argObj), applyCell);
    /* The wrapping OuterObject uses:
       - `ar.producerFn` as its producer — for callback applies this
         constructs `SelectorCallbackApply{fn, argObsSet=<snapshot>}` on
         demand, so probes on this apply-result yield compositional
         `SelectorGetAttr(name, from=SelectorCallbackApply(...))`.
       - `callerScope` (parent of applyCell) as its argCell — probes
         on this apply-result attribute to the caller's cell, so the
         outer probes flow into the enclosing scope's factset (e.g.,
         the primop's seedCell for a callback firing inside the primop's
         body). applyCell itself carries the callback firing state
         (its `callbackState`) — the producerFn captures it directly. */
    auto result = std::make_shared<OuterObject>(
        std::move(ar.producerFn),
        std::move(ar.applyResult), queryFn, outerRootFSRoot, applyFn);
    result->withArgCell(callerScope);
    return result;
}

} // namespace nix
