#include "nix/expr/tracing-replay-object.hh"
#include "nix/expr/value/context.hh"
#include "nix/store/store-api.hh"
#include "nix/util/error.hh"
#include "nix/util/logging.hh"
#include "nix/util/util.hh"

namespace nix {

static ObjectType stringToObjectType(const std::string & type)
{
    if (type == "set")
        return nAttrs;
    if (type == "list")
        return nList;
    if (type == "string")
        return nString;
    if (type == "path")
        return nPath;
    if (type == "int")
        return nInt;
    if (type == "float")
        return nFloat;
    if (type == "bool")
        return nBool;
    if (type == "null")
        return nNull;
    if (type == "lambda")
        return nFunction;
    if (type == "thunk")
        return nThunk;
    if (type == "external")
        return nExternal;
    throw Error("unknown type in trace: %s", type);
}

TracingReplayObject::TracingReplayObject(
    Store & store,
    const std::vector<trace::TraceEntry> & trace,
    const trace::QueryIndex & index,
    uint64_t valueNum,
    std::function<ref<Object>()> getInner)
    : store(store)
    , trace(trace)
    , index(index)
    , valueNum(valueNum)
    , getInner(std::move(getInner))
{
}

ref<Object> TracingReplayObject::ensureInner() const
{
    if (!inner) {
        debug("replay fallback: activating inner for v=%d", valueNum);
        inner = getInner();
    }
    return *inner;
}

template<typename Q>
std::optional<typename trace::ResultOf<Q>::Type> TracingReplayObject::lookupResult(const Q & query) const
{
    auto entry = index.lookup(query);
    if (!entry) {
        debug("replay miss: %s (v=%d) not in index", Q::tag, valueNum);
        return std::nullopt;
    }

    using ResultPayload = typename trace::ResultOf<Q>::Type;
    auto * result = std::get_if<trace::Result<ResultPayload>>(&trace[entry->resultIndex]);
    if (!result) {
        debug("replay miss: %s result type mismatch at index %d", Q::tag, entry->resultIndex);
        return std::nullopt;
    }

    return result->result;
}

std::shared_ptr<Object> TracingReplayObject::maybeGetAttr(const std::string & name)
{
    trace::QueryGetAttr query{name, valueNum};
    auto entry = index.lookup(query);

    if (!entry) {
        debug("replay miss: getAttr '%s' from v=%d not in index", name, valueNum);
        return ensureInner()->maybeGetAttr(name);
    }

    // Check if the result indicates the attribute doesn't exist
    auto * result = std::get_if<trace::Result<trace::ResultMaybeType>>(&trace[entry->resultIndex]);
    if (result && !result->result.type) {
        debug("replay hit: getAttr '%s' from v=%d -> missing", name, valueNum);
        return nullptr;
    }

    // Get the query's v (result handle) for the child object
    auto * q = std::get_if<trace::Query<trace::QueryGetAttr>>(&trace[entry->queryIndex]);
    if (!q)
        return ensureInner()->maybeGetAttr(name);

    debug("replay hit: getAttr '%s' from v=%d -> v=%d", name, valueNum, q->v);
    return std::make_shared<TracingReplayObject>(
        store, trace, index, q->v, [this, name]() { return ref<Object>(ensureInner()->maybeGetAttr(name)); });
}

std::vector<std::string> TracingReplayObject::getAttrNames()
{
    if (auto r = lookupResult(trace::QueryGetAttrNames{valueNum}))
        return r->values;
    return ensureInner()->getAttrNames();
}

std::string TracingReplayObject::getStringIgnoreContext()
{
    if (auto r = lookupResult(trace::QueryGetString{valueNum}))
        return r->value;
    return ensureInner()->getStringIgnoreContext();
}

std::string TracingReplayObject::getStringWithoutContext()
{
    // Delegate to inner — this checks for empty context which the cache doesn't track
    return ensureInner()->getStringWithoutContext();
}

std::pair<std::string, NixStringContext> TracingReplayObject::getStringWithContext()
{
    if (auto r = lookupResult(trace::QueryGetStringWithContext{valueNum})) {
        NixStringContext ctx;
        for (const auto & s : r->context)
            ctx.insert(NixStringContextElem::parse(s));

        // Validate that all context paths still exist in the store
        bool valid = true;
        for (const auto & elem : ctx) {
            const StorePath & path = std::visit(
                overloaded{
                    [&](const NixStringContextElem::DrvDeep & d) -> const StorePath & { return d.drvPath; },
                    [&](const NixStringContextElem::Built & b) -> const StorePath & {
                        return b.drvPath->getBaseStorePath();
                    },
                    [&](const NixStringContextElem::Opaque & o) -> const StorePath & { return o.path; },
                },
                elem.raw);
            if (!store.isValidPath(path)) {
                debug("replay miss: context path %s no longer valid", store.printStorePath(path));
                valid = false;
                break;
            }
        }

        if (valid)
            return {r->value, std::move(ctx)};
    }
    return ensureInner()->getStringWithContext();
}

SourcePath TracingReplayObject::getPath()
{
    // Paths are not cached — always delegate
    return ensureInner()->getPath();
}

bool TracingReplayObject::getBool(std::string_view errorCtx)
{
    if (auto r = lookupResult(trace::QueryGetBool{valueNum}))
        return r->value;
    return ensureInner()->getBool(errorCtx);
}

NixInt TracingReplayObject::getInt(std::string_view errorCtx)
{
    if (auto r = lookupResult(trace::QueryGetInt{valueNum}))
        return NixInt{r->value};
    return ensureInner()->getInt(errorCtx);
}

NixFloat TracingReplayObject::getFloat(std::string_view errorCtx)
{
    if (auto r = lookupResult(trace::QueryGetFloat{valueNum}))
        return r->value;
    return ensureInner()->getFloat(errorCtx);
}

size_t TracingReplayObject::getListSize()
{
    if (auto r = lookupResult(trace::QueryGetListSize{valueNum}))
        return r->size;
    return ensureInner()->getListSize();
}

std::shared_ptr<Object> TracingReplayObject::getListElem(size_t idx)
{
    trace::QueryGetListElem query{valueNum, idx};
    auto entry = index.lookup(query);

    if (!entry)
        return ensureInner()->getListElem(idx);

    auto * q = std::get_if<trace::Query<trace::QueryGetListElem>>(&trace[entry->queryIndex]);
    if (!q)
        return ensureInner()->getListElem(idx);

    return std::make_shared<TracingReplayObject>(
        store, trace, index, q->v, [this, idx]() { return ref<Object>(ensureInner()->getListElem(idx)); });
}

std::vector<std::string> TracingReplayObject::getListOfStringsNoCtx()
{
    if (auto r = lookupResult(trace::QueryGetListOfStrings{valueNum}))
        return r->values;
    return ensureInner()->getListOfStringsNoCtx();
}

ObjectType TracingReplayObject::getTypeLazy()
{
    return getType();
}

ObjectType TracingReplayObject::getType()
{
    if (auto r = lookupResult(trace::QueryGetType{valueNum}))
        return stringToObjectType(r->type);
    return ensureInner()->getType();
}

RootValue TracingReplayObject::defeatCache()
{
    return ensureInner()->defeatCache();
}

std::optional<FunctionInfo> TracingReplayObject::getFunctionInfo()
{
    return ensureInner()->getFunctionInfo();
}

} // namespace nix
