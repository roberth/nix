#include "nix/expr/outer-object.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/expr-from-object.hh"
#include "nix/expr/object-type.hh"
#include "nix/expr/tracing-object.hh"
#include "nix/util/error.hh"
#include "nix/util/source-accessor.hh"

namespace nix {

/* Populate `q`'s per-arg fields (from, path, fromStateHashes) so its
   reqHash matches what the writer flushed for the corresponding
   observation. */
template <typename Q>
static void stampPerArgFieldsAmbient(Q & q, const Subject & subject, const Hash & argAncestry)
{
    auto par = pathAndRootsFromSubject(subject);
    std::vector<trace::QueryLeaf> fromStateHashes;
    fromStateHashes.reserve(par.roots.size());
    for (size_t i = 0; i < par.roots.size(); ++i) {
        auto cid = stateHashAfter(par.roots[i], argAncestry, {});
        fromStateHashes.emplace_back(cid.to_string(HashFormat::Base16, false));
    }
    q.from = fromStateHashes.empty() ? trace::QueryLeaf{std::string{}} : fromStateHashes[0];
    q.path = std::move(par.path);
    q.fromStateHashes = std::move(fromStateHashes);
}

OuterObject::OuterObject(
    Subject subject_, OuterQueryFn queryFn, ref<SourceRoot> outerRootFSRoot, OuterApplyFn applyFn)
    : subject(std::move(subject_))
    , argAncestry(HashAlgorithm::SHA256)
    , queryFn(std::move(queryFn))
    , applyFn(std::move(applyFn))
    , outerRootFSRoot(std::move(outerRootFSRoot))
{
}

std::shared_ptr<Object> OuterObject::maybeGetAttr(const std::string & name)
{
    auto stateHash = stateHashAfterSubject(subject, argAncestry, {});
    trace::QueryGetAttr q{name, std::string{}};
    stampPerArgFieldsAmbient(q, subject, argAncestry);
    auto qr = queryFn(stateHash, q, subject, argAncestry);
    auto * r = std::get_if<trace::ResultMaybeType>(&qr.result);
    if (!r || !r->type)
        return nullptr;
    if (!qr.childId)
        throw Error("ambient maybeGetAttr: resolver didn't return child id");
    Subject childSubject{DerivedSubject{
        .parent = std::make_shared<const Subject>(subject),
        .kind = DerivedSubject::Kind::GetAttr,
        .name = name,
    }};
    auto child = std::make_shared<OuterObject>(std::move(childSubject), queryFn, outerRootFSRoot, applyFn);
    /* Navigation child inherits parent's argCell cell directly. */
    child->withArgCell(argCell);
    /* Inherit argAncestry so the child's `from` fields include
       the same state hash(Q) the parent uses. */
    child->withInheritedScope(argAncestry);
    return child;
}

trace::ResultWHNF & OuterObject::whnf()
{
    if (cachedWHNF)
        return *cachedWHNF;
    auto stateHash = stateHashAfterSubject(subject, argAncestry, {});
    trace::QueryGetWHNF q{std::string{}};
    stampPerArgFieldsAmbient(q, subject, argAncestry);
    auto qr = queryFn(stateHash, q, subject, argAncestry);
    auto * r = std::get_if<trace::ResultWHNF>(&qr.result);
    if (!r)
        throw Error("ambient getWHNF: unexpected result type");
    cachedWHNF = std::move(*r);
    return *cachedWHNF;
}

std::vector<std::string> OuterObject::getAttrNames()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFAttrs>(&w.payload);
    if (!p)
        throw Error("ambient getAttrNames: WHNF payload not attrs (type %s)", w.type);
    return p->names;
}

std::string OuterObject::getStringIgnoreContext()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFString>(&w.payload);
    if (!p)
        throw Error("ambient getStringIgnoreContext: WHNF payload not string (type %s)", w.type);
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
        throw Error("ambient getStringWithContext: WHNF payload not string (type %s)", w.type);
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
        throw Error("ambient getPath: WHNF payload not path (type %s)", w.type);
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
        throw Error("ambient getBool: WHNF payload not bool (type %s)", w.type);
    return p->value;
}

NixInt OuterObject::getInt(std::string_view)
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFInt>(&w.payload);
    if (!p)
        throw Error("ambient getInt: WHNF payload not int (type %s)", w.type);
    return NixInt{p->value};
}

NixFloat OuterObject::getFloat(std::string_view)
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFFloat>(&w.payload);
    if (!p)
        throw Error("ambient getFloat: WHNF payload not float (type %s)", w.type);
    return p->value;
}

size_t OuterObject::getListSize()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFList>(&w.payload);
    if (!p)
        throw Error("ambient getListSize: WHNF payload not list (type %s)", w.type);
    return p->size;
}

std::shared_ptr<Object> OuterObject::getListElem(size_t index)
{
    auto stateHash = stateHashAfterSubject(subject, argAncestry, {});
    trace::QueryGetListElem q{std::string{}, index};
    stampPerArgFieldsAmbient(q, subject, argAncestry);
    auto qr = queryFn(stateHash, q, subject, argAncestry);
    if (!qr.childId)
        throw Error("ambient getListElem: resolver didn't return child id");
    Subject childSubject{DerivedSubject{
        .parent = std::make_shared<const Subject>(subject),
        .kind = DerivedSubject::Kind::GetListElem,
        .index = index,
    }};
    auto child = std::make_shared<OuterObject>(std::move(childSubject), queryFn, outerRootFSRoot, applyFn);
    /* Navigation child inherits parent's argCell cell directly. */
    child->withArgCell(argCell);
    child->withInheritedScope(argAncestry);
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
    throw Error("ambient defeatCache: not supported on virtual values");
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
    auto stateHash = stateHashAfterSubject(subject, argAncestry, {});
    trace::QueryGetFunctionInfo q{std::string{}};
    stampPerArgFieldsAmbient(q, subject, argAncestry);
    auto qr = queryFn(stateHash, q, subject, argAncestry);
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
        throw Error("ambient apply: no apply callback");
    /* Thread the caller's effective argAncestry into applyFn so the cb
       apply's new local cell can chain off the right depth, even
       when `resolve(fnId)` returns an InterpreterObject without a
       proxy parent chain. Keep a copy of argObj for the result's
       cell before moving it into applyFn. */
    auto callerScope = effectiveArgCell(*this);
    auto argForScope = argObj;
    /* Each value crossing into a cb-apply boundary starts fresh as
       a Arg at the apply's reverse-De-Bruijn depth — no
       inherited Subject is propagated, so observations at the
       boundary are predictable regardless of where the arg came
       from. Must match what `OuterApply::run` computes for argSubject
       downstream so the registry's resultId and this proxy's state hash
       for queryFn lookups agree. */
    int localDepth = callerScope ? callerScope->depth + 1 : 0;
    Subject argSubject{Arg{localDepth}};
    applyFn(stateHashAfterSubject(subject, argAncestry, {}), std::move(argObj), callerScope);
    Subject resultSubject{ApplyResultSubject{
        .fn = std::make_shared<const Subject>(subject),
        .arg = std::make_shared<const Subject>(std::move(argSubject)),
    }};
    auto result = std::make_shared<OuterObject>(std::move(resultSubject), queryFn, outerRootFSRoot, applyFn);
    /* Apply-result argAncestry cell rooted at the caller's argAncestry. */
    auto cell = ArgCell::make(callerScope, std::move(argForScope));
    result->withArgCell(std::move(cell));
    result->withInheritedScope(argAncestry);
    return result;
}

} // namespace nix
