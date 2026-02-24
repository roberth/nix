#include "nix/expr/get-drvs.hh"
#include "nix/expr/environment/system.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/print.hh"
#include "nix/store/derivations.hh"
#include "nix/store/store-api.hh"
#include "nix/store/path-with-outputs.hh"

#include <cstring>
#include <regex>

namespace nix {

PackageInfo::PackageInfo(EvalState & state, std::string attrPath, std::shared_ptr<Object> attrs)
    : state(&state)
    , attrs(std::move(attrs))
    , attrPath(std::move(attrPath))
{
}

PackageInfo::PackageInfo(EvalState & state, ref<Store> store, const std::string & drvPathWithOutputs)
    : state(&state)
    , attrs(nullptr)
    , attrPath("")
{
    auto [drvPath, selectedOutputs] = parsePathWithOutputs(*store, drvPathWithOutputs);

    this->drvPath = drvPath;

    auto drv = store->derivationFromPath(drvPath);

    name = drvPath.name();

    if (selectedOutputs.size() > 1)
        throw Error("building more than one derivation output is not supported, in '%s'", drvPathWithOutputs);

    outputName = selectedOutputs.empty() ? getOr(drv.env, "outputName", "out") : *selectedOutputs.begin();

    auto i = drv.outputs.find(outputName);
    if (i == drv.outputs.end())
        throw Error("derivation '%s' does not have output '%s'", store->printStorePath(drvPath), outputName);
    auto & [outputName, output] = *i;

    outPath = {output.path(*store, drv.name, outputName)};
}

std::string PackageInfo::queryName() const
{
    if (name == "" && attrs) {
        auto i = attrs->maybeGetAttr("name");
        if (!i)
            state->error<TypeError>("derivation name missing").debugThrow();
        try {
            name = i->getStringWithoutContext();
        } catch (Error & e) {
            e.addTrace(nullptr, "while evaluating the 'name' attribute of a derivation");
            throw;
        }
    }
    return name;
}

std::string PackageInfo::querySystem() const
{
    if (system == "" && attrs) {
        auto i = attrs->maybeGetAttr("system");
        if (!i) {
            system = "unknown";
        } else {
            try {
                system = i->getStringWithoutContext();
            } catch (Error & e) {
                e.addTrace(state->positions[i->getPos()], "while evaluating the 'system' attribute of a derivation");
                throw;
            }
        }
    }
    return system;
}

std::optional<StorePath> PackageInfo::queryDrvPath() const
{
    if (!drvPath && attrs) {
        if (auto i = attrs->maybeGetAttr("drvPath")) {
            try {
                auto [s, context] = i->getStringWithContext();
                auto & store = *state->systemEnvironment->store;
                if (auto storePath = store.maybeParseStorePath(s)) {
                    storePath->requireDerivation();
                    drvPath = {std::move(*storePath)};
                } else {
                    state->error<EvalError>("path '%1%' is not in the Nix store", s).debugThrow();
                }
            } catch (Error & e) {
                e.addTrace(state->positions[i->getPos()], "while evaluating the 'drvPath' attribute of a derivation");
                throw;
            }
        } else
            drvPath = {std::nullopt};
    }
    return drvPath.value_or(std::nullopt);
}

StorePath PackageInfo::requireDrvPath() const
{
    if (auto drvPath = queryDrvPath())
        return *drvPath;
    throw Error("derivation does not contain a 'drvPath' attribute");
}

StorePath PackageInfo::queryOutPath() const
{
    if (!outPath && attrs) {
        if (auto i = attrs->maybeGetAttr("outPath")) {
            try {
                auto [s, context] = i->getStringWithContext();
                auto & store = *state->systemEnvironment->store;
                if (auto storePath = store.maybeParseStorePath(s))
                    outPath = std::move(*storePath);
                else
                    state->error<EvalError>("path '%1%' is not in the Nix store", s).debugThrow();
            } catch (Error & e) {
                e.addTrace(state->positions[i->getPos()], "while evaluating the output path of a derivation");
                throw;
            }
        }
    }
    if (!outPath)
        throw Error("derivation does not have attribute 'outPath'");
    return *outPath;
}

static bool checkMeta(EvalState & state, Object & obj);

PackageInfo::Outputs PackageInfo::queryOutputs(bool withPaths, bool onlyOutputsToInstall)
{
    if (outputs.empty()) {
        /* Get the 'outputs' list. */
        auto i = attrs ? attrs->maybeGetAttr("outputs") : nullptr;
        if (i) {
            /* Force the list type. */
            if (i->getType() != nList) {
                try {
                    auto val = i->defeatCache();
                    state
                        ->error<TypeError>(
                            "expected a list but found %1%: %2%",
                            showType(**val),
                            ValuePrinter(*state, **val, errorPrintOptions))
                        .debugThrow();
                } catch (Error & e) {
                    e.addTrace(
                        state->positions[i->getPos()], "while evaluating the 'outputs' attribute of a derivation");
                    throw;
                }
            }

            /* Iterate output names manually for per-element error traces. */
            auto listSize = i->getListSize();
            for (size_t idx = 0; idx < listSize; idx++) {
                auto elem = i->getListElem(idx);
                std::string output;
                try {
                    output = elem->getStringWithoutContext();
                } catch (Error & e) {
                    e.addTrace(state->positions[i->getPos()], "while evaluating the name of an output of a derivation");
                    throw;
                }

                if (withPaths) {
                    /* Evaluate the corresponding set. */
                    auto out = attrs->maybeGetAttr(output);
                    if (!out)
                        continue; // FIXME: throw error?
                    if (out->getType() != nAttrs) {
                        try {
                            auto val = out->defeatCache();
                            state
                                ->error<TypeError>(
                                    "expected a set but found %1%: %2%",
                                    showType(**val),
                                    ValuePrinter(*state, **val, errorPrintOptions))
                                .debugThrow();
                        } catch (Error & e) {
                            e.addTrace(state->positions[i->getPos()], "while evaluating an output of a derivation");
                            throw;
                        }
                    }

                    /* And evaluate its 'outPath' attribute. */
                    auto outPathObj = out->maybeGetAttr("outPath");
                    if (!outPathObj)
                        continue; // FIXME: throw error?
                    try {
                        auto [s, context] = outPathObj->getStringWithContext();
                        auto & store = *state->systemEnvironment->store;
                        if (auto storePath = store.maybeParseStorePath(s))
                            outputs.emplace(output, std::move(*storePath));
                        else
                            state->error<EvalError>("path '%1%' is not in the Nix store", s).debugThrow();
                    } catch (Error & e) {
                        e.addTrace(
                            state->positions[outPathObj->getPos()], "while evaluating an output path of a derivation");
                        throw;
                    }
                } else
                    outputs.emplace(output, std::nullopt);
            }
        } else
            outputs.emplace("out", withPaths ? std::optional{queryOutPath()} : std::nullopt);
    }

    if (!onlyOutputsToInstall || !attrs)
        return outputs;

    /* If outputSpecified is set and true, use the specified output. */
    if (auto aOutputSpecified = attrs->maybeGetAttr("outputSpecified")) {
        if (aOutputSpecified->getBool("while evaluating the 'outputSpecified' attribute of a derivation")) {
            std::string sOutputName = queryOutputName();
            auto out = outputs.find(sOutputName);
            if (out == outputs.end())
                throw Error("derivation does not have output '%s'", sOutputName);
            return Outputs{*out};
        }
    }

    /* Check for `meta.outputsToInstall` and return `outputs` reduced to that. */
    auto meta = getMetaObj();
    if (meta) {
        if (auto outTI = meta->maybeGetAttr("outputsToInstall")) {
            if (checkMeta(*state, *outTI)) {
                auto errMsg = Error("this derivation has bad 'meta.outputsToInstall'");
                if (outTI->getType() != nList)
                    throw errMsg;

                Outputs result;
                for (size_t i = 0; i < outTI->getListSize(); i++) {
                    auto elem = outTI->getListElem(i);
                    if (elem->getType() != nString)
                        throw errMsg;
                    auto name = elem->getStringWithoutContext();
                    auto out = outputs.find(name);
                    if (out == outputs.end())
                        throw errMsg;
                    result.insert(*out);
                }
                return result;
            }
        }
    }

    return outputs;
}

std::string PackageInfo::queryOutputName() const
{
    if (outputName == "" && attrs) {
        auto i = attrs->maybeGetAttr("outputName");
        if (!i) {
            outputName = "";
        } else {
            try {
                outputName = i->getStringWithoutContext();
            } catch (Error & e) {
                e.addTrace(nullptr, "while evaluating the output name of a derivation");
                throw;
            }
        }
    }
    return outputName;
}

std::shared_ptr<Object> PackageInfo::getMetaObj()
{
    if (meta)
        return meta;
    if (!attrs)
        return nullptr;
    auto metaObj = attrs->maybeGetAttr("meta");
    if (!metaObj)
        return nullptr;
    if (metaObj->getType() != nAttrs) {
        try {
            auto val = metaObj->defeatCache();
            state
                ->error<TypeError>(
                    "expected a set but found %1%: %2%",
                    showType(**val),
                    ValuePrinter(*state, **val, errorPrintOptions))
                .debugThrow();
        } catch (Error & e) {
            e.addTrace(state->positions[metaObj->getPos()], "while evaluating the 'meta' attribute of a derivation");
            throw;
        }
    }
    meta = metaObj;
    return meta;
}

StringSet PackageInfo::queryMetaNames()
{
    StringSet res;
    auto meta = getMetaObj();
    if (!meta)
        return res;
    for (auto & name : meta->getAttrNames())
        res.emplace(name);
    return res;
}

static bool checkMeta(EvalState & state, Object & obj)
{
    auto _level = state.addCallDepth(obj.getPos());

    auto type = obj.getType();
    if (type == nList) {
        for (size_t i = 0; i < obj.getListSize(); i++)
            if (!checkMeta(state, *obj.getListElem(i)))
                return false;
        return true;
    } else if (type == nAttrs) {
        if (obj.maybeGetAttr("outPath"))
            return false;
        for (auto & name : obj.getAttrNames())
            if (!checkMeta(state, *obj.maybeGetAttr(name)))
                return false;
        return true;
    } else
        return type == nInt || type == nBool || type == nString || type == nFloat;
}

Value * PackageInfo::queryMeta(const std::string & name)
{
    auto meta = getMetaObj();
    if (!meta)
        return 0;
    auto attr = meta->maybeGetAttr(name);
    if (!attr || !checkMeta(*state, *attr))
        return 0;
    return *attr->defeatCache();
}

std::string PackageInfo::queryMetaString(const std::string & name)
{
    Value * v = queryMeta(name);
    if (!v || v->type() != nString)
        return "";
    return std::string{v->string_view()};
}

NixInt PackageInfo::queryMetaInt(const std::string & name, NixInt def)
{
    Value * v = queryMeta(name);
    if (!v)
        return def;
    if (v->type() == nInt)
        return v->integer();
    if (v->type() == nString) {
        /* Backwards compatibility with before we had support for
           integer meta fields. */
        if (auto n = string2Int<NixInt::Inner>(v->string_view()))
            return NixInt{*n};
    }
    return def;
}

NixFloat PackageInfo::queryMetaFloat(const std::string & name, NixFloat def)
{
    Value * v = queryMeta(name);
    if (!v)
        return def;
    if (v->type() == nFloat)
        return v->fpoint();
    if (v->type() == nString) {
        /* Backwards compatibility with before we had support for
           float meta fields. */
        if (auto n = string2Float<NixFloat>(v->string_view()))
            return *n;
    }
    return def;
}

bool PackageInfo::queryMetaBool(const std::string & name, bool def)
{
    Value * v = queryMeta(name);
    if (!v)
        return def;
    if (v->type() == nBool)
        return v->boolean();
    if (v->type() == nString) {
        /* Backwards compatibility with before we had support for
           Boolean meta fields. */
        if (v->string_view() == "true")
            return true;
        if (v->string_view() == "false")
            return false;
    }
    return def;
}

void PackageInfo::setMeta(const std::string & name, Value * v)
{
    auto oldMeta = getMetaObj();
    auto bindings = state->buildBindings(1 + (oldMeta ? oldMeta->getAttrNames().size() : 0));
    auto sym = state->symbols.create(name);
    if (oldMeta) {
        for (auto & attrName : oldMeta->getAttrNames()) {
            if (attrName != name)
                bindings.insert(state->symbols.create(attrName), *oldMeta->maybeGetAttr(attrName)->defeatCache());
        }
    }
    if (v)
        bindings.insert(sym, v);
    auto metaVal = state->allocValue();
    metaVal->mkAttrs(bindings.finish());
    meta = state->toObjectCompat(*metaVal);
}

/* Evaluate value `v'.  If it evaluates to a set of type `derivation',
   then put information about it in `drvs'.
   The result boolean indicates whether it makes sense
   for the caller to recursively search for derivations in `v'. */
static bool getDerivation(
    EvalState & state, Value & v, const std::string & attrPath, PackageInfos & drvs, bool ignoreAssertionFailures)
{
    try {
        state.forceValue(v, v.determinePos(noPos));
        if (!state.isDerivation(v))
            return true;

        auto attrsVal = state.allocValue();
        // FIXME: get rid of raw Value &, avoid these crimes: const_cast and copying
        attrsVal->mkAttrs(const_cast<Bindings *>(v.attrs()));
        PackageInfo drv(state, attrPath, state.toObjectCompat(*attrsVal));

        drv.queryName();

        drvs.push_back(drv);

        return false;

    } catch (AssertionError & e) {
        if (ignoreAssertionFailures)
            return false;
        throw;
    }
}

std::optional<PackageInfo> getDerivation(EvalState & state, Value & v, bool ignoreAssertionFailures)
{
    PackageInfos drvs;
    getDerivation(state, v, "", drvs, ignoreAssertionFailures);
    if (drvs.size() != 1)
        return {};
    return std::move(drvs.front());
}

static std::string addToPath(const std::string & s1, std::string_view s2)
{
    return s1.empty() ? std::string(s2) : s1 + "." + s2;
}

static bool isAttrPathComponent(std::string_view symbol)
{
    if (symbol.empty())
        return false;

    /* [A-Za-z_] */
    unsigned char first = symbol[0];
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_'))
        return false;

    /* [A-Za-z0-9-_+]* */
    for (unsigned char c : symbol.substr(1)) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'
            || c == '+')
            continue;
        return false;
    }

    return true;
}

