#pragma once
/**
 * @file
 * ContraObject — Object backed by a contra-query callback.
 *
 * A virtual value provided by the outer evaluator, accessed by the inner
 * evaluator through contra-queries. Each Object method issues a
 * ContraQuery Request through the provided callback and interprets the
 * ContraResult Response.
 */

#include "nix/expr/evaluator.hh"
#include "nix/expr/trace-types.hh"

#include <functional>
#include <string>

namespace nix {

/**
 * Callback type for issuing contra-queries.
 * Takes a query, returns the result. The implementation is responsible
 * for recording the interaction in the trace.
 */
using ContraQueryFn = std::function<trace::ResultVariant(const trace::QueryVariant &)>;

/**
 * Object implementation backed by contra-queries to the outer evaluator.
 * Each method composes a Query, issues it via the callback, and
 * interprets the Result.
 */
class ContraObject : public Object
{
    std::string id;        ///< Identity in the contra-query `from` field
    ContraQueryFn queryFn; ///< Callback to issue contra-queries

public:
    ContraObject(std::string id, ContraQueryFn queryFn);

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
    ObjectType getTypeLazy() override;
    ObjectType getType() override;
    RootValue defeatCache() override;
    std::optional<FunctionInfo> getFunctionInfo() override;
    PosIdx getPos() override;
    std::optional<std::vector<std::string>> getAttrPath() override;
};

} // namespace nix
