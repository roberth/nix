#include "nix/expr/tracing-callback-arg.hh"
#include "nix/expr/object-type.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-object.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/util/source-accessor.hh"

namespace nix {

/* Same-name helper exists in ambient-object.cc; both translation
   units are unity-built into libnixexpr, so the helpers must have
   distinct names to avoid ODR collisions. */
static std::string tracingLocalFromOf(AmbientId id)
{
    return id.to_string(HashFormat::Base16, false);
}

TracingCallbackArg::TracingCallbackArg(
    std::shared_ptr<Object> inner,
    Subject subject_,
    TracingWriter & writer,
    ref<SourceRoot> rootFSRoot,
    std::shared_ptr<const ArgCell> argCell,
    Hash inheritedScope_,
    Hash ambientApplyId_)
    : inner(std::move(inner))
    , subject(std::move(subject_))
    , argAncestry(std::move(inheritedScope_))
    , ambientApplyId(std::move(ambientApplyId_))
    , writer(writer)
    , rootFSRoot(std::move(rootFSRoot))
    , argCell(std::move(argCell))
{
}

std::shared_ptr<Object> TracingCallbackArg::maybeGetAttr(const std::string & name)
{
    auto child = inner->maybeGetAttr(name);
    trace::QueryGetAttr query{name, tracingLocalFromOf(localId())};
    auto resultJson = child
        ? trace::ResultMaybeType{std::optional{objectTypeToString(child->getType())}}
        : trace::ResultMaybeType{std::nullopt};
    recordObservation(query, resultJson);
    if (!child)
        return nullptr;
    Subject childSubject{DerivedSubject{
        .parent = std::make_shared<const Subject>(subject),
        .kind = DerivedSubject::Kind::GetAttr,
        .name = name,
    }};
    return std::make_shared<TracingCallbackArg>(
        std::move(child), std::move(childSubject), writer, rootFSRoot, argCell, argAncestry, ambientApplyId);
}

trace::ResultWHNF & TracingCallbackArg::whnf()
{
    if (cachedWHNF)
        return *cachedWHNF;
    auto whnfResult = computeWHNFFromObject(*inner);
    recordObservation(
        trace::QueryGetWHNF{tracingLocalFromOf(localId())},
        whnfResult);
    cachedWHNF = std::move(whnfResult);
    return *cachedWHNF;
}

std::vector<std::string> TracingCallbackArg::getAttrNames()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFAttrs>(&w.payload);
    if (!p)
        throw Error("tlo getAttrNames: WHNF payload not attrs (type %s)", w.type);
    return p->names;
}

std::string TracingCallbackArg::getStringIgnoreContext()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFString>(&w.payload);
    if (!p)
        throw Error("tlo getStringIgnoreContext: WHNF payload not string (type %s)", w.type);
    return p->value;
}

std::string TracingCallbackArg::getStringWithoutContext()
{
    return getStringIgnoreContext();
}

std::pair<std::string, NixStringContext> TracingCallbackArg::getStringWithContext()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFString>(&w.payload);
    if (!p)
        throw Error("tlo getStringWithContext: WHNF payload not string (type %s)", w.type);
    NixStringContext ctx;
    for (auto & s : p->context)
        ctx.insert(NixStringContextElem::parse(s));
    return {p->value, std::move(ctx)};
}

RootedPath TracingCallbackArg::getPath()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFPath>(&w.payload);
    if (!p)
        throw Error("tlo getPath: WHNF payload not path (type %s)", w.type);
    /* lazy-paths: reuse the cached SourceRoot so the path outlives the
       returned RootedPath. */
    return RootedPath{rootFSRoot, CanonPath{p->path}};
}

bool TracingCallbackArg::getBool(std::string_view)
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFBool>(&w.payload);
    if (!p)
        throw Error("tlo getBool: WHNF payload not bool (type %s)", w.type);
    return p->value;
}

NixInt TracingCallbackArg::getInt(std::string_view)
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFInt>(&w.payload);
    if (!p)
        throw Error("tlo getInt: WHNF payload not int (type %s)", w.type);
    return NixInt{p->value};
}

