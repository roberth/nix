#include "nix/expr/tracing-local-object.hh"
#include "nix/expr/object-type.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-decision-graph.hh"
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

TracingLocalObject::TracingLocalObject(
    std::shared_ptr<Object> inner,
    cidasks::Subject subject_,
    TracingWriter & writer,
    ref<SourceRoot> rootFSRoot,
    std::shared_ptr<const ArgScopeCell> argScope,
    Hash inheritedScope_,
    Hash depth2ApplyId_)
    : inner(std::move(inner))
    , subject(std::move(subject_))
    , inheritedScope(std::move(inheritedScope_))
    , depth2ApplyId(std::move(depth2ApplyId_))
    , writer(writer)
    , rootFSRoot(std::move(rootFSRoot))
    , argScope(std::move(argScope))
{
}

std::shared_ptr<Object> TracingLocalObject::maybeGetAttr(const std::string & name)
{
    auto child = inner->maybeGetAttr(name);
    trace::QueryGetAttr query{name, tracingLocalFromOf(localId())};
    auto resultJson = child
        ? trace::ResultMaybeType{std::optional{objectTypeToString(child->getType())}}
        : trace::ResultMaybeType{std::nullopt};
    recordObservation(query, resultJson);
    if (!child)
        return nullptr;
    cidasks::Subject childSubject{cidasks::DerivedSubject{
        .parent = std::make_shared<const cidasks::Subject>(subject),
        .kind = cidasks::DerivedSubject::Kind::GetAttr,
        .name = name,
    }};
    return std::make_shared<TracingLocalObject>(
        std::move(child), std::move(childSubject), writer, rootFSRoot, argScope, inheritedScope, depth2ApplyId);
}

std::vector<std::string> TracingLocalObject::getAttrNames()
{
    auto names = inner->getAttrNames();
    recordObservation(
        trace::QueryGetAttrNames{tracingLocalFromOf(localId())}, trace::ResultListOfStrings{names});
    return names;
}

std::string TracingLocalObject::getStringIgnoreContext()
{
    auto value = inner->getStringIgnoreContext();
    recordObservation(trace::QueryGetString{tracingLocalFromOf(localId())}, trace::ResultString{value});
    return value;
}

std::string TracingLocalObject::getStringWithoutContext()
{
    return getStringIgnoreContext();
}

std::pair<std::string, NixStringContext> TracingLocalObject::getStringWithContext()
{
    auto [str, ctx] = inner->getStringWithContext();
    std::vector<std::string> ctxStrings;
    for (auto & c : ctx)
        ctxStrings.push_back(c.to_string());
    recordObservation(
        trace::QueryGetStringWithContext{tracingLocalFromOf(localId())},
        trace::ResultStringWithContext{str, std::move(ctxStrings)});
    return {str, std::move(ctx)};
}

RootedPath TracingLocalObject::getPath()
{
    auto path = inner->getPath();
    recordObservation(trace::QueryGetPath{tracingLocalFromOf(localId())}, trace::ResultPath{path.path.abs()});
    /* lazy-paths: reuse the cached SourceRoot so the path outlives the
       returned RootedPath. */
    return RootedPath{rootFSRoot, path.path};
}

bool TracingLocalObject::getBool(std::string_view)
{
    auto value = inner->getBool();
    recordObservation(trace::QueryGetBool{tracingLocalFromOf(localId())}, trace::ResultBool{value});
    return value;
}

NixInt TracingLocalObject::getInt(std::string_view)
{
    auto value = inner->getInt();
    recordObservation(trace::QueryGetInt{tracingLocalFromOf(localId())}, trace::ResultInt{value.value});
    return value;
}

NixFloat TracingLocalObject::getFloat(std::string_view)
{
    auto value = inner->getFloat();
    recordObservation(trace::QueryGetFloat{tracingLocalFromOf(localId())}, trace::ResultFloat{value});
    return value;
}

size_t TracingLocalObject::getListSize()
{
    auto size = inner->getListSize();
    recordObservation(trace::QueryGetListSize{tracingLocalFromOf(localId())}, trace::ResultListSize{size});
    return size;
}

