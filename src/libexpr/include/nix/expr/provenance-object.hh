#pragma once
/**
 * @file
 * ProvenanceObject - Object wrapper that tracks navigation provenance for position lookup.
 */

#include "nix/expr/evaluator.hh"

namespace nix {

/**
 * Object wrapper that tracks how we navigated to this object.
 *
 * Stores parent + attribute name so that getPos() can compute the
 * attribute binding position lazily (by defeating parent's cache).
 *
 * Use this when you need position information for error reporting.
 */
class ProvenanceObject : public Object
{
    ref<Object> inner;
    std::shared_ptr<ProvenanceObject> parent; // null for root
    std::string attrName;                     // empty for root
    EvalState & state;                        // needed for symbol lookup in getPos

public:
    /**
     * Create a root ProvenanceObject (no parent, no position).
     */
    ProvenanceObject(ref<Object> inner, EvalState & state);

    /**
     * Create a ProvenanceObject with parent tracking.
     */
    ProvenanceObject(
        ref<Object> inner, std::shared_ptr<ProvenanceObject> parent, std::string attrName, EvalState & state);

    // --- Object interface (delegates to inner) ---

    std::shared_ptr<Object> maybeGetAttr(const std::string & name) override;

    std::vector<std::string> getAttrNames() override;

    std::string getStringIgnoreContext() override;

    std::pair<std::string, NixStringContext> getStringWithContext() override;

    std::string getStringWithoutContext() override;

    SourcePath getPath() override;

    bool getBool(std::string_view errorCtx) override;

    NixInt getInt(std::string_view errorCtx) override;

    NixFloat getFloat(std::string_view errorCtx) override;

    size_t getListSize() override;

    std::shared_ptr<Object> getListElem(size_t index) override;

    std::vector<std::string> getListOfStringsNoCtx() override;

    ObjectType getTypeLazy() override;

    ObjectType getType() override;

    RootValue defeatCache() override;

    std::optional<FunctionInfo> getFunctionInfo() override;

    // --- Provenance ---

    /**
     * Compute position lazily by looking up attrName in parent's attrs.
     * Defeats parent's cache if needed.
     */
    PosIdx getPos() override;

    /**
     * Get the inner Object (for direct access when provenance not needed).
     */
    ref<Object> getInner()
    {
        return inner;
    }
};

/**
 * Evaluator wrapper that automatically wraps returned Objects in ProvenanceObject.
 *
 * Use this to get automatic position tracking for all evaluated expressions.
 */
class ProvenanceEvaluator : public Evaluator
{
    ref<Evaluator> inner;

    ref<Object> wrap(ref<Object> obj);

public:
    ProvenanceEvaluator(ref<Evaluator> inner);

    bool isReadOnly() const override;
    Store & getStore() override;
    const fetchers::Settings & getFetchSettings() override;
    ref<Object> evalFile(const SourcePath & path, const std::string & displayPath) override;
    ref<Object> evalExpr(const std::string & expr, const SourcePath & basePath) override;
    ref<Object> evalExprLazy(const std::string & expr, const SourcePath & basePath) override;
    ref<Object> mkString(const std::string & s) override;
    ref<Object> mkAttrs(const std::map<std::string, ref<Object>> & attrs) override;
    ref<Object> apply(ref<Object> fn, ref<Object> arg) override;
    EvalState & getEvalState() override;
};

} // namespace nix
