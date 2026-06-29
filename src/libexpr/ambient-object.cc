#include "nix/expr/ambient-object.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/expr-from-object.hh"
#include "nix/expr/object-type.hh"
#include "nix/util/source-accessor.hh"

namespace nix {

/* Under Step C, AmbientId is a Hash. The wire format puts the
   hex representation in the query's `from` field. */
static std::string fromOf(AmbientId scopeStateId)
{
    return scopeStateId.to_string(HashFormat::Base16, false);
}

/* Populate `q`'s per-arg fields (from, path, fromCIDs) so its
   reqHash matches what the writer flushed for the corresponding
   observation. Returns the first-root CDI (= fromCIDs[0]) so callers
   can pass it where the legacy single-root rootCdi was expected. */
template <typename Q>
static Hash stampPerArgFieldsAmbient(Q & q, const cidasks::Subject & subject, const Hash & inheritedScope)
{
    auto par = cidasks::pathAndRootsFromSubject(subject);
    std::vector<trace::QueryLeaf> fromCIDs;
    fromCIDs.reserve(par.roots.size());
    Hash rootCdi(HashAlgorithm::SHA256);
    for (size_t i = 0; i < par.roots.size(); ++i) {
        auto cid = cidasks::scopeStateIdAfter(par.roots[i], inheritedScope, {});
        if (i == 0)
            rootCdi = cid;
        fromCIDs.emplace_back(cid.to_string(HashFormat::Base16, false));
    }
    q.from = fromCIDs.empty() ? trace::QueryLeaf{std::string{}} : fromCIDs[0];
    q.path = std::move(par.path);
    q.fromCIDs = std::move(fromCIDs);
    return rootCdi;
}

AmbientObject::AmbientObject(
    cidasks::Subject subject_, AmbientQueryFn queryFn, ref<SourceRoot> ambientRootFSRoot, AmbientApplyFn applyFn)
    : subject(std::move(subject_))
    , inheritedScope(HashAlgorithm::SHA256)
    , queryFn(std::move(queryFn))
    , applyFn(std::move(applyFn))
    , ambientRootFSRoot(std::move(ambientRootFSRoot))
{
}

std::shared_ptr<Object> AmbientObject::maybeGetAttr(const std::string & name)
{
    auto scopeStateId = cidasks::structuralAddressAfter(subject, inheritedScope, {});
    trace::QueryGetAttr q{name, std::string{}};
    auto rootCdi = stampPerArgFieldsAmbient(q, subject, inheritedScope);
    auto qr = queryFn(scopeStateId, q, subject, inheritedScope);
    auto * r = std::get_if<trace::ResultMaybeType>(&qr.result);
    if (!r || !r->type)
        return nullptr;
    if (!qr.childId)
        throw Error("ambient maybeGetAttr: resolver didn't return child id");
    cidasks::Subject childSubject{cidasks::DerivedSubject{
        .parent = std::make_shared<const cidasks::Subject>(subject),
        .kind = cidasks::DerivedSubject::Kind::GetAttr,
        .name = name,
    }};
    auto child = std::make_shared<AmbientObject>(std::move(childSubject), queryFn, ambientRootFSRoot, applyFn);
    /* Navigation child inherits parent's argScope cell directly. */
    child->withScope(argScope);
    /* Inherit content-id scope so the child's `from` fields include
       the same CDI(Q) the parent uses. */
    child->withInheritedScope(inheritedScope);
    return child;
}

std::vector<std::string> AmbientObject::getAttrNames()
{
    auto scopeStateId = cidasks::structuralAddressAfter(subject, inheritedScope, {});
    trace::QueryGetAttrNames q{std::string{}};
    auto rootCdi = stampPerArgFieldsAmbient(q, subject, inheritedScope);
    auto qr = queryFn(scopeStateId, q, subject, inheritedScope);
    auto * r = std::get_if<trace::ResultListOfStrings>(&qr.result);
    if (!r)
        throw Error("ambient getAttrNames: unexpected result type");
    return r->values;
}

std::string AmbientObject::getStringIgnoreContext()
{
    auto scopeStateId = cidasks::structuralAddressAfter(subject, inheritedScope, {});
    trace::QueryGetString q{std::string{}};
    auto rootCdi = stampPerArgFieldsAmbient(q, subject, inheritedScope);
    auto qr = queryFn(scopeStateId, q, subject, inheritedScope);
    auto * r = std::get_if<trace::ResultString>(&qr.result);
    if (!r)
        throw Error("ambient getString: unexpected result type");
    return r->value;
}

std::string AmbientObject::getStringWithoutContext()
{
    return getStringIgnoreContext();
}

std::pair<std::string, NixStringContext> AmbientObject::getStringWithContext()
{
    auto scopeStateId = cidasks::structuralAddressAfter(subject, inheritedScope, {});
    trace::QueryGetStringWithContext q{std::string{}};
    auto rootCdi = stampPerArgFieldsAmbient(q, subject, inheritedScope);
    auto qr = queryFn(scopeStateId, q, subject, inheritedScope);
    auto * r = std::get_if<trace::ResultStringWithContext>(&qr.result);
    if (!r)
        throw Error("ambient getStringWithContext: unexpected result type");
    NixStringContext ctx;
    for (auto & s : r->context)
        ctx.insert(NixStringContextElem::parse(s));
    return {r->value, std::move(ctx)};
}

RootedPath AmbientObject::getPath()
{
    auto scopeStateId = cidasks::structuralAddressAfter(subject, inheritedScope, {});
    trace::QueryGetPath q{std::string{}};
    auto rootCdi = stampPerArgFieldsAmbient(q, subject, inheritedScope);
    auto qr = queryFn(scopeStateId, q, subject, inheritedScope);
    auto * r = std::get_if<trace::ResultPath>(&qr.result);
    if (!r)
        throw Error("ambient getPath: unexpected result type");
    /* lazy-paths: reuse the outer EvalState's `rootFSRoot` so the
       SourceRoot outlives the Value the outer evaluator constructs
       from this path. A one-off `SourceRoot::make` here would be
       freed when the returned RootedPath drops, leaving Value's raw
       SourceRoot pointer dangling. */
    return RootedPath{ambientRootFSRoot, CanonPath(r->path)};
}

bool AmbientObject::getBool(std::string_view)
{
    auto scopeStateId = cidasks::structuralAddressAfter(subject, inheritedScope, {});
    trace::QueryGetBool q{std::string{}};
    auto rootCdi = stampPerArgFieldsAmbient(q, subject, inheritedScope);
    auto qr = queryFn(scopeStateId, q, subject, inheritedScope);
    auto * r = std::get_if<trace::ResultBool>(&qr.result);
    if (!r)
        throw Error("ambient getBool: unexpected result type");
    return r->value;
}

NixInt AmbientObject::getInt(std::string_view)
{
    auto scopeStateId = cidasks::structuralAddressAfter(subject, inheritedScope, {});
    trace::QueryGetInt q{std::string{}};
    auto rootCdi = stampPerArgFieldsAmbient(q, subject, inheritedScope);
    auto qr = queryFn(scopeStateId, q, subject, inheritedScope);
    auto * r = std::get_if<trace::ResultInt>(&qr.result);
    if (!r)
        throw Error("ambient getInt: unexpected result type");
    return NixInt{r->value};
}

NixFloat AmbientObject::getFloat(std::string_view)
{
    auto scopeStateId = cidasks::structuralAddressAfter(subject, inheritedScope, {});
    trace::QueryGetFloat q{std::string{}};
    auto rootCdi = stampPerArgFieldsAmbient(q, subject, inheritedScope);
    auto qr = queryFn(scopeStateId, q, subject, inheritedScope);
    auto * r = std::get_if<trace::ResultFloat>(&qr.result);
    if (!r)
        throw Error("ambient getFloat: unexpected result type");
    return r->value;
}

size_t AmbientObject::getListSize()
{
    auto scopeStateId = cidasks::structuralAddressAfter(subject, inheritedScope, {});
    trace::QueryGetListSize q{std::string{}};
    auto rootCdi = stampPerArgFieldsAmbient(q, subject, inheritedScope);
    auto qr = queryFn(scopeStateId, q, subject, inheritedScope);
    auto * r = std::get_if<trace::ResultListSize>(&qr.result);
    if (!r)
        throw Error("ambient getListSize: unexpected result type");
    return r->size;
}

std::shared_ptr<Object> AmbientObject::getListElem(size_t index)
{
    auto scopeStateId = cidasks::structuralAddressAfter(subject, inheritedScope, {});
    trace::QueryGetListElem q{std::string{}, index};
    auto rootCdi = stampPerArgFieldsAmbient(q, subject, inheritedScope);
    auto qr = queryFn(scopeStateId, q, subject, inheritedScope);
    if (!qr.childId)
        throw Error("ambient getListElem: resolver didn't return child id");
    cidasks::Subject childSubject{cidasks::DerivedSubject{
        .parent = std::make_shared<const cidasks::Subject>(subject),
        .kind = cidasks::DerivedSubject::Kind::GetListElem,
        .index = index,
    }};
    auto child = std::make_shared<AmbientObject>(std::move(childSubject), queryFn, ambientRootFSRoot, applyFn);
    /* Navigation child inherits parent's argScope cell directly. */
    child->withScope(argScope);
    child->withInheritedScope(inheritedScope);
    return child;
}

ObjectType AmbientObject::getTypeLazy()
{
    return getType();
}

ObjectType AmbientObject::getType()
{
    auto scopeStateId = cidasks::structuralAddressAfter(subject, inheritedScope, {});
    trace::QueryGetType q{std::string{}};
    auto rootCdi = stampPerArgFieldsAmbient(q, subject, inheritedScope);
    auto qr = queryFn(scopeStateId, q, subject, inheritedScope);
    auto * r = std::get_if<trace::ResultType>(&qr.result);
    if (!r)
        throw Error("ambient getType: unexpected result type");
    return stringToObjectType(r->type);
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
    auto scopeStateId = cidasks::structuralAddressAfter(subject, inheritedScope, {});
    trace::QueryGetFunctionInfo q{std::string{}};
    auto rootCdi = stampPerArgFieldsAmbient(q, subject, inheritedScope);
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
    auto callerScope = effectiveArgScope(*this);
    auto argForScope = argObj;
    /* Each value crossing into a cb-apply boundary starts fresh as
       a PositionalSeed at the apply's reverse-De-Bruijn depth — no
       inherited Subject is propagated, so observations at the
       boundary are predictable regardless of where the arg came
       from. Must match what `AmbientApply::run` computes for argId
       downstream so the registry's resultId and this proxy's CDI
       for queryFn lookups agree. */
    int localDepth = callerScope ? callerScope->depth + 1 : 0;
    cidasks::Subject argSubject{cidasks::PositionalSeed{localDepth}};
    applyFn(cidasks::structuralAddressAfter(subject, inheritedScope, {}), std::move(argObj), callerScope);
    cidasks::Subject resultSubject{cidasks::ApplyResultSubject{
        .fn = std::make_shared<const cidasks::Subject>(subject),
        .arg = std::make_shared<const cidasks::Subject>(std::move(argSubject)),
    }};
    auto result = std::make_shared<AmbientObject>(std::move(resultSubject), queryFn, ambientRootFSRoot, applyFn);
    /* Apply-result scope cell rooted at the caller's scope. */
    auto cell = ArgScopeCell::make(callerScope, std::move(argForScope));
    result->withScope(std::move(cell));
    result->withInheritedScope(inheritedScope);
    return result;
}

} // namespace nix
