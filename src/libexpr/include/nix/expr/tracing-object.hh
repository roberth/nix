#pragma once

#include "nix/expr/evaluator.hh"
#include "nix/expr/trace-sink.hh"
#include "nix/util/ref.hh"

#include <optional>
#include <string>

namespace nix {

/**
 * Object decorator that logs all value access operations to a TraceSink.
 * Each TracingObject has a value handle linking it to its query in the trace.
 */
class TracingObject : public Object
{
    ref<Object> inner;
    TraceSink & sink;
    uint64_t valueNum;

    TracingObject(ref<Object> inner, TraceSink & sink, uint64_t valueNum);

public:
    static ref<TracingObject> create(ref<Object> inner, TraceSink & sink, uint64_t valueNum);

    std::shared_ptr<Object> maybeGetAttr(const std::string & name) override;
    std::vector<std::string> getAttrNames() override;
    std::string getStringIgnoreContext() override;
    std::string getStringWithoutContext() override;
    std::pair<std::string, NixStringContext> getStringWithContext() override;
    SourcePath getPath() override;
    bool getBool(std::string_view errorCtx = "") override;
    NixInt getInt(std::string_view errorCtx = "") override;
    NixFloat getFloat(std::string_view errorCtx = "") override;
    size_t getListSize() override;
    std::shared_ptr<Object> getListElem(size_t index) override;
    std::vector<std::string> getListOfStringsNoCtx() override;
    ObjectType getTypeLazy() override;
    ObjectType getType() override;
    RootValue defeatCache() override;
    std::optional<FunctionInfo> getFunctionInfo() override;
    PosIdx getPos() override;
    std::optional<std::vector<std::string>> getAttrPath() override;
};

} // namespace nix