std::shared_ptr<Object> TracingLocalObject::getListElem(size_t index)
{
    auto child = inner->getListElem(index);
    trace::QueryGetListElem query{tracingLocalFromOf(localId()), index};
    recordObservation(query, trace::ResultType{objectTypeToString(child->getType())});
    cidasks::Subject childSubject{cidasks::DerivedSubject{
        .parent = std::make_shared<const cidasks::Subject>(subject),
        .kind = cidasks::DerivedSubject::Kind::GetListElem,
        .index = index,
    }};
    return std::make_shared<TracingLocalObject>(
        std::move(child), std::move(childSubject), writer, rootFSRoot, argScope, inheritedScope, depth2ApplyId);
}

ObjectType TracingLocalObject::getTypeLazy()
{
    return getType();
}

ObjectType TracingLocalObject::getType()
{
    auto type = inner->getType();
    trace::QueryGetType q{tracingLocalFromOf(localId())};
    recordObservation(q, trace::ResultType{objectTypeToString(type)});
    auto reqHash = TracingDecisionGraph::computeQueryHash(q);
    tracingCacheLog("tlo: getType from=%s reqHash=%s type=%s",
        tracingLocalFromOf(localId()).substr(0, 12),
        reqHash.to_string(HashFormat::Base16, false).substr(0, 12),
        objectTypeToString(type));
    return type;
}

RootValue TracingLocalObject::defeatCache()
{
    /* Pass through unrecorded. defeatCache yields a concrete RootValue
       (no observable side effects), and there's no incoming-Fact shape
       for "I gave you my underlying value." */
    return inner->defeatCache();
}

std::optional<FunctionInfo> TracingLocalObject::getFunctionInfo()
{
    auto info = inner->getFunctionInfo();
    trace::ResultFunctionInfo rfi{
        info.has_value(), info ? info->formals : std::map<std::string, bool>{}, info ? info->ellipsis : false};
    recordObservation(trace::QueryGetFunctionInfo{tracingLocalFromOf(localId())}, rfi);
    return info;
}

PosIdx TracingLocalObject::getPos()
{
    return inner->getPos();
}

std::optional<std::vector<std::string>> TracingLocalObject::getAttrPath()
{
    return inner->getAttrPath();
}

void TracingLocalObject::recordObservation(const trace::QueryVariant & query, const trace::ResultVariant & result)
{
    /* Route through the depth-2 entry point: the outer is probing
       an inner-supplied local. The `depth2ApplyId` groups this fact
       into the cb apply's AmbientAsks edge at flush. */
    writer.logDepth2Observation(query, result, subject, inheritedScope, depth2ApplyId);
}

std::shared_ptr<Object> TracingLocalObject::queryApply(std::shared_ptr<Object> argObj)
{
    /* Delegate the apply itself to the wrapped inner Object. For an
       inner-supplied lambda (the cb-higher-order case) `inner` is an
       InterpreterObject whose queryApply does mkApp + bridging. For
       a replay-time ReplayLocalObject standin, inner->queryApply
       throws "can't validate" (= the depth-2 divergence signal).

       The result wrapper carries an ApplyResultSubject so accesses
       on the apply result continue to be recorded in the depth-2
       trace with an evolved scopeStateId (per the cidasks design). */
    auto argCdiHex = argObj->getScopeStateIdHex();
    cidasks::Subject argSubject = argObj->getSubject()
        ? *argObj->getSubject()
        : cidasks::Subject{cidasks::PostulatedIdempotentRead{
              argCdiHex
                  ? Hash::parseNonSRIUnprefixed(*argCdiHex, HashAlgorithm::SHA256)
                  : Hash{HashAlgorithm::SHA256}}};
    auto result = inner->queryApply(argObj);
    cidasks::Subject resultSubject{cidasks::ApplyResultSubject{
        .fn = std::make_shared<const cidasks::Subject>(subject),
        .arg = std::make_shared<const cidasks::Subject>(std::move(argSubject)),
    }};
    return std::make_shared<TracingLocalObject>(
        std::move(result), std::move(resultSubject), writer, rootFSRoot, argScope, inheritedScope, depth2ApplyId);
}

} // namespace nix