NixFloat TracingCallbackArg::getFloat(std::string_view)
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFFloat>(&w.payload);
    if (!p)
        throw Error("tlo getFloat: WHNF payload not float (type %s)", w.type);
    return p->value;
}

size_t TracingCallbackArg::getListSize()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFList>(&w.payload);
    if (!p)
        throw Error("tlo getListSize: WHNF payload not list (type %s)", w.type);
    return p->size;
}

std::shared_ptr<Object> TracingCallbackArg::getListElem(size_t index)
{
    auto child = inner->getListElem(index);
    trace::QueryGetListElem query{tracingLocalFromOf(localId()), index};
    recordObservation(query, trace::ResultType{objectTypeToString(child->getType())});
    Subject childSubject{DerivedSubject{
        .parent = std::make_shared<const Subject>(subject),
        .kind = DerivedSubject::Kind::GetListElem,
        .index = index,
    }};
    return std::make_shared<TracingCallbackArg>(
        std::move(child), std::move(childSubject), writer, rootFSRoot, argCell, argAncestry, ambientApplyId);
}

ObjectType TracingCallbackArg::getTypeLazy()
{
    return getType();
}

ObjectType TracingCallbackArg::getType()
{
    auto type = stringToObjectType(whnf().type);
    tracingCacheLog("tlo: getType from=%s type=%s",
        tracingLocalFromOf(localId()).substr(0, 12),
        objectTypeToString(type));
    return type;
}

RootValue TracingCallbackArg::defeatCache()
{
    /* Pass through unrecorded. defeatCache yields a concrete RootValue
       (no observable side effects), and there's no incoming-Fact shape
       for "I gave you my underlying value." */
    return inner->defeatCache();
}

std::optional<FunctionInfo> TracingCallbackArg::getFunctionInfo()
{
    auto info = inner->getFunctionInfo();
    trace::ResultFunctionInfo rfi{
        info.has_value(), info ? info->formals : std::map<std::string, bool>{}, info ? info->ellipsis : false};
    recordObservation(trace::QueryGetFunctionInfo{tracingLocalFromOf(localId())}, rfi);
    return info;
}

PosIdx TracingCallbackArg::getPos()
{
    return inner->getPos();
}

std::optional<std::vector<std::string>> TracingCallbackArg::getAttrPath()
{
    return inner->getAttrPath();
}

void TracingCallbackArg::recordObservation(const trace::QueryVariant & query, const trace::ResultVariant & result)
{
    /* Route through the ambient layer entry point: the outer is probing
       an inner-supplied local. The `ambientApplyId` groups this fact
       into the cb apply's AmbientAsks edge at flush. */
    writer.logAmbientObservation(query, result, subject, argAncestry, ambientApplyId);
}

std::shared_ptr<Object> TracingCallbackArg::queryApply(std::shared_ptr<Object> argObj)
{
    /* Delegate the apply itself to the wrapped inner Object. For an
       inner-supplied lambda (the cb-higher-order case) `inner` is an
       InterpreterObject whose queryApply does mkApp + bridging. For
       a replay-time ReplayCallbackArg standin, inner->queryApply
       throws "can't validate" — the ambient-layer divergence
       exception the walker turns into a cache miss.

       The result wrapper carries an ApplyResultSubject so accesses
       on the apply result continue to be recorded in the ambient layer
       trace with an evolved state hash (per the subject-id design). */
    auto argCdiHex = argObj->getStateHashHex();
    Subject argId = argObj->getSubject()
        ? *argObj->getSubject()
        : Subject{PostulatedIdempotentRead{
              argCdiHex
                  ? Hash::parseNonSRIUnprefixed(*argCdiHex, HashAlgorithm::SHA256)
                  : Hash{HashAlgorithm::SHA256}}};
    auto result = inner->queryApply(argObj);
    Subject resultSubject{ApplyResultSubject{
        .fn = std::make_shared<const Subject>(subject),
        .arg = std::make_shared<const Subject>(std::move(argId)),
    }};
    return std::make_shared<TracingCallbackArg>(
        std::move(result), std::move(resultSubject), writer, rootFSRoot, argCell, argAncestry, ambientApplyId);
}

} // namespace nix
