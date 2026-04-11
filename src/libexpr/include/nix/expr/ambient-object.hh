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
#include "nix/expr/trace-ids.hh"
#include "nix/expr/trace-types.hh"

#include <functional>
#include <optional>
#include <string>

namespace nix {

/**
 * Response from an ambient query: the result plus an optional child id
 * for queries that produce child Objects (getAttr, getListElem, apply).
 */
struct AmbientQueryResult
{
    trace::ResultVariant result;
    std::optional<AmbientId> childId; // id of child Object in the resolver, if applicable
};

/**
 * Callback type for issuing ambient queries.
 * Takes the caller's Object id and a query, returns the result.
 */
using AmbientQueryFn = std::function<AmbientQueryResult(AmbientId objectId, const trace::QueryVariant &)>;

/**
 * Callback type for ambient function application.
 * Takes the function's Object id and the argument Object, returns
 * the result Object id.
 */
using AmbientApplyFn = std::function<AmbientId(AmbientId fnId, std::shared_ptr<Object> argObj)>;

/**
 * Object implementation backed by ambient queries to the outer evaluator.
 * Each method composes a Query, issues it via the callback, and
 * interprets the Result.
 */
class AmbientObject : public Object
{
    AmbientId id;             ///< Integer id in the resolver
    AmbientQueryFn queryFn;   ///< Callback to issue ambient queries
    AmbientApplyFn applyFn;   ///< Callback for function application (may be null)

public:
    AmbientObject(AmbientId id, AmbientQueryFn queryFn, AmbientApplyFn applyFn = {});

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
     * Issue a QueryApply. The resolver registers the arg and creates
     * the lazy application. Returns an AmbientObject wrapping the result.
     */
    std::shared_ptr<Object> queryApply(std::shared_ptr<Object> argObj);

    AmbientId getId() const { return id; }
};

} // namespace nix
