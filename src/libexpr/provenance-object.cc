#include "nix/expr/provenance-object.hh"
#include "nix/expr/eval.hh"

namespace nix {

ProvenanceObject::ProvenanceObject(ref<Object> inner, EvalState & state)
    : inner(inner)
    , parent(nullptr)
    , attrName()
    , state(state)
{
}

ProvenanceObject::ProvenanceObject(
    ref<Object> inner, std::shared_ptr<ProvenanceObject> parent, std::string attrName, EvalState & state)
    : inner(inner)
    , parent(parent)
    , attrName(std::move(attrName))
    , state(state)
{
}

std::shared_ptr<Object> ProvenanceObject::maybeGetAttr(const std::string & name)
{
    auto child = inner->maybeGetAttr(name);
    if (!child)
        return nullptr;
    // Wrap in ProvenanceObject tracking how we got there
    return std::make_shared<ProvenanceObject>(
        ref<Object>(child), std::dynamic_pointer_cast<ProvenanceObject>(shared_from_this()), name, state);
}

std::vector<std::string> ProvenanceObject::getAttrNames()
{
    return inner->getAttrNames();
}

std::string ProvenanceObject::getStringIgnoreContext()
{
    return inner->getStringIgnoreContext();
}

std::pair<std::string, NixStringContext> ProvenanceObject::getStringWithContext()
{
    return inner->getStringWithContext();
}

std::string ProvenanceObject::getStringWithoutContext()
{
    return inner->getStringWithoutContext();
}

SourcePath ProvenanceObject::getPath()
{
    return inner->getPath();
}

bool ProvenanceObject::getBool(std::string_view errorCtx)
{
    return inner->getBool(errorCtx);
}

NixInt ProvenanceObject::getInt(std::string_view errorCtx)
{
    return inner->getInt(errorCtx);
}

NixFloat ProvenanceObject::getFloat(std::string_view errorCtx)
{
    return inner->getFloat(errorCtx);
}

size_t ProvenanceObject::getListSize()
{
    return inner->getListSize();
}

std::shared_ptr<Object> ProvenanceObject::getListElem(size_t index)
{
    // Note: list elements don't have attribute positions
    auto elem = inner->getListElem(index);
    if (!elem)
        return nullptr;
    // Wrap but with no parent tracking (no position for list elements)
    return std::make_shared<ProvenanceObject>(ref<Object>(elem), state);
}

std::vector<std::string> ProvenanceObject::getListOfStringsNoCtx()
{
    return inner->getListOfStringsNoCtx();
}

ObjectType ProvenanceObject::getTypeLazy()
{
    return inner->getTypeLazy();
}

ObjectType ProvenanceObject::getType()
{
    return inner->getType();
}

RootValue ProvenanceObject::defeatCache()
{
    return inner->defeatCache();
}

std::optional<FunctionInfo> ProvenanceObject::getFunctionInfo()
{
    return inner->getFunctionInfo();
}

PosIdx ProvenanceObject::getPos()
{
    if (!parent || attrName.empty())
        return noPos;

    // Defeat parent's cache to get the actual Value with attr positions
    auto parentValue = parent->inner->defeatCache();
    if ((*parentValue)->type() != nAttrs)
        return noPos;

    auto attr = (*parentValue)->attrs()->get(state.symbols.create(attrName));
    return attr ? attr->pos : noPos;
}

// --- ProvenanceEvaluator ---

ProvenanceEvaluator::ProvenanceEvaluator(ref<Evaluator> inner)
    : inner(inner)
{
}

ref<Object> ProvenanceEvaluator::wrap(ref<Object> obj)
{
    return make_ref<ProvenanceObject>(obj, inner->getEvalState());
}

bool ProvenanceEvaluator::isReadOnly() const
{
    return inner->isReadOnly();
}

Store & ProvenanceEvaluator::getStore()
{
    return inner->getStore();
}

const fetchers::Settings & ProvenanceEvaluator::getFetchSettings()
{
    return inner->getFetchSettings();
}

ref<Object> ProvenanceEvaluator::evalFile(const SourcePath & path, const std::string & displayPath)
{
    return wrap(inner->evalFile(path, displayPath));
}

ref<Object> ProvenanceEvaluator::evalExpr(const std::string & expr, const SourcePath & basePath)
{
    return wrap(inner->evalExpr(expr, basePath));
}

ref<Object> ProvenanceEvaluator::evalExprLazy(const std::string & expr, const SourcePath & basePath)
{
    return wrap(inner->evalExprLazy(expr, basePath));
}

ref<Object> ProvenanceEvaluator::mkString(const std::string & s)
{
    return wrap(inner->mkString(s));
}

ref<Object> ProvenanceEvaluator::mkAttrs(const std::map<std::string, ref<Object>> & attrs)
{
    return wrap(inner->mkAttrs(attrs));
}

ref<Object> ProvenanceEvaluator::apply(ref<Object> fn, ref<Object> arg)
{
    return wrap(inner->apply(fn, arg));
}

EvalState & ProvenanceEvaluator::getEvalState()
{
    return inner->getEvalState();
}

} // namespace nix
