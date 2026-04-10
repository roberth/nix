#pragma once
/**
 * @file
 * AmbientObject — Object backed by an ambient query callback.
 *
 * A value from the ambient (outer) evaluator, accessed by the local
 * (inner) evaluator through ambient queries. Each Object method issues
 * a query through the provided callback and interprets the response.
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
using AmbientQueryFn = std::function<trace::ResultVariant(const trace::QueryVariant &)>;

/**
 * Callback to register a local Object for use in ambient apply queries.
 * Returns the assigned local id.
 */
using AmbientRegisterLocalFn = std::function<std::string(std::shared_ptr<Object>)>;

/**
 * Object implementation backed by contra-queries to the outer evaluator.
 * Each method composes a Query, issues it via the callback, and
 * interprets the Result.
 */
class AmbientObject : public Object
{
    std::string id;        ///< Identity in the ambient query `from` field
    AmbientQueryFn queryFn; ///< Callback to issue ambient queries
    AmbientRegisterLocalFn registerLocal; ///< Callback to register local values (may be null)

public:
    AmbientObject(std::string id, AmbientQueryFn queryFn, AmbientRegisterLocalFn registerLocal = {});

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

    /**
     * Issue a QueryApply through the ambient query mechanism.
     * Returns an AmbientObject wrapping the result.
     */
    std::shared_ptr<Object> queryApply(const std::string & argId, std::shared_ptr<Object> argObj);

    const std::string & getId() const { return id; }
};

} // namespace nix
