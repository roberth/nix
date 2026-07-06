#include "nix/expr/ambient-object.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/expr-from-object.hh"
#include "nix/expr/object-type.hh"
#include "nix/expr/tracing-object.hh"
#include "nix/util/error.hh"
#include "nix/util/source-accessor.hh"

namespace nix {

/* Populate `q`'s per-arg fields (from, path, fromCIDs) so its
   reqHash matches what the writer flushed for the corresponding
   observation. */
template <typename Q>
static void stampPerArgFieldsAmbient(Q & q, const Subject & subject, const Hash & inheritedScope)
{
    auto par = pathAndRootsFromSubject(subject);
    std::vector<trace::QueryLeaf> fromCIDs;
    fromCIDs.reserve(par.roots.size());
    for (size_t i = 0; i < par.roots.size(); ++i) {
        auto cid = scopeStateIdAfter(par.roots[i], inheritedScope, {});
        fromCIDs.emplace_back(cid.to_string(HashFormat::Base16, false));
    }
    q.from = fromCIDs.empty() ? trace::QueryLeaf{std::string{}} : fromCIDs[0];
    q.path = std::move(par.path);
    q.fromCIDs = std::move(fromCIDs);
}

AmbientObject::AmbientObject(
    Subject subject_, AmbientQueryFn queryFn, ref<SourceRoot> ambientRootFSRoot, AmbientApplyFn applyFn)
    : subject(std::move(subject_))
    , inheritedScope(HashAlgorithm::SHA256)
    , queryFn(std::move(queryFn))
    , applyFn(std::move(applyFn))
    , ambientRootFSRoot(std::move(ambientRootFSRoot))
{
}

std::shared_ptr<Object> AmbientObject::maybeGetAttr(const std::string & name)
{
    auto scopeStateId = structuralAddressAfter(subject, inheritedScope, {});
    trace::QueryGetAttr q{name, std::string{}};
    stampPerArgFieldsAmbient(q, subject, inheritedScope);
    auto qr = queryFn(scopeStateId, q, subject, inheritedScope);
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
    auto child = std::make_shared<AmbientObject>(std::move(childSubject), queryFn, ambientRootFSRoot, applyFn);
    /* Navigation child inherits parent's argCell cell directly. */
    child->withScope(argCell);
    /* Inherit content-id scope so the child's `from` fields include
       the same argStateId(Q) the parent uses. */
    child->withInheritedScope(inheritedScope);
    return child;
}

trace::ResultWHNF & AmbientObject::whnf()
{
    if (cachedWHNF)
        return *cachedWHNF;
    auto scopeStateId = structuralAddressAfter(subject, inheritedScope, {});
    trace::QueryGetWHNF q{std::string{}};
    stampPerArgFieldsAmbient(q, subject, inheritedScope);
    auto qr = queryFn(scopeStateId, q, subject, inheritedScope);
    auto * r = std::get_if<trace::ResultWHNF>(&qr.result);
    if (!r)
        throw Error("ambient getWHNF: unexpected result type");
    cachedWHNF = std::move(*r);
    return *cachedWHNF;
}

std::vector<std::string> AmbientObject::getAttrNames()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFAttrs>(&w.payload);
    if (!p)
        throw Error("ambient getAttrNames: WHNF payload not attrs (type %s)", w.type);
    return p->names;
}

std::string AmbientObject::getStringIgnoreContext()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFString>(&w.payload);
    if (!p)
        throw Error("ambient getStringIgnoreContext: WHNF payload not string (type %s)", w.type);
    return p->value;
}

std::string AmbientObject::getStringWithoutContext()
{
    return getStringIgnoreContext();
}

std::pair<std::string, NixStringContext> AmbientObject::getStringWithContext()
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

RootedPath AmbientObject::getPath()
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
    return RootedPath{ambientRootFSRoot, CanonPath(p->path)};
}

bool AmbientObject::getBool(std::string_view)
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFBool>(&w.payload);
    if (!p)
        throw Error("ambient getBool: WHNF payload not bool (type %s)", w.type);
    return p->value;
}

NixInt AmbientObject::getInt(std::string_view)
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFInt>(&w.payload);
    if (!p)
        throw Error("ambient getInt: WHNF payload not int (type %s)", w.type);
    return NixInt{p->value};
}