void getDerivations(
    EvalState & state,
    Value & vIn,
    const std::string & pathPrefix,
    Bindings & autoArgs,
    PackageInfos & drvs,
    bool ignoreAssertionFailures)
{
    auto _level = state.addCallDepth(vIn.determinePos(noPos));

    Value v;
    state.autoCallFunction(autoArgs, vIn, v);

    /* Process the expression. */
    if (!getDerivation(state, v, pathPrefix, drvs, ignoreAssertionFailures))
        ;

    else if (v.type() == nAttrs) {

        /* !!! undocumented hackery to support combining channels in
           nix-env.cc. */
        bool combineChannels = v.attrs()->get(state.symbols.create("_combineChannels"));

        /* Consider the attributes in sorted order to get more
           deterministic behaviour in nix-env operations (e.g. when
           there are names clashes between derivations, the derivation
           bound to the attribute with the "lower" name should take
           precedence). */
        for (auto & i : v.attrs()->lexicographicOrder(state.symbols)) {
            std::string_view symbol{state.symbols[i->name]};
            try {
                debug("evaluating attribute '%1%'", symbol);
                if (!isAttrPathComponent(symbol))
                    continue;
                std::string pathPrefix2 = addToPath(pathPrefix, symbol);
                if (combineChannels)
                    getDerivations(state, *i->value, pathPrefix2, autoArgs, drvs, ignoreAssertionFailures);
                else if (getDerivation(state, *i->value, pathPrefix2, drvs, ignoreAssertionFailures)) {
                    /* If the value of this attribute is itself a set,
                    should we recurse into it?  => Only if it has a
                    `recurseForDerivations = true' attribute. */
                    if (i->value->type() == nAttrs) {
                        auto j = i->value->attrs()->get(state.s.recurseForDerivations);
                        if (j
                            && state.forceBool(
                                *j->value, j->pos, "while evaluating the attribute `recurseForDerivations`"))
                            getDerivations(state, *i->value, pathPrefix2, autoArgs, drvs, ignoreAssertionFailures);
                    }
                }
            } catch (Error & e) {
                e.addTrace(state.positions[i->pos], "while evaluating the attribute '%s'", symbol);
                throw;
            }
        }
    }

    else if (v.type() == nList) {
        auto listView = v.listView();
        for (auto [n, elem] : enumerate(listView)) {
            std::string pathPrefix2 = addToPath(pathPrefix, fmt("%d", n));
            if (getDerivation(state, *elem, pathPrefix2, drvs, ignoreAssertionFailures))
                getDerivations(state, *elem, pathPrefix2, autoArgs, drvs, ignoreAssertionFailures);
        }
    }

    else
        state.error<TypeError>("expression does not evaluate to a derivation (or a set or list of those)").debugThrow();
}

} // namespace nix
