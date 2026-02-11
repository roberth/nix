#pragma once

#include "nix/expr/evaluator.hh"
#include "nix/util/ref.hh"

#include <optional>
#include <string>

namespace nix {

class TraceFile;

/**
 * Object wrapper that logs all operations to a trace file.
 */
class TracingObject : public Object
{
    ref<Object> inner;
    TraceFile & traceFile;
    uint64_t valueNum;

    TracingObject(ref<Object> inner, TraceFile & traceFile, uint64_t valueNum);

public:
    /**
     * Create a tracing object with the given value number.
     */
    static ref<TracingObject> create(ref<Object> inner, TraceFile & traceFile, uint64_t valueNum);

    std::shared_ptr<Object> maybeGetAttr(const std::string & name) override;
    std::vector<std::string> getAttrNames() override;
    std::string getStringIgnoreContext() override;
    std::pair<std::string, NixStringContext> getStringWithContext() override;
    SourcePath getPath() override;
    bool getBool(std::string_view errorCtx = "") override;
    NixInt getInt(std::string_view errorCtx = "") override;
    std::vector<std::string> getListOfStringsNoCtx() override;
    ObjectType getTypeLazy() override;
    ObjectType getType() override;
    RootValue defeatCache() override;
};

} // namespace nix