NixFloat AmbientObject::getFloat(std::string_view)
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFFloat>(&w.payload);
    if (!p)
        throw Error("ambient getFloat: WHNF payload not float (type %s)", w.type);
    return p->value;
}

size_t AmbientObject::getListSize()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFList>(&w.payload);
    if (!p)
        throw Error("ambient getListSize: WHNF payload not list (type %s)", w.type);
    return p->size;
}

std::shared_ptr<Object> AmbientObject::getListElem(size_t index)
{
    auto scopeStateId = structuralAddressAfter(subject, inheritedScope, {});
    trace::QueryGetListElem q{std::string{}, index};
    stampPerArgFieldsAmbient(q, subject, inheritedScope);
    auto qr = queryFn(scopeStateId, q, subject, inheritedScope);
    if (!qr.childId)
        throw Error("ambient getListElem: resolver didn't return child id");
    Subject childSubject{DerivedSubject{
        .parent = std::make_shared<const Subject>(subject),
        .kind = DerivedSubject::Kind::GetListElem,
        .index = index,
    }};
    auto child = std::make_shared<AmbientObject>(std::move(childSubject), queryFn, ambientRootFSRoot, applyFn);
    /* Navigation child inherits parent's argCell cell directly. */
    child->withScope(argCell);
    child->withInheritedScope(inheritedScope);
    return child;
}

ObjectType AmbientObject::getTypeLazy()
{
    return getType();
}

ObjectType AmbientObject::getType()
{
    return stringToObjectType(whnf().type);
}

RootValue AmbientObject::defeatCache()
{
    throw Error("ambient defeatCache: not supported on virtual values");
}

RootValue AmbientObject::toValueOrProxy(EvalState & state, std::shared_ptr<AmbientResolver> resolver)
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

std::optional<FunctionInfo> AmbientObject::getFunctionInfo()
{
    auto scopeStateId = structuralAddressAfter(subject, inheritedScope, {});
    trace::QueryGetFunctionInfo q{std::string{}};
    stampPerArgFieldsAmbient(q, subject, inheritedScope);
    auto qr = queryFn(scopeStateId, q, subject, inheritedScope);
    auto * r = std::get_if<trace::ResultFunctionInfo>(&qr.result);
    if (!r || !r->hasInfo)
        return std::nullopt;
    return FunctionInfo{.formals = r->formals, .ellipsis = r->ellipsis};
}

PosIdx AmbientObject::getPos()
{
    return noPos;
}

std::optional<std::vector<std::string>> AmbientObject::getAttrPath()
{
    return std::nullopt;
}

std::shared_ptr<Object> AmbientObject::queryApply(std::shared_ptr<Object> argObj)
{
    if (!applyFn)
        throw Error("ambient apply: no apply callback");
    /* Thread the caller's effective scope into applyFn so the cb
       apply's new local cell can chain off the right depth, even
       when `resolve(fnId)` returns an InterpreterObject without a
       proxy parent chain. Keep a copy of argObj for the result's
       cell before moving it into applyFn. */
    auto callerScope = effectiveArgCell(*this);
    auto argForScope = argObj;
    /* Each value crossing into a cb-apply boundary starts fresh as
       a PositionalSeed at the apply's reverse-De-Bruijn depth — no
       inherited Subject is propagated, so observations at the
       boundary are predictable regardless of where the arg came
       from. Must match what `AmbientApply::run` computes for argId
       downstream so the registry's resultId and this proxy's argStateId
       for queryFn lookups agree. */
    int localDepth = callerScope ? callerScope->depth + 1 : 0;
    Subject argSubject{PositionalSeed{localDepth}};
    applyFn(structuralAddressAfter(subject, inheritedScope, {}), std::move(argObj), callerScope);
    Subject resultSubject{ApplyResultSubject{
        .fn = std::make_shared<const Subject>(subject),
        .arg = std::make_shared<const Subject>(std::move(argSubject)),
    }};
    auto result = std::make_shared<AmbientObject>(std::move(resultSubject), queryFn, ambientRootFSRoot, applyFn);
    /* Apply-result scope cell rooted at the caller's scope. */
    auto cell = ArgCell::make(callerScope, std::move(argForScope));
    result->withScope(std::move(cell));
    result->withInheritedScope(inheritedScope);
    return result;
}

} // namespace nix
