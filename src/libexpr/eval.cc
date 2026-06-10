#include "nix/expr/eval.hh"
#include "nix/expr/eval-error.hh"
#include "nix/util/base-nix-32.hh"
#include "nix/util/diagnose.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/expr/source-root.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/primops.hh"
#include "nix/expr/print-options.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/expr/value.hh"
#include "nix/util/exit.hh"
#include "nix/util/types.hh"
#include "nix/util/util.hh"
#include "nix/util/environment-variables.hh"
#include "nix/store/store-api.hh"
#include "nix/store/derivations.hh"
#include "nix/store/downstream-placeholder.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/store/filetransfer.hh"
#include "nix/expr/function-trace.hh"
#include "nix/store/profiles.hh"
#include "nix/expr/print.hh"
#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/expr/gc-small-vector.hh"
#include "nix/util/url.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/fetchers/tarball.hh"
#include "nix/fetchers/input-cache.hh"
#include "nix/util/current-process.hh"

#include "parser-tab.hh"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
#include <cstring>
#include <optional>
#include <unistd.h>
#include <sys/time.h>
#include <fstream>
#include <functional>
#include <ranges>
#include <mutex>

#include <nlohmann/json.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/unordered/concurrent_flat_map.hpp>
#include <boost/unordered/concurrent_flat_set.hpp>

#include "nix/util/strings-inline.hh"

using json = nlohmann::json;

namespace nix {

/**
 * Just for doc strings. Not for regular string values.
 */
static char * allocString(size_t size)
{
    char * t;
    t = (char *) GC_MALLOC_ATOMIC(size);
    if (!t)
        throw std::bad_alloc();
    return t;
}

// When there's no need to write to the string, we can optimize away empty
// string allocations.
// This function handles makeImmutableString(std::string_view()) by returning
// the empty string.
/**
 * Just for doc strings. Not for regular string values.
 */
static const char * makeImmutableString(std::string_view s)
{
    const size_t size = s.size();
    if (size == 0)
        return "";
    auto t = allocString(size + 1);
    memcpy(t, s.data(), size);
    t[size] = '\0';
    return t;
}

StringData & StringData::alloc(EvalMemory & mem, size_t size)
{
    void * t = mem.allocBytes(sizeof(StringData) + size + 1);
    if (!t)
        throw std::bad_alloc();
    auto res = new (t) StringData(size);
    return *res;
}

const StringData & StringData::make(EvalMemory & mem, std::string_view s)
{
    if (s.empty())
        return ""_sds;
    auto & res = alloc(mem, s.size());
    std::memcpy(&res.data_, s.data(), s.size());
    res.data_[s.size()] = '\0';
    return res;
}

RootValue allocRootValue(Value * v)
{
    return std::allocate_shared<Value *>(traceable_allocator<Value *>(), v);
}

// Pretty print types for assertion errors
std::ostream & operator<<(std::ostream & os, const ValueType t)
{
    os << showType(t);
    return os;
}

std::string printValue(EvalState & state, Value & v)
{
    std::ostringstream out;
    v.print(state, out);
    return out.str();
}

Value * Value::toPtr(SymbolStr str) noexcept
{
    return const_cast<Value *>(str.valuePtr());
}

void Value::print(EvalState & state, std::ostream & str, PrintOptions options)
{
    printValue(state, str, *this, options);
}

std::string_view showType(ValueType type, bool withArticle)
{
#define WA(a, w) withArticle ? a " " w : w
    switch (type) {
    case nInt:
        return WA("an", "integer");
    case nBool:
        return WA("a", "Boolean");
    case nString:
        return WA("a", "string");
    case nPath:
        return WA("a", "path");
    case nNull:
        return "null";
    case nAttrs:
        return WA("a", "set");
    case nList:
        return WA("a", "list");
    case nFunction:
        return WA("a", "function");
    case nExternal:
        return WA("an", "external value");
    case nFloat:
        return WA("a", "float");
    case nThunk:
        return WA("a", "thunk");
    case nFailed:
        return WA("an", "error");
    }
    unreachable();
}

std::string showType(const Value & v)
{
// Allow selecting a subset of enum values
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (v.getInternalType()) {
    case tString:
        return v.context() ? "a string with context" : "a string";
    case tPrimOp:
        return fmt("the built-in function '%s'", std::string(v.primOp()->name));
    case tPrimOpApp:
        return fmt("the partially applied built-in function '%s'", v.primOpAppPrimOp()->name);
    case tExternal:
        return v.external()->showType();
    case tThunk:
        return v.isBlackhole() ? "a black hole" : "a thunk";
    case tApp:
        return "a function application";
    default:
        return std::string(showType(v.type()));
    }
#pragma GCC diagnostic pop
}

PosIdx Value::determinePos(const PosIdx pos) const
{
// Allow selecting a subset of enum values
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (getInternalType()) {
    case tAttrs:
        return attrs()->pos;
    case tLambda:
        return lambda().fun->pos;
    case tApp:
        return app().left->determinePos(pos);
    default:
        return pos;
    }
#pragma GCC diagnostic pop
}

bool Value::isTrivial() const
{
    return !isa<tApp, tPrimOpApp>()
           && (!isa<tThunk>()
               || (dynamic_cast<ExprAttrs *>(thunk().expr) && ((ExprAttrs *) thunk().expr)->dynamicAttrs->empty())
               || dynamic_cast<ExprLambda *>(thunk().expr) || dynamic_cast<ExprList *>(thunk().expr));
}

static Symbol getName(const AttrName & name, EvalState & state, Env & env)
{
    if (name.symbol) {
        return name.symbol;
    } else {
        Value nameValue;
        name.expr->eval(state, env, nameValue);
        state.forceStringNoCtx(nameValue, name.expr->getPos(), "while evaluating an attribute name");
        return state.symbols.create(nameValue.string_view());
    }
}

static constexpr size_t BASE_ENV_SIZE = 128;

EvalMemory::EvalMemory()
{
    assertGCInitialized();
}

EvalState::EvalState(
    const LookupPath & lookupPathFromArguments,
    ref<Store> store,
    const fetchers::Settings & fetchSettings,
    const EvalSettings & settings,
    std::shared_ptr<Store> buildStore)
    : fetchSettings{fetchSettings}
    , settings{settings}
    , symbols(StaticEvalSymbols::staticSymbolTable())
    , repair(NoRepair)
    , storeFS(makeMountedSourceAccessor({
          {CanonPath::root, [acc = makeEmptySourceAccessor()]() { return acc; }},
          /* In the pure eval case, we can simply require
             valid paths. However, in the *impure* eval
             case this gets in the way of the union
             mechanism, because an invalid access in the
             upper layer will *not* be caught by the union
             source accessor, but instead abort the entire
             lookup.

             This happens when the store dir in the
             ambient file system has a path (e.g. because
             another Nix store there), but the relocated
             store does not.

             TODO make the various source accessors doing
             access control all throw the same type of
             exception, and make union source accessor
             catch it, so we don't need to do this hack.
           */
          {CanonPath(store->storeDir), [acc = store->getFSAccessor(settings.pureEval)]() { return acc; }},
      }))
    , rootFS([&] {
        /* In pure eval mode, we provide a filesystem that only
           contains the Nix store.

           Otherwise, use a union accessor to make the augmented store
           available at its logical location while still having the
           underlying directory available. This is necessary for
           instance if we're evaluating a file from the physical
           /nix/store while using a chroot store, and also for lazy
           mounted fetchTree. */
        auto accessor = settings.pureEval ? storeFS.cast<SourceAccessor>()
                                          : makeUnionSourceAccessor({getFSSourceAccessor(), storeFS});
        /* Cache positive lstat/readlink results to speed up resolveSymlinks. */
        accessor = makeCachingSourceAccessor(accessor);

        /* Apply access control if needed. */
        if (settings.restrictEval || settings.pureEval)
            accessor = AllowListSourceAccessor::create(
                accessor, {}, {}, [&settings](const CanonPath & path) -> RestrictedPathError {
                    auto modeInformation = settings.pureEval ? "in pure evaluation mode (use '--impure' to override)"
                                                             : "in restricted mode";
                    throw RestrictedPathError("access to absolute path '%1%' is forbidden %2%", path, modeInformation);
                });

        return accessor;
    }())
    , corepkgsFS(make_ref<MemorySourceAccessor>())
    , internalFS(make_ref<MemorySourceAccessor>())
    , rootFSRoot(SourceRoot::make(rootFS, SourceRootKind::System))
    , corepkgsRoot(SourceRoot::make(corepkgsFS.cast<SourceAccessor>(), SourceRootKind::Internal))
    , internalFSRoot(SourceRoot::make(internalFS.cast<SourceAccessor>(), SourceRootKind::Internal))
    , derivationInternal{internalFS->addFile(
          CanonPath("derivation-internal.nix"),
#include "primops/derivation.nix.gen.hh"
          )}
    , importedDrvToDerivation{internalFS->addFile(
          CanonPath("imported-drv-to-derivation.nix"),
#include "imported-drv-to-derivation.nix.gen.hh"
          )}
    , store(store)
    , buildStore(buildStore ? buildStore : store)
    , inputCache(fetchers::InputCache::create())
    , debugRepl(nullptr)
    , debugStop(false)
    , trylevel(0)
    , accessorsKnownInequivalent(make_ref<decltype(accessorsKnownInequivalent)::element_type>())
    , accessorRootProbeCache(make_ref<decltype(accessorRootProbeCache)::element_type>())
    , accessorHintProbeCache(make_ref<decltype(accessorHintProbeCache)::element_type>())
    , srcToStore(make_ref<decltype(srcToStore)::element_type>())
    , importResolutionCache(make_ref<decltype(importResolutionCache)::element_type>())
    , fileEvalCache(make_ref<decltype(fileEvalCache)::element_type>())
    , positionToDocComment(make_ref<decltype(positionToDocComment)::element_type>())
    , lookupPathResolved(make_ref<decltype(lookupPathResolved)::element_type>())
    , regexCache(makeRegexCache())
    , rootCache(make_ref<decltype(rootCache)::element_type>())
#if NIX_USE_BOEHMGC
    , baseEnvP(std::allocate_shared<Env *>(traceable_allocator<Env *>(), &mem.allocEnv(BASE_ENV_SIZE)))
    , baseEnv(**baseEnvP)
#else
    , baseEnv(mem.allocEnv(BASE_ENV_SIZE))
#endif
    , staticBaseEnv{std::make_shared<StaticEnv>(nullptr, nullptr)}
{
#ifndef _WIN32
    static std::once_flag stackSizeBumped;
    std::call_once(stackSizeBumped, []() {
        // Increase the default stack size for the evaluator and for
        // libstdc++'s std::regex.
        // This used to be 64 MiB, but macOS as deployed on GitHub Actions has a
        // hard limit slightly under that, so we round it down a bit.
        nix::ensureStackSizeAtLeast(60 * 1024 * 1024);
    });
#endif

    corepkgsFS->setPathDisplay("<nix", ">");
    internalFS->setPathDisplay("«nix-internal»", "");

    countCalls = getEnv("NIX_COUNT_CALLS").value_or("0") != "0";

    static_assert(sizeof(Env) <= 16, "environment must be <= 16 bytes");

    /* Construct the Nix expression search path. */
    assert(lookupPath.elements.empty());
    if (!settings.pureEval) {
        for (auto & i : lookupPathFromArguments.elements) {
            lookupPath.elements.emplace_back(LookupPath::Elem{i});
        }
        /* $NIX_PATH overriding regular settings is implemented as a hack in `initGC()` */
        for (auto & i : settings.nixPath.get()) {
            lookupPath.elements.emplace_back(LookupPath::Elem::parse(i));
        }
        if (!settings.restrictEval) {
            for (auto & i : EvalSettings::getDefaultNixPath()) {
                lookupPath.elements.emplace_back(LookupPath::Elem::parse(i));
            }
        }
    }

    /* Allow access to all paths in the search path. */
    if (rootFS.dynamic_pointer_cast<AllowListSourceAccessor>())
        for (auto & i : lookupPath.elements)
            resolveLookupPathPath(i.path, true);

    corepkgsFS->addFile(
        CanonPath("fetchurl.nix"),
#include "fetchurl.nix.gen.hh"
    );

    createBaseEnv(settings);

    /* Register function call tracer. */
    if (settings.traceFunctionCalls)
        profiler.addProfiler(make_ref<FunctionCallTrace>());

    switch (settings.evalProfilerMode) {
    case EvalProfilerMode::flamegraph:
        profiler.addProfiler(
            makeSampleStackProfiler(*this, settings.evalProfileFile.get(), settings.evalProfilerFrequency));
        break;
    case EvalProfilerMode::disabled:
        break;
    }
}

EvalState::~EvalState() {}

void EvalState::allowPathLegacy(const std::string & path)
{
    if (auto rootFS2 = rootFS.dynamic_pointer_cast<AllowListSourceAccessor>())
        rootFS2->allowPrefix(CanonPath(path));
}

void EvalState::allowPath(const StorePath & storePath)
{
    if (auto rootFS2 = rootFS.dynamic_pointer_cast<AllowListSourceAccessor>())
        rootFS2->allowPrefix(CanonPath(store->printStorePath(storePath)));
}

void EvalState::allowClosure(const StorePath & storePath)
{
    if (!rootFS.dynamic_pointer_cast<AllowListSourceAccessor>())
        return;

    StorePathSet closure;
    store->computeFSClosure(storePath, closure);
    for (auto & p : closure)
        allowPath(p);
}

void EvalState::allowAndSetStorePathString(const StorePath & storePath, Value & v)
{
    allowPath(storePath);

    mkStorePathString(storePath, v);
}

inline static bool isJustSchemePrefix(std::string_view prefix)
{
    return !prefix.empty() && prefix[prefix.size() - 1] == ':'
           && isValidSchemeName(prefix.substr(0, prefix.size() - 1));
}

bool isAllowedURI(std::string_view uri, const Strings & allowedUris)
{
    /* 'uri' should be equal to a prefix, or in a subdirectory of a
       prefix. Thus, the prefix https://github.co does not permit
       access to https://github.com. */
    for (auto & prefix : allowedUris) {
        if (uri == prefix
            // Allow access to subdirectories of the prefix.
            || (uri.size() > prefix.size() && prefix.size() > 0 && hasPrefix(uri, prefix)
                && (
                    // Allow access to subdirectories of the prefix.
                    prefix[prefix.size() - 1] == '/'
                    || uri[prefix.size()] == '/'

                    // Allow access to whole schemes
                    || isJustSchemePrefix(prefix))))
            return true;
    }

    return false;
}

void EvalState::checkURI(const std::string & uri0)
{
    if (!settings.restrictEval)
        return;

    if (isAllowedURI(uri0, settings.allowedUris.get()))
        return;

    /* If the URI is a path, then check it against allowedPaths as
       well. */
    {
        std::filesystem::path path(uri0);
        if (path.is_absolute()) {
            if (auto rootFS2 = rootFS.dynamic_pointer_cast<AllowListSourceAccessor>())
                rootFS2->checkAccess(CanonPath(path.string()));
            return;
        }
    }

    try {
        ParsedURL uri = parseURL(uri0);
        if (uri.scheme == "file") {
            if (auto rootFS2 = rootFS.dynamic_pointer_cast<AllowListSourceAccessor>())
                rootFS2->checkAccess(CanonPath(urlPathToPath(uri.path).string()));
            return;
        }
    } catch (BadURL &) {
    }

    throw RestrictedPathError("access to URI '%s' is forbidden in restricted mode", uri0);
}

Value * EvalState::addConstant(const std::string & name, Value & v, Constant info)
{
    Value * v2 = allocValue();
    *v2 = v;
    addConstant(name, v2, info);
    return v2;
}

void EvalState::addConstant(const std::string & name, Value * v, Constant info)
{
    auto name2 = name.substr(0, 2) == "__" ? name.substr(2) : name;

    constantInfos.push_back({name2, info});

    if (!(settings.pureEval && info.impureOnly)) {
        /* Check the type, if possible.

           We might know the type of a thunk in advance, so be allowed
           to just write it down in that case. */
        if (auto gotType = v->type</*invalidIsThunk=*/true>(); gotType != nThunk)
            assert(info.type == gotType);

        /* Install value the base environment. */
        staticBaseEnv->vars.emplace_back(symbols.create(name), baseEnvDispl);
        baseEnv.values[baseEnvDispl++] = v;
        const_cast<Bindings *>(getBuiltins().attrs())->push_back(Attr(symbols.create(name2), v));
    }
}

void PrimOp::check()
{
    if (arity > maxPrimOpArity) {
        throw Error("primop arity must not exceed %1%", maxPrimOpArity);
    }
}

std::ostream & operator<<(std::ostream & output, const PrimOp & primOp)
{
    output << "primop " << primOp.name;
    return output;
}

const PrimOp * Value::primOpAppPrimOp() const
{
    Value * left = primOpApp().left;
    while (left && !left->isPrimOp()) {
        left = left->primOpApp().left;
    }

    if (!left)
        return nullptr;

    assert(left->isPrimOp());
    return left->primOp();
}

void Value::mkPrimOp(PrimOp * p)
{
    p->check();
    setStorage(p);
}

Value * EvalState::addPrimOp(PrimOp && primOp)
{
    /* Hack to make constants lazy: turn them into a application of
       the primop to a dummy value. */
    if (primOp.arity == 0) {
        primOp.arity = 1;
        auto vPrimOp = allocValue();
        vPrimOp->mkPrimOp(new PrimOp(std::move(primOp)));
        Value v;
        v.mkApp(vPrimOp, vPrimOp);
        auto & primOp1 = *vPrimOp->primOp();
        return addConstant(
            primOp1.name,
            v,
            {
                .type = nThunk, // FIXME
                .doc = primOp1.doc ? primOp1.doc->c_str() : nullptr,
            });
    }

    auto envName = symbols.create(primOp.name);
    if (hasPrefix(primOp.name, "__"))
        primOp.name = primOp.name.substr(2);

    Value * v = allocValue();
    v->mkPrimOp(new PrimOp(primOp));

    if (primOp.internal)
        internalPrimOps.emplace(primOp.name, v);
    else {
        staticBaseEnv->vars.emplace_back(envName, baseEnvDispl);
        baseEnv.values[baseEnvDispl++] = v;
        const_cast<Bindings *>(getBuiltins().attrs())->push_back(Attr(symbols.create(primOp.name), v));
    }

    return v;
}

Value & EvalState::getBuiltins()
{
    return *baseEnv.values[0];
}

Value & EvalState::getBuiltin(const std::string & name)
{
    auto it = getBuiltins().attrs()->get(symbols.create(name));
    if (it)
        return *it->value;
    else
        error<EvalError>("builtin '%1%' not found", name).debugThrow();
}

std::optional<EvalState::Doc> EvalState::getDoc(Value & v)
{
    if (v.isPrimOp()) {
        auto v2 = &v;
        auto & primOp = *v2->primOp();
        if (primOp.doc)
            return Doc{
                .pos = {},
                .name = primOp.name,
                .arity = primOp.arity,
                .args = primOp.args,
                .doc = primOp.doc->c_str(),
            };
    }
    if (v.isLambda()) {
        auto exprLambda = v.lambda().fun;

        std::ostringstream s;
        std::string name;
        auto pos = positions[exprLambda->getPos()];
        std::string docStr;

        if (exprLambda->name) {
            name = symbols[exprLambda->name];
        }

        if (exprLambda->docComment) {
            docStr = exprLambda->docComment.getInnerText(positions);
        }

        if (name.empty()) {
            s << "Function ";
        } else {
            s << "Function `" << name << "`";
            if (pos)
                s << "\\\n  … ";
            else
                s << "\\\n";
        }
        if (pos) {
            s << "defined at " << pos;
        }
        if (!docStr.empty()) {
            s << "\n\n";
        }

        s << docStr;

        return Doc{
            .pos = pos,
            .name = name,
            .arity = 0, // FIXME: figure out how deep by syntax only? It's not semantically useful though...
            .args = {},
            /* N.B. Can't use StringData here, because that would lead to an interior pointer.
               NOTE: memory leak when compiled without GC. */
            .doc = makeImmutableString(s.view()),
        };
    }
    if (isFunctor(v)) {
        try {
            Value & functor = *v.attrs()->get(s.functor)->value;
            Value * vp[] = {&v};
            Value partiallyApplied;
            // The first parameter is not user-provided, and may be
            // handled by code that is opaque to the user, like lib.const = x: y: y;
            // So preferably we show docs that are relevant to the
            // "partially applied" function returned by e.g. `const`.
            // We apply the first argument:
            callFunction(functor, vp, partiallyApplied, noPos);
            auto _level = addCallDepth(noPos);
            return getDoc(partiallyApplied);
        } catch (Error & e) {
            e.addTrace(nullptr, "while partially calling '%1%' to retrieve documentation", "__functor");
            throw;
        }
    }
    return {};
}

// just for the current level of StaticEnv, not the whole chain.
void printStaticEnvBindings(const SymbolTable & st, const StaticEnv & se)
{
    std::cout << ANSI_MAGENTA;
    for (auto & i : se.vars)
        std::cout << st[i.first] << " ";
    std::cout << ANSI_NORMAL;
    std::cout << std::endl;
}

// just for the current level of Env, not the whole chain.
void printWithBindings(const SymbolTable & st, const Env & env)
{
    if (!env.values[0]->isThunk()) {
        std::cout << "with: ";
        std::cout << ANSI_MAGENTA;
        auto j = env.values[0]->attrs()->begin();
        while (j != env.values[0]->attrs()->end()) {
            std::cout << st[j->name] << " ";
            ++j;
        }
        std::cout << ANSI_NORMAL;
        std::cout << std::endl;
    }
}

void printEnvBindings(const SymbolTable & st, const StaticEnv & se, const Env & env, int lvl)
{
    std::cout << "Env level " << lvl << std::endl;

    if (se.up && env.up) {
        std::cout << "static: ";
        printStaticEnvBindings(st, se);
        if (se.isWith)
            printWithBindings(st, env);
        std::cout << std::endl;
        printEnvBindings(st, *se.up, *env.up, ++lvl);
    } else {
        std::cout << ANSI_MAGENTA;
        // for the top level, don't print the double underscore ones;
        // they are in builtins.
        for (auto & i : se.vars)
            if (!hasPrefix(st[i.first], "__"))
                std::cout << st[i.first] << " ";
        std::cout << ANSI_NORMAL;
        std::cout << std::endl;
        if (se.isWith)
            printWithBindings(st, env); // probably nothing there for the top level.
        std::cout << std::endl;
    }
}

void printEnvBindings(const EvalState & es, const Expr & expr, const Env & env)
{
    // just print the names for now
    auto se = es.getStaticEnv(expr);
    if (se)
        printEnvBindings(es.symbols, *se, env, 0);
}

void mapStaticEnvBindings(const SymbolTable & st, const StaticEnv & se, const Env & env, ValMap & vm)
{
    // add bindings for the next level up first, so that the bindings for this level
    // override the higher levels.
    // The top level bindings (builtins) are skipped since they are added for us by initEnv()
    if (env.up && se.up) {
        mapStaticEnvBindings(st, *se.up, *env.up, vm);

        if (se.isWith && !env.values[0]->isThunk()) {
            // add 'with' bindings.
            for (auto & j : *env.values[0]->attrs())
                vm.insert_or_assign(std::string(st[j.name]), j.value);
        } else {
            // iterate through staticenv bindings and add them.
            for (auto & i : se.vars)
                vm.insert_or_assign(std::string(st[i.first]), env.values[i.second]);
        }
    }
}

std::unique_ptr<ValMap> mapStaticEnvBindings(const SymbolTable & st, const StaticEnv & se, const Env & env)
{
    auto vm = std::make_unique<ValMap>();
    mapStaticEnvBindings(st, se, env, *vm);
    return vm;
}

/**
 * Sets `inDebugger` to true on construction and false on destruction.
 */
class DebuggerGuard
{
    bool & inDebugger;
public:
    DebuggerGuard(bool & inDebugger)
        : inDebugger(inDebugger)
    {
        inDebugger = true;
    }

    DebuggerGuard(DebuggerGuard &&) = delete;
    DebuggerGuard(const DebuggerGuard &) = delete;
    DebuggerGuard & operator=(DebuggerGuard &&) = delete;
    DebuggerGuard & operator=(const DebuggerGuard &) = delete;

    ~DebuggerGuard()
    {
        inDebugger = false;
    }
};

bool EvalState::canDebug()
{
    return debugRepl && !debugTraces.empty();
}

void EvalState::runDebugRepl(const Error * error)
{
    if (!canDebug())
        return;

    assert(!debugTraces.empty());
    const DebugTrace & last = debugTraces.front();
    const Env & env = last.env;
    const Expr & expr = last.expr;

    runDebugRepl(error, env, expr);
}

void EvalState::runDebugRepl(const Error * error, const Env & env, const Expr & expr)
{
    // Make sure we have a debugger to run and we're not already in a debugger.
    if (!debugRepl || inDebugger)
        return;

    auto dts = [&]() -> std::unique_ptr<DebugTraceStacker> {
        if (error && expr.getPos()) {
            auto trace = DebugTrace{
                .pos = [&]() -> std::variant<Pos, PosIdx> {
                    if (error->info().pos) {
                        if (auto * pos = error->info().pos.get())
                            return *pos;
                        return noPos;
                    }
                    return expr.getPos();
                }(),
                .expr = expr,
                .env = env,
                .hint = error->info().msg,
                .isError = true};

            return std::make_unique<DebugTraceStacker>(*this, std::move(trace));
        }
        return nullptr;
    }();

    if (error) {
        printError("%s\n", error->what());

        if (trylevel > 0 && error->info().level != lvlInfo)
            printError(
                "This exception occurred in a 'tryEval' call. Use " ANSI_GREEN "--ignore-try" ANSI_NORMAL
                " to skip these.\n");
    }

    auto se = getStaticEnv(expr);
    if (se) {
        auto vm = mapStaticEnvBindings(symbols, *se.get(), env);
        DebuggerGuard _guard(inDebugger);
        auto exitStatus = (debugRepl) (ref<EvalState>(shared_from_this()), *vm);
        switch (exitStatus) {
        case ReplExitStatus::QuitAll:
            if (error)
                throw *error;
            throw Exit(0);
        case ReplExitStatus::Continue:
            break;
        default:
            unreachable();
        }
    }
}

template<typename... Args>
void EvalState::addErrorTrace(Error & e, const Args &... formatArgs) const
{
    e.addTrace(nullptr, HintFmt(formatArgs...));
}

template<typename... Args>
void EvalState::addErrorTrace(Error & e, const PosIdx pos, const Args &... formatArgs) const
{
    e.addTrace(positions[pos], HintFmt(formatArgs...));
}

template<typename... Args>
static std::unique_ptr<DebugTraceStacker> makeDebugTraceStacker(
    EvalState & state, Expr & expr, Env & env, std::variant<Pos, PosIdx> pos, const Args &... formatArgs)
{
    return std::make_unique<DebugTraceStacker>(
        state,
        DebugTrace{.pos = std::move(pos), .expr = expr, .env = env, .hint = HintFmt(formatArgs...), .isError = false});
}

DebugTraceStacker::DebugTraceStacker(EvalState & evalState, DebugTrace t)
    : evalState(evalState)
    , trace(std::move(t))
{
    evalState.debugTraces.push_front(trace);
    if (evalState.debugStop && evalState.debugRepl)
        evalState.runDebugRepl(nullptr, trace.env, trace.expr);
}

void Value::mkString(std::string_view s, EvalMemory & mem)
{
    mkStringNoCopy(StringData::make(mem, s));
}

Value::StringWithContext::Context *
Value::StringWithContext::Context::fromBuilder(const NixStringContext & context, EvalMemory & mem)
{
    if (context.empty())
        return nullptr;

    auto ctx = new (mem.allocBytes(sizeof(Context) + context.size() * sizeof(value_type))) Context(context.size());
    std::ranges::transform(
        context, ctx->elems, [&](const NixStringContextElem & elt) { return &StringData::make(mem, elt.to_string()); });
    return ctx;
}

void Value::mkString(std::string_view s, const NixStringContext & context, EvalMemory & mem)
{
    mkStringNoCopy(StringData::make(mem, s), Value::StringWithContext::Context::fromBuilder(context, mem));
}

void Value::mkStringMove(const StringData & s, const NixStringContext & context, EvalMemory & mem)
{
    mkStringNoCopy(s, Value::StringWithContext::Context::fromBuilder(context, mem));
}

void Value::mkPath(const RootedPath & path, EvalMemory & mem)
{
    mkPath(&*path.root, StringData::make(mem, path.path.abs()));
}

[[gnu::always_inline]] inline Value * EvalState::lookupVar(Env * env, const ExprVar & var, bool noEval)
{
    for (auto l = var.level; l; --l, env = env->up)
        ;

    if (!var.fromWith)
        return env->values[var.displ];

    // This early exit defeats the `maybeThunk` optimization for variables from `with`,
    // The added complexity of handling this appears to be similarly in cost, or
    // the cases where applicable were insignificant in the first place.
    if (noEval)
        return nullptr;

    auto * fromWith = var.fromWith;
    while (1) {
        forceAttrs(*env->values[0], fromWith->pos, "while evaluating the first subexpression of a with expression");
        if (auto j = env->values[0]->attrs()->get(var.name)) {
            if (countCalls) [[unlikely]]
                attrSelects[j->pos]++;
            return j->value;
        }
        if (!fromWith->parentWith) [[unlikely]]
            error<UndefinedVarError>("undefined variable '%1%'", symbols[var.name])
                .atPos(var.pos)
                .withFrame(*env, var)
                .debugThrow();
        for (size_t l = fromWith->prevWith; l; --l, env = env->up)
            ;
        fromWith = fromWith->parentWith;
    }
}

ListBuilder::ListBuilder(EvalMemory & mem, size_t size)
    : size(size)
    , elems(size <= 2 ? inlineElems : (Value **) mem.allocBytes(size * sizeof(Value *)))
{
}

Value * EvalState::getBool(bool b)
{
    return b ? &Value::vTrue : &Value::vFalse;
}

static Counter nrThunks;

static inline void mkThunk(Value & v, Env & env, Expr * expr)
{
    v.mkThunk(&env, expr);
    nrThunks++;
}

void EvalState::mkThunk_(Value & v, Expr * expr)
{
    mkThunk(v, baseEnv, expr);
}

void EvalState::mkPos(Value & v, PosIdx p)
{
    auto origin = positions.originOf(p);
    if (auto path = std::get_if<RootedPath>(&origin)) {
        /* Positions whose origin lives on an Internal-kinded
           SourceRoot — corepkgs files, derivation-internal helpers,
           similar internals — have no user-visible source location.
           Surface them as `null`, matching the shape unsafeGetAttrPos
           already uses for genuinely missing positions, rather than
           an attrset whose `.file` would resolve to nix-internal
           accessor coordinates (`<nix>foo.nix`,
           `«nix-internal»derivation-internal.nix`, ...). */
        if (path->root->kind == SourceRootKind::Internal) {
            v.mkNull();
            return;
        }
        auto attrs = buildBindings(4);
        /* `.file` is a thunk over `fileOfPos` (see `LazyPosAccessors`).
           System accessors render to the raw absolute canon path;
           Copyable accessors render to `<storePath>/<subpath>` via
           `coerceToString`, so the file string is reachable via the
           store (`nix edit`, `meta.position`, ...) instead of being
           accessor-internal nonsense like `/inner.nix` from a fetched
           tree. The thunk shape lets the kind-driven rendering stay
           lazy: positions are emitted in every eval but only a small
           fraction ever has `.file` forced. */
        makePositionThunks(*this, p, attrs.alloc(s.file), attrs.alloc(s.line), attrs.alloc(s.column));
        /* `.path` carries the same origin as a path Value — the
           same shape `fetchTree`'s lazy `outPath` emits. Callers
           that just want to compare or manipulate the path
           (NixOS-style module discovery: `unsafeGetAttrPos`'s
           result feeds into module identity) can stay on the
           path side and skip the Copyable materialisation that
           `.file` forces. `toString` on `.path` reproduces
           `.file`'s string. */
        attrs.alloc(s.path).mkPath(*path, mem);
        v.mkAttrs(attrs);
    } else
        v.mkNull();
}

void EvalState::mkStorePathString(const StorePath & p, Value & v)
{
    v.mkString(
        store->printStorePath(p),
        NixStringContext{
            NixStringContextElem::Opaque{.path = p},
        },
        mem);
}

std::string EvalState::mkOutputStringRaw(
    const SingleDerivedPath::Built & b,
    std::optional<StorePath> optStaticOutputPath,
    const ExperimentalFeatureSettings & xpSettings)
{
    /* In practice, this is testing for the case of CA derivations, or
       dynamic derivations. */
    return optStaticOutputPath ? store->printStorePath(std::move(*optStaticOutputPath))
                               /* Downstream we would substitute this for an actual path once
                                  we build the floating CA derivation */
                               : DownstreamPlaceholder::fromSingleDerivedPathBuilt(b, xpSettings).render();
}

void EvalState::mkOutputString(
    Value & value,
    const SingleDerivedPath::Built & b,
    std::optional<StorePath> optStaticOutputPath,
    const ExperimentalFeatureSettings & xpSettings)
{
    value.mkString(mkOutputStringRaw(b, optStaticOutputPath, xpSettings), NixStringContext{b}, mem);
}

std::string EvalState::mkSingleDerivedPathStringRaw(const SingleDerivedPath & p)
{
    return std::visit(
        overloaded{
            [&](const SingleDerivedPath::Opaque & o) { return store->printStorePath(o.path); },
            [&](const SingleDerivedPath::Built & b) {
                auto optStaticOutputPath = std::visit(
                    overloaded{
                        [&](const SingleDerivedPath::Opaque & o) {
                            auto drv = store->readDerivation(o.path);
                            auto i = drv.outputs.find(b.output);
                            if (i == drv.outputs.end())
                                throw Error(
                                    "derivation '%s' does not have output '%s'",
                                    b.drvPath->to_string(*store),
                                    b.output);
                            return i->second.path(*store, drv.name, b.output);
                        },
                        [&](const SingleDerivedPath::Built & o) -> std::optional<StorePath> { return std::nullopt; },
                    },
                    b.drvPath->raw());
                return mkOutputStringRaw(b, optStaticOutputPath);
            }},
        p.raw());
}

void EvalState::mkSingleDerivedPathString(const SingleDerivedPath & p, Value & v)
{
    v.mkString(
        mkSingleDerivedPathStringRaw(p),
        NixStringContext{
            std::visit([](auto && v) -> NixStringContextElem { return v; }, p),
        },
        mem);
}

Value * Expr::maybeThunk(EvalState & state, Env & env)
{
    Value * v = state.allocValue();
    mkThunk(*v, env, this);
    return v;
}

Value * ExprVar::maybeThunk(EvalState & state, Env & env)
{
    Value * v = state.lookupVar(&env, *this, true);
    /* The value might not be initialised in the environment yet.
       In that case, ignore it. */
    if (v) {
        state.nrAvoided++;
        return v;
    }
    return Expr::maybeThunk(state, env);
}

Value * ExprString::maybeThunk(EvalState & state, Env & env)
{
    state.nrAvoided++;
    return &v;
}

Value * ExprInt::maybeThunk(EvalState & state, Env & env)
{
    state.nrAvoided++;
    return &v;
}

Value * ExprFloat::maybeThunk(EvalState & state, Env & env)
{
    state.nrAvoided++;
    return &v;
}

Value * ExprPath::maybeThunk(EvalState & state, Env & env)
{
    state.nrAvoided++;
    return &v;
}

namespace {

/**
 * A helper `Expr` class to lets us parse and evaluate Nix expressions
 * from a thunk, ensuring that every file is parsed/evaluated only
 * once (via the thunk stored in `EvalState::fileEvalCache`).
 */
struct ExprParseFile : Expr, gc
{
    // FIXME: make this a reference (see below).
    RootedPath path;
    bool mustBeTrivial;

    ExprParseFile(RootedPath & path, bool mustBeTrivial)
        : path(path)
        , mustBeTrivial(mustBeTrivial)
    {
    }

    void eval(EvalState & state, Env & env, Value & v) override
    {
        auto sp = path.sourcePath();
        printTalkative("evaluating file '%s'", sp);

        auto e = state.parseExprFromFile(path);

        try {
            auto dts =
                state.debugRepl
                    ? makeDebugTraceStacker(
                          state, *e, state.baseEnv, e->getPos(), "while evaluating the file '%s':", sp.to_string())
                    : nullptr;

            // Enforce that 'flake.nix' is a direct attrset, not a
            // computation.
            if (mustBeTrivial && !(dynamic_cast<ExprAttrs *>(e)))
                state.error<EvalError>("file '%s' must be an attribute set", sp).debugThrow();

            state.eval(e, v);
        } catch (Error & e) {
            state.addErrorTrace(e, "while evaluating the file '%s':", sp.to_string());
            throw;
        }
    }
};

} // namespace

void EvalState::evalFile(const RootedPath & path, Value & v, bool mustBeTrivial)
{
    auto sp = path.sourcePath();
    auto resolvedSp = getConcurrent(*importResolutionCache, sp);

    if (!resolvedSp) {
        /* Kind-aware overload: a Copyable accessor whose ancestor
           chain traverses a symlink whose target escapes the
           accessor root raises `AccessorBoundaryEscape`. */
        resolvedSp = resolveExprPath(path);
        importResolutionCache->emplace(sp, *resolvedSp);
    }

    if (auto v2 = getConcurrent(*fileEvalCache, *resolvedSp)) {
        forceValue(**v2, noPos);
        v = **v2;
        return;
    }

    /* The resolved SourcePath inherits the input's SourceRoot — the
       resolution (e.g. `./foo` → `./foo/default.nix`) stays within
       the same admission. */
    RootedPath resolvedPath{path.root, resolvedSp->path};

    Value * vExpr;
    // FIXME: put ExprParseFile on the stack instead of the heap once
    // https://github.com/NixOS/nix/pull/13930 is merged. That will ensure
    // the post-condition that `expr` is unreachable after
    // `forceValue()` returns.
    auto expr = new ExprParseFile{resolvedPath, mustBeTrivial};

    fileEvalCache->try_emplace_and_cvisit(
        *resolvedSp,
        nullptr,
        [&](auto & i) {
            vExpr = allocValue();
            vExpr->mkThunk(&baseEnv, expr);
            i.second = vExpr;
        },
        [&](auto & i) { vExpr = i.second; });

    forceValue(*vExpr, noPos);

    v = *vExpr;
}

void EvalState::resetFileCache()
{
    importResolutionCache->clear();
    fileEvalCache->clear();
    inputCache->clear();
    lookupPathResolved->clear();
    positions.clear();
    rootFS->invalidateCache();
}

void EvalState::eval(Expr * e, Value & v)
{
    e->eval(*this, baseEnv, v);
}

inline bool EvalState::evalBool(Env & env, Expr * e, const PosIdx pos, std::string_view errorCtx)
{
    try {
        Value v;
        e->eval(*this, env, v);
        if (v.type() != nBool)
            error<TypeError>(
                "expected a Boolean but found %1%: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
                .atPos(pos)
                .withFrame(env, *e)
                .debugThrow();
        return v.boolean();
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }
}

inline void EvalState::evalAttrs(Env & env, Expr * e, Value & v, const PosIdx pos, std::string_view errorCtx)
{
    try {
        e->eval(*this, env, v);
        if (v.type() != nAttrs)
            error<TypeError>(
                "expected a set but found %1%: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
                .withFrame(env, *e)
                .debugThrow();
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }
}

void Expr::eval(EvalState & state, Env & env, Value & v)
{
    unreachable();
}

void ExprInt::eval(EvalState & state, Env & env, Value & v)
{
    v = this->v;
}

void ExprFloat::eval(EvalState & state, Env & env, Value & v)
{
    v = this->v;
}

void ExprString::eval(EvalState & state, Env & env, Value & v)
{
    v = this->v;
}

void ExprPath::eval(EvalState & state, Env & env, Value & v)
{
    v = this->v;
}

Env * ExprAttrs::buildInheritFromEnv(EvalState & state, Env & up)
{
    Env & inheritEnv = state.mem.allocEnv(inheritFromExprs->size());
    inheritEnv.up = &up;

    Displacement displ = 0;
    for (auto from : *inheritFromExprs)
        inheritEnv.values[displ++] = from->maybeThunk(state, up);

    return &inheritEnv;
}

void ExprAttrs::eval(EvalState & state, Env & env, Value & v)
{
    auto bindings = state.buildBindings(attrs->size() + dynamicAttrs->size());
    auto dynamicEnv = &env;
    bool sort = false;

    if (recursive) {
        /* Create a new environment that contains the attributes in
           this `rec'. */
        Env & env2(state.mem.allocEnv(attrs->size()));
        env2.up = &env;
        dynamicEnv = &env2;
        Env * inheritEnv = inheritFromExprs ? buildInheritFromEnv(state, env2) : nullptr;

        AttrDefs::iterator overrides = attrs->find(state.s.overrides);
        bool hasOverrides = overrides != attrs->end();

        /* The recursive attributes are evaluated in the new
           environment, while the inherited attributes are evaluated
           in the original environment. */
        Displacement displ = 0;
        for (auto & i : *attrs) {
            Value * vAttr;
            if (hasOverrides && i.second.kind != AttrDef::Kind::Inherited) {
                vAttr = state.allocValue();
                mkThunk(*vAttr, *i.second.chooseByKind(&env2, &env, inheritEnv), i.second.e);
            } else
                vAttr = i.second.e->maybeThunk(state, *i.second.chooseByKind(&env2, &env, inheritEnv));
            env2.values[displ++] = vAttr;
            bindings.insert(i.first, vAttr, i.second.pos);
        }

        /* If the rec contains an attribute called `__overrides', then
           evaluate it, and add the attributes in that set to the rec.
           This allows overriding of recursive attributes, which is
           otherwise not possible.  (You can use the // operator to
           replace an attribute, but other attributes in the rec will
           still reference the original value, because that value has
           been substituted into the bodies of the other attributes.
           Hence we need __overrides.) */
        if (hasOverrides) {
            Value * vOverrides = (*bindings.bindings)[overrides->second.displ].value;
            state.forceAttrs(
                *vOverrides,
                [&]() { return vOverrides->determinePos(noPos); },
                "while evaluating the `__overrides` attribute");
            bindings.grow(state.buildBindings(bindings.capacity() + vOverrides->attrs()->size()));
            for (auto & i : *vOverrides->attrs()) {
                AttrDefs::iterator j = attrs->find(i.name);
                if (j != attrs->end()) {
                    (*bindings.bindings)[j->second.displ] = i;
                    env2.values[j->second.displ] = i.value;
                } else
                    bindings.push_back(i);
            }
            sort = true;
        }
    }

    else {
        Env * inheritEnv = inheritFromExprs ? buildInheritFromEnv(state, env) : nullptr;
        for (auto & i : *attrs)
            bindings.insert(
                i.first, i.second.e->maybeThunk(state, *i.second.chooseByKind(&env, &env, inheritEnv)), i.second.pos);
    }

    /* Dynamic attrs apply *after* rec and __overrides. */
    for (auto & i : *dynamicAttrs) {
        Value nameVal;
        i.nameExpr->eval(state, *dynamicEnv, nameVal);
        state.forceValue(nameVal, i.pos);
        if (nameVal.type() == nNull)
            continue;
        state.forceStringNoCtx(nameVal, i.pos, "while evaluating the name of a dynamic attribute");
        auto nameSym = state.symbols.create(nameVal.string_view());
        if (sort)
            // FIXME: inefficient
            bindings.bindings->sort();
        if (auto j = bindings.bindings->get(nameSym))
            state
                .error<EvalError>(
                    "dynamic attribute '%1%' already defined at %2%", state.symbols[nameSym], state.positions[j->pos])
                .atPos(i.pos)
                .withFrame(env, *this)
                .debugThrow();

        i.valueExpr->setName(nameSym);
        /* Keep sorted order so find can catch duplicates */
        bindings.insert(nameSym, i.valueExpr->maybeThunk(state, *dynamicEnv), i.pos);
        sort = true;
    }

    bindings.bindings->pos = pos;

    v.mkAttrs(sort ? bindings.finish() : bindings.alreadySorted());
}

void ExprLet::eval(EvalState & state, Env & env, Value & v)
{
    /* Create a new environment that contains the attributes in this
       `let'. */
    Env & env2(state.mem.allocEnv(attrs->attrs->size()));
    env2.up = &env;

    Env * inheritEnv = attrs->inheritFromExprs ? attrs->buildInheritFromEnv(state, env2) : nullptr;

    /* The recursive attributes are evaluated in the new environment,
       while the inherited attributes are evaluated in the original
       environment. */
    Displacement displ = 0;
    for (auto & i : *attrs->attrs) {
        env2.values[displ++] = i.second.e->maybeThunk(state, *i.second.chooseByKind(&env2, &env, inheritEnv));
    }

    auto dts = state.debugRepl
                   ? makeDebugTraceStacker(state, *this, env2, getPos(), "while evaluating a '%1%' expression", "let")
                   : nullptr;

    body->eval(state, env2, v);
}

void ExprList::eval(EvalState & state, Env & env, Value & v)
{
    auto list = state.buildList(elems.size());
    for (const auto & [n, v2] : enumerate(list))
        v2 = elems[n]->maybeThunk(state, env);
    v.mkList(list);
}

Value * ExprList::maybeThunk(EvalState & state, Env & env)
{
    if (elems.empty()) {
        return &Value::vEmptyList;
    }
    return Expr::maybeThunk(state, env);
}

void ExprVar::eval(EvalState & state, Env & env, Value & v)
{
    Value * v2 = state.lookupVar(&env, *this, false);
    state.forceValue(*v2, pos);
    v = *v2;
}

static std::string showAttrSelectionPath(EvalState & state, Env & env, std::span<const AttrName> attrPath)
{
    std::ostringstream out;
    bool first = true;
    for (auto & i : attrPath) {
        if (!first)
            out << '.';
        else
            first = false;
        try {
            out << state.symbols[getName(i, state, env)];
        } catch (Error & e) {
            assert(!i.symbol);
            out << "\"${";
            i.expr->show(state.symbols, out);
            out << "}\"";
        }
    }
    return out.str();
}

void ExprSelect::eval(EvalState & state, Env & env, Value & v)
{
    Value vTmp;
    PosIdx pos2;
    Value * vAttrs = &vTmp;

    e->eval(state, env, vTmp);

    try {
        auto dts = state.debugRepl ? makeDebugTraceStacker(
                                         state,
                                         *this,
                                         env,
                                         getPos(),
                                         "while evaluating the attribute '%1%'",
                                         showAttrSelectionPath(state, env, getAttrPath()))
                                   : nullptr;

        for (auto & i : getAttrPath()) {
            state.nrLookups++;
            const Attr * j;
            auto name = getName(i, state, env);
            if (def) {
                state.forceValue(*vAttrs, pos);
                if (vAttrs->type() != nAttrs || !(j = vAttrs->attrs()->get(name))) {
                    def->eval(state, env, v);
                    return;
                }
            } else {
                state.forceAttrs(*vAttrs, pos, "while selecting an attribute");
                if (!(j = vAttrs->attrs()->get(name))) {
                    StringSet allAttrNames;
                    for (auto & attr : *vAttrs->attrs())
                        allAttrNames.insert(std::string(state.symbols[attr.name]));
                    auto suggestions = Suggestions::bestMatches(allAttrNames, state.symbols[name]);
                    state.error<EvalError>("attribute '%1%' missing", state.symbols[name])
                        .atPos(pos)
                        .withSuggestions(suggestions)
                        .withFrame(env, *this)
                        .debugThrow();
                }
            }
            vAttrs = j->value;
            pos2 = j->pos;
            if (state.countCalls)
                state.attrSelects[pos2]++;
        }

        state.forceValue(*vAttrs, (pos2 ? pos2 : this->pos));

    } catch (Error & e) {
        if (pos2) {
            auto pos2r = state.positions[pos2];
            auto origin = std::get_if<RootedPath>(&pos2r.origin);
            if (!(origin && origin->sourcePath() == state.derivationInternal))
                state.addErrorTrace(
                    e, pos2, "while evaluating the attribute '%1%'", showAttrSelectionPath(state, env, getAttrPath()));
        }
        throw;
    }

    v = *vAttrs;
}

Symbol ExprSelect::evalExceptFinalSelect(EvalState & state, Env & env, Value & attrs)
{
    Value vTmp;
    Symbol name = getName(attrPathStart[nAttrPath - 1], state, env);

    if (nAttrPath == 1) {
        e->eval(state, env, vTmp);
    } else {
        ExprSelect init(*this);
        init.nAttrPath--;
        init.eval(state, env, vTmp);
    }
    attrs = vTmp;
    return name;
}

void ExprOpHasAttr::eval(EvalState & state, Env & env, Value & v)
{
    Value vTmp;
    Value * vAttrs = &vTmp;

    e->eval(state, env, vTmp);

    for (auto & i : attrPath) {
        state.forceValue(*vAttrs, getPos());
        const Attr * j;
        auto name = getName(i, state, env);
        if (vAttrs->type() == nAttrs && (j = vAttrs->attrs()->get(name))) {
            vAttrs = j->value;
        } else {
            v.mkBool(false);
            return;
        }
    }

    v.mkBool(true);
}

void ExprLambda::eval(EvalState & state, Env & env, Value & v)
{
    v.mkLambda(&env, this);
}

void EvalState::callFunction(Value & fun, std::span<Value *> args, Value & vRes, const PosIdx pos)
{
    auto _level = addCallDepth(pos);

    auto neededHooks = profiler.getNeededHooks();
    if (neededHooks.test(EvalProfiler::preFunctionCall)) [[unlikely]]
        profiler.preFunctionCallHook(*this, fun, args, pos);

    Finally traceExit_{[&]() {
        if (profiler.getNeededHooks().test(EvalProfiler::postFunctionCall)) [[unlikely]]
            profiler.postFunctionCallHook(*this, fun, args, pos);
    }};

    forceValue(fun, pos);

    Value vCur(fun);

    auto makeAppChain = [&]() {
        for (auto arg : args) {
            auto fun2 = allocValue();
            *fun2 = vCur;
            vCur.mkPrimOpApp(fun2, arg);
        }
        vRes = vCur;
    };

    const Attr * functor;

    while (args.size() > 0) {

        if (vCur.isLambda()) {

            ExprLambda & lambda(*vCur.lambda().fun);

            auto size = (!lambda.arg ? 0 : 1) + (lambda.getFormals() ? lambda.getFormals()->formals.size() : 0);
            Env & env2(mem.allocEnv(size));
            env2.up = vCur.lambda().env;

            Displacement displ = 0;

            if (auto formals = lambda.getFormals()) {
                try {
                    forceAttrs(*args[0], lambda.pos, "while evaluating the value passed for the lambda argument");
                } catch (Error & e) {
                    if (pos)
                        e.addTrace(positions[pos], "from call site");
                    throw;
                }

                if (lambda.arg)
                    env2.values[displ++] = args[0];

                /* For each formal argument, get the actual argument.  If
                   there is no matching actual argument but the formal
                   argument has a default, use the default. */
                size_t attrsUsed = 0;
                for (auto & i : formals->formals) {
                    auto j = args[0]->attrs()->get(i.name);
                    if (!j) {
                        if (!i.def) {
                            error<TypeError>(
                                "function '%1%' called without required argument '%2%'",
                                (lambda.name ? std::string(symbols[lambda.name]) : "anonymous lambda"),
                                symbols[i.name])
                                .atPos(lambda.pos)
                                .withTrace(pos, "from call site")
                                .withFrame(*vCur.lambda().env, lambda)
                                .debugThrow();
                        }
                        env2.values[displ++] = i.def->maybeThunk(*this, env2);
                    } else {
                        attrsUsed++;
                        env2.values[displ++] = j->value;
                    }
                }

                /* Check that each actual argument is listed as a formal
                   argument (unless the attribute match specifies a `...'). */
                if (!formals->ellipsis && attrsUsed != args[0]->attrs()->size()) {
                    /* Nope, so show the first unexpected argument to the
                       user. */
                    for (auto & i : *args[0]->attrs())
                        if (!formals->has(i.name)) {
                            StringSet formalNames;
                            for (auto & formal : formals->formals)
                                formalNames.insert(std::string(symbols[formal.name]));
                            auto suggestions = Suggestions::bestMatches(formalNames, symbols[i.name]);
                            error<TypeError>(
                                "function '%1%' called with unexpected argument '%2%'",
                                (lambda.name ? std::string(symbols[lambda.name]) : "anonymous lambda"),
                                symbols[i.name])
                                .atPos(lambda.pos)
                                .withTrace(pos, "from call site")
                                .withSuggestions(suggestions)
                                .withFrame(*vCur.lambda().env, lambda)
                                .debugThrow();
                        }
                    unreachable();
                }
            } else {
                env2.values[displ++] = args[0];
            }

            nrFunctionCalls++;
            if (countCalls)
                incrFunctionCall(&lambda);

            /* Evaluate the body. */
            try {
                auto dts = debugRepl
                               ? makeDebugTraceStacker(
                                     *this,
                                     *lambda.body,
                                     env2,
                                     lambda.pos,
                                     "while calling %s",
                                     lambda.name ? concatStrings("'", symbols[lambda.name], "'") : "anonymous lambda")
                               : nullptr;

                lambda.body->eval(*this, env2, vCur);
            } catch (Error & e) {
                if (loggerSettings.showTrace.get()) {
                    addErrorTrace(
                        e,
                        lambda.pos,
                        "while calling %s",
                        lambda.name ? concatStrings("'", symbols[lambda.name], "'") : "anonymous lambda");
                    if (pos)
                        addErrorTrace(e, pos, "from call site");
                }
                throw;
            }

            args = args.subspan(1);
        }

        else if (vCur.isPrimOp()) {

            size_t argsLeft = vCur.primOp()->arity;

            if (args.size() < argsLeft) {
                /* We don't have enough arguments, so create a tPrimOpApp chain. */
                makeAppChain();
                return;
            } else {
                /* We have all the arguments, so call the primop. */
                auto * fn = vCur.primOp();

                nrPrimOpCalls++;
                if (countCalls)
                    primOpCalls[fn->name]++;

                try {
                    fn->impl(*this, vCur.determinePos(noPos), args.data(), vCur);
                } catch (Error & e) {
                    if (fn->addTrace)
                        addErrorTrace(e, pos, "while calling the '%1%' builtin", fn->name);
                    throw;
                }

                args = args.subspan(argsLeft);
            }
        }

        else if (vCur.isPrimOpApp()) {
            /* Figure out the number of arguments still needed. */
            size_t argsDone = 0;
            Value * primOp = &vCur;
            while (primOp->isPrimOpApp()) {
                argsDone++;
                primOp = primOp->primOpApp().left;
            }
            assert(primOp->isPrimOp());
            auto arity = primOp->primOp()->arity;
            auto argsLeft = arity - argsDone;

            if (args.size() < argsLeft) {
                /* We still don't have enough arguments, so extend the tPrimOpApp chain. */
                makeAppChain();
                return;
            } else {
                /* We have all the arguments, so call the primop with
                   the previous and new arguments. */

                Value * vArgs[maxPrimOpArity];
                auto n = argsDone;
                for (Value * arg = &vCur; arg->isPrimOpApp(); arg = arg->primOpApp().left)
                    vArgs[--n] = arg->primOpApp().right;

                for (size_t i = 0; i < argsLeft; ++i)
                    vArgs[argsDone + i] = args[i];

                auto fn = primOp->primOp();
                nrPrimOpCalls++;
                if (countCalls)
                    primOpCalls[fn->name]++;

                try {
                    // TODO:
                    // 1. Unify this and above code. Heavily redundant.
                    // 2. Create a fake env (arg1, arg2, etc.) and a fake expr (arg1: arg2: etc: builtins.name arg1 arg2
                    // etc)
                    //    so the debugger allows to inspect the wrong parameters passed to the builtin.
                    fn->impl(*this, vCur.determinePos(noPos), vArgs, vCur);
                } catch (Error & e) {
                    if (fn->addTrace)
                        addErrorTrace(e, pos, "while calling the '%1%' builtin", fn->name);
                    throw;
                }

                args = args.subspan(argsLeft);
            }
        }

        else if (vCur.type() == nAttrs && (functor = vCur.attrs()->get(s.functor))) {
            /* 'vCur' may be allocated on the stack of the calling
               function, but for functors we may keep a reference, so
               heap-allocate a copy and use that instead. */
            Value * args2[] = {allocValue(), args[0]};
            *args2[0] = vCur;
            try {
                callFunction(*functor->value, args2, vCur, functor->pos);
            } catch (Error & e) {
                e.addTrace(positions[pos], "while calling a functor (an attribute set with a '__functor' attribute)");
                throw;
            }
            args = args.subspan(1);
        }

        else
            error<TypeError>(
                "attempt to call something which is not a function but %1%: %2%",
                showType(vCur),
                ValuePrinter(*this, vCur, errorPrintOptions))
                .atPos(pos)
                .debugThrow();
    }

    vRes = vCur;
}

void ExprCall::eval(EvalState & state, Env & env, Value & v)
{
    auto dts =
        state.debugRepl ? makeDebugTraceStacker(state, *this, env, getPos(), "while calling a function") : nullptr;

    Value vFun;
    fun->eval(state, env, vFun);

    // Empirical arity of Nixpkgs lambdas by regex e.g. ([a-zA-Z]+:(\s|(/\*.*\/)|(#.*\n))*){5}
    // 2: over 4000
    // 3: about 300
    // 4: about 60
    // 5: under 10
    // This excluded attrset lambdas (`{...}:`). Contributions of mixed lambdas appears insignificant at ~150 total.
    SmallValueVector<4> vArgs(args->size());
    for (size_t i = 0; i < args->size(); ++i)
        vArgs[i] = (*args)[i]->maybeThunk(state, env);

    state.callFunction(vFun, vArgs, v, pos);
}

// Lifted out of callFunction() because it creates a temporary that
// prevents tail-call optimisation.
void EvalState::incrFunctionCall(ExprLambda * fun)
{
    functionCalls[fun]++;
}

void EvalState::autoCallFunction(const Bindings & args, Value & fun, Value & res)
{
    auto pos = fun.determinePos(noPos);

    forceValue(fun, pos);

    if (fun.type() == nAttrs) {
        auto found = fun.attrs()->get(s.functor);
        if (found) {
            Value * v = allocValue();
            callFunction(*found->value, fun, *v, pos);
            forceValue(*v, pos);
            return autoCallFunction(args, *v, res);
        }
    }

    if (!fun.isLambda() || !fun.lambda().fun->getFormals()) {
        res = fun;
        return;
    }
    auto formals = fun.lambda().fun->getFormals();

    auto attrs = buildBindings(std::max(static_cast<uint32_t>(formals->formals.size()), args.size()));

    if (formals->ellipsis) {
        // If the formals have an ellipsis (eg the function accepts extra args) pass
        // all available automatic arguments (which includes arguments specified on
        // the command line via --arg/--argstr)
        for (auto & v : args)
            attrs.insert(v);
    } else {
        // Otherwise, only pass the arguments that the function accepts
        for (auto & i : formals->formals) {
            auto j = args.get(i.name);
            if (j) {
                attrs.insert(*j);
            } else if (!i.def) {
                error<MissingArgumentError>(
                    R"(cannot evaluate a function that has an argument without a value ('%1%')
Nix attempted to evaluate a function as a top level expression; in
this case it must have its arguments supplied either by default
values, or passed explicitly with '--arg' or '--argstr'. See
https://nix.dev/manual/nix/stable/language/syntax.html#functions.)",
                    symbols[i.name])
                    .atPos(i.pos)
                    .withFrame(*fun.lambda().env, *fun.lambda().fun)
                    .debugThrow();
            }
        }
    }

    callFunction(fun, allocValue()->mkAttrs(attrs), res, pos);
}

void ExprWith::eval(EvalState & state, Env & env, Value & v)
{
    Env & env2(state.mem.allocEnv(1));
    env2.up = &env;
    env2.values[0] = attrs->maybeThunk(state, env);

    body->eval(state, env2, v);
}

void ExprIf::eval(EvalState & state, Env & env, Value & v)
{
    // We cheat in the parser, and pass the position of the condition as the position of the if itself.
    (state.evalBool(env, cond, pos, "while evaluating a branch condition") ? then : else_)->eval(state, env, v);
}

void ExprAssert::eval(EvalState & state, Env & env, Value & v)
{
    if (!state.evalBool(env, cond, pos, "in the condition of the assert statement")) {
        std::ostringstream out;
        cond->show(state.symbols, out);
        auto exprStr = out.view();

        if (auto eq = dynamic_cast<ExprOpEq *>(cond)) {
            try {
                Value v1;
                eq->e1->eval(state, env, v1);
                Value v2;
                eq->e2->eval(state, env, v2);
                state.assertEqValues(v1, v2, eq->pos, "in an equality assertion");
            } catch (AssertionError & e) {
                e.addTrace(state.positions[pos], "while evaluating the condition of the assertion '%s'", exprStr);
                throw;
            }
        }

        state.error<AssertionError>("assertion '%1%' failed", exprStr).atPos(pos).withFrame(env, *this).debugThrow();
    }
    body->eval(state, env, v);
}

void ExprOpNot::eval(EvalState & state, Env & env, Value & v)
{
    v.mkBool(!state.evalBool(env, e, getPos(), "in the argument of the not operator")); // XXX: FIXME: !
}

void ExprOpEq::eval(EvalState & state, Env & env, Value & v)
{
    Value v1;
    e1->eval(state, env, v1);
    Value v2;
    e2->eval(state, env, v2);
    v.mkBool(state.eqValues(v1, v2, pos, "while testing two values for equality"));
}

void ExprOpNEq::eval(EvalState & state, Env & env, Value & v)
{
    Value v1;
    e1->eval(state, env, v1);
    Value v2;
    e2->eval(state, env, v2);
    v.mkBool(!state.eqValues(v1, v2, pos, "while testing two values for inequality"));
}

void ExprOpAnd::eval(EvalState & state, Env & env, Value & v)
{
    v.mkBool(
        state.evalBool(env, e1, pos, "in the left operand of the AND (&&) operator")
        && state.evalBool(env, e2, pos, "in the right operand of the AND (&&) operator"));
}

void ExprOpOr::eval(EvalState & state, Env & env, Value & v)
{
    v.mkBool(
        state.evalBool(env, e1, pos, "in the left operand of the OR (||) operator")
        || state.evalBool(env, e2, pos, "in the right operand of the OR (||) operator"));
}

void ExprOpImpl::eval(EvalState & state, Env & env, Value & v)
{
    v.mkBool(
        !state.evalBool(env, e1, pos, "in the left operand of the IMPL (->) operator")
        || state.evalBool(env, e2, pos, "in the right operand of the IMPL (->) operator"));
}

void ExprOpUpdate::eval(EvalState & state, Value & v, Value & v1, Value & v2)
{
    state.nrOpUpdates++;

    const Bindings & bindings1 = *v1.attrs();
    if (bindings1.empty()) {
        v = v2;
        return;
    }

    const Bindings & bindings2 = *v2.attrs();
    if (bindings2.empty()) {
        v = v1;
        return;
    }

    /* Simple heuristic for determining whether attrs2 should be "layered" on top of
       attrs1 instead of copying to a new Bindings. */
    bool shouldLayer = [&]() -> bool {
        if (bindings1.isLayerListFull())
            return false;

        if (bindings2.size() > state.settings.bindingsUpdateLayerRhsSizeThreshold)
            return false;

        return true;
    }();

    if (shouldLayer) {
        auto attrs = state.buildBindings(bindings2.size());
        attrs.layerOnTopOf(bindings1);

        std::ranges::copy(bindings2, std::back_inserter(attrs));
        v.mkAttrs(attrs.alreadySorted());

        state.nrOpUpdateValuesCopied += bindings2.size();
        return;
    }

    auto attrs = state.buildBindings(bindings1.size() + bindings2.size());

    /* Merge the sets, preferring values from the second set.  Make
       sure to keep the resulting vector in sorted order. */
    auto i = bindings1.begin();
    auto j = bindings2.begin();

    while (i != bindings1.end() && j != bindings2.end()) {
        if (i->name == j->name) {
            attrs.insert(*j);
            ++i;
            ++j;
        } else if (i->name < j->name) {
            attrs.insert(*i);
            ++i;
        } else {
            attrs.insert(*j);
            ++j;
        }
    }

    while (i != bindings1.end()) {
        attrs.insert(*i);
        ++i;
    }

    while (j != bindings2.end()) {
        attrs.insert(*j);
        ++j;
    }

    v.mkAttrs(attrs.alreadySorted());

    state.nrOpUpdateValuesCopied += v.attrs()->size();
}

void ExprOpUpdate::eval(EvalState & state, Env & env, Value & v)
{
    UpdateQueue q;
    evalForUpdate(state, env, q);

    Value vTmp;
    vTmp.mkAttrs(&Bindings::emptyBindings);

    for (auto & rhs : std::views::reverse(q)) {
        /* Remember that queue is sorted rightmost attrset first. */
        eval(state, /*v=*/vTmp, /*v1=*/vTmp, /*v2=*/rhs);
    }

    v = vTmp;
}

void Expr::evalForUpdate(EvalState & state, Env & env, UpdateQueue & q, std::string_view errorCtx)
{
    Value v;
    state.evalAttrs(env, this, v, getPos(), errorCtx);
    q.push_back(v);
}

void ExprOpUpdate::evalForUpdate(EvalState & state, Env & env, UpdateQueue & q)
{
    /* Output rightmost attrset first to the merge queue as the one
       with the most priority. */
    e2->evalForUpdate(state, env, q, "in the right operand of the update (//) operator");
    e1->evalForUpdate(state, env, q, "in the left operand of the update (//) operator");
}

void ExprOpUpdate::evalForUpdate(EvalState & state, Env & env, UpdateQueue & q, std::string_view errorCtx)
{
    evalForUpdate(state, env, q);
}

void ExprOpConcatLists::eval(EvalState & state, Env & env, Value & v)
{
    Value v1;
    e1->eval(state, env, v1);
    Value v2;
    e2->eval(state, env, v2);
    Value * lists[2] = {&v1, &v2};
    state.concatLists(v, lists, pos, "while evaluating one of the elements to concatenate");
}

void EvalState::concatLists(Value & v, std::span<Value * const> lists, const PosIdx pos, std::string_view errorCtx)
{
    nrListConcats++;

    Value * nonEmpty = nullptr;
    size_t len = 0;
    for (auto * list : lists) {
        forceList(*list, pos, errorCtx);
        auto l = list->listSize();
        len += l;
        if (l)
            nonEmpty = list;
    }

    if (nonEmpty && len == nonEmpty->listSize()) {
        v = *nonEmpty;
        return;
    }

    auto list = buildList(len);
    auto out = list.elems;
    size_t pos2 = 0;
    for (auto * l : lists) {
        auto listView = l->listView();
        auto n = listView.size();
        if (n)
            memcpy(out + pos2, listView.data(), n * sizeof(Value *));
        pos2 += n;
    }
    v.mkList(list);
}

void ExprConcatStrings::eval(EvalState & state, Env & env, Value & v)
{
    NixStringContext context;
    std::vector<BackedStringView> strings;
    size_t sSize = 0;
    NixInt n{0};
    NixFloat nf = 0;

    bool first = !forceString;
    ValueType firstType = nString;
    std::shared_ptr<SourceRoot> firstPathRoot;

    // List of returned strings. References to these Values must NOT be persisted.
    SmallTemporaryValueVector<conservativeStackReservation> values(es.size());
    Value * vTmpP = values.data();

    for (auto & [i_pos, i] : es) {
        Value & vTmp = *vTmpP++;
        i->eval(state, env, vTmp);

        /* If the first element is a path, then the result will also
           be a path, we don't copy anything (yet - that's done later,
           since paths are copied when they are used in a derivation),
           and none of the strings are allowed to have contexts. */
        if (first) {
            firstType = vTmp.type();
            /* Capture the first path's accessor so the concatenated
               result stays rooted on it. Re-rooting on `rootFS` (the
               historical default) erases custom accessors like fetched
               trees, which the rest of the path then can't be read
               through. */
            if (firstType == nPath)
                firstPathRoot = vTmp.pathRoot()->shared_from_this();
        }

        /* Reject a non-first path operand whose SourceRoot is
           Copyable. `./foo + fetchTreeResult` would stringify the
           fetched-tree operand through `coerceToString`'s Copyable
           arm, walking the entire tree to splice a storepath
           substring into the joined path -- the resulting subpath
           isn't a meaningful address.

           The first operand being Copyable is fine: it just sets
           the result's root, no walk involved (`fetchTreeResult +
           "/sub"` stays valid because the second operand is a
           string, not a Copyable path that needs walking).

           The grandfathered System+System form
           (`/var/lib + /var/log == /var/lib/var/log`) is
           preserved. Internal-rooted operands fall through to
           `coerceToString`'s Internal-reject (C6), with the same
           error shape. */
        if (!first && firstType == nPath && vTmp.type() == nPath && vTmp.pathRoot()->kind == SourceRootKind::Copyable) {
            state
                .error<EvalError>(
                    "concatenating a fetched-tree path onto a path is not supported -- "
                    "use string interpolation (\"${a}/${b}\") if you need to join paths "
                    "involving fetched trees")
                .atPos(i_pos)
                .withFrame(env, *this)
                .debugThrow();
        }

        if (firstType == nInt) {
            if (vTmp.type() == nInt) {
                auto newN = n + vTmp.integer();
                if (auto checked = newN.valueChecked(); checked.has_value()) {
                    n = NixInt(*checked);
                } else {
                    state.error<EvalError>("integer overflow in adding %1% + %2%", n, vTmp.integer())
                        .atPos(i_pos)
                        .debugThrow();
                }
            } else if (vTmp.type() == nFloat) {
                // Upgrade the type from int to float;
                firstType = nFloat;
                nf = n.value;
                nf += vTmp.fpoint();
            } else
                state.error<EvalError>("cannot add %1% to an integer", showType(vTmp))
                    .atPos(i_pos)
                    .withFrame(env, *this)
                    .debugThrow();
        } else if (firstType == nFloat) {
            if (vTmp.type() == nInt) {
                nf += vTmp.integer().value;
            } else if (vTmp.type() == nFloat) {
                nf += vTmp.fpoint();
            } else
                state.error<EvalError>("cannot add %1% to a float", showType(vTmp))
                    .atPos(i_pos)
                    .withFrame(env, *this)
                    .debugThrow();
        } else {
            if (strings.empty())
                strings.reserve(es.size());
            /* skip canonization of first path, which would only be not
            canonized in the first place if it's coming from a ./${foo} type
            path */
            auto part = state.coerceToString(
                i_pos, vTmp, context, "while evaluating a path segment", false, firstType == nString, !first);
            sSize += part->size();
            strings.emplace_back(std::move(part));
        }

        first = false;
    }

    if (firstType == nInt) {
        v.mkInt(n);
    } else if (firstType == nFloat) {
        v.mkFloat(nf);
    } else if (firstType == nPath) {
        if (!context.empty())
            state.error<EvalError>("a string that refers to a store path cannot be appended to a path")
                .atPos(pos)
                .withFrame(env, *this)
                .debugThrow();
        std::string resultStr;
        resultStr.reserve(sSize);
        for (const auto & part : strings) {
            resultStr += *part;
        }

        /* Part 1 of the three-part escape handling: lexical check
           for Copyable-rooted path concatenation. `CanonPath(raw)`
           silently clamps `..`-past-root. For Copyable accessors
           (fetched trees), the clamped result lands at a path
           *inside* the tree that the user almost certainly didn't
           mean. Walk the components lexically and reject if depth
           would go negative.

           No I/O — no symlink resolution, no accessor reads. The
           check is pure-language, matching the operator's
           character (the reviewer's objection to cff28d23f was
           the I/O at concat time, not the principle of catching
           escapes).

           System keeps historical silent-clamp; the long-standing
           `./foo + "../bar"` idiom depends on it. Internal can't
           reach here (path concat preserves the first arg's root;
           Internal-rooted paths aren't admitted as user-visible
           starting points in practice).

           Symlink-target escapes (where the joined string has no
           literal `..` but a symlink in the path traverses one)
           are NOT caught here — those need the read-time
           kind-aware resolver in Part 2 (which fires when the
           resulting path is actually read). The two layers
           compose: Part 1 catches obvious lexical escapes early
           and cheaply; Part 2 catches symlink-spliced escapes at
           the read site. */
        if (firstPathRoot->kind == SourceRootKind::Copyable) {
            /* Reuse libutil's `tokenizeString`: it collapses runs of
               separators and skips empty components naturally, so the
               loop here only handles `.` / `..` semantics. Container
               choice (`std::vector<std::string>`) differs from
               `resolveSymlinks`'s `std::list<std::string>` because
               this loop walks the components forward and never
               splices — vector is the right shape; list buys
               splice support we don't need. The hand-rolled walker
               the previous shape used was a parallel implementation
               of equivalent component splitting; the simpler shape
               here keeps the lexical check honest about that. The
               trade-off vs. the hand-rolled walker is one
               `std::string` allocation per component plus a vector
               per call — fine for Copyable path-concat, which is
               not a hot path (concats per eval are dwarfed by other
               allocations in `ExprConcatStrings`). A
               `tokenizeString<std::vector<std::string_view>>` extern
               instantiation could lift the per-component allocation
               if needed in the future. */
            int depth = 0;
            for (const auto & component : tokenizeString<std::vector<std::string>>(resultStr, "/")) {
                if (component == ".")
                    continue;
                if (component == "..") {
                    if (depth == 0) {
                        state
                            .error<EvalError>(
                                "concatenated path '%s' would escape the source tree at %s",
                                resultStr,
                                firstPathRoot->accessor->showPath(CanonPath::root))
                            .atPos(pos)
                            .withFrame(env, *this)
                            .debugThrow();
                    }
                    depth--;
                } else
                    depth++;
            }
        }

        v.mkPath(RootedPath{ref(firstPathRoot), CanonPath(resultStr)}, state.mem);
    } else {
        auto & resultStr = StringData::alloc(state.mem, sSize);
        auto * tmp = resultStr.data();
        for (const auto & part : strings) {
            std::memcpy(tmp, part->data(), part->size());
            tmp += part->size();
        }
        *tmp = '\0';
        v.mkStringMove(resultStr, context, state.mem);
    }
}

void ExprPos::eval(EvalState & state, Env & env, Value & v)
{
    state.mkPos(v, pos);
}

void ExprBlackHole::eval(EvalState & state, [[maybe_unused]] Env & env, Value & v)
{
    throwInfiniteRecursionError(state, v);
}

[[gnu::noinline]] [[noreturn]] void ExprBlackHole::throwInfiniteRecursionError(EvalState & state, Value & v)
{
    state.error<InfiniteRecursionError>("infinite recursion encountered").atPos(v.determinePos(noPos)).debugThrow();
}

// always force this to be separate, otherwise forceValue may inline it and take
// a massive perf hit
[[gnu::noinline]]
void EvalState::handleEvalExceptionForThunk(Env * env, Expr * expr, Value & v, const PosIdx pos)
{
    if (!env)
        tryFixupBlackHolePos(v, pos);

    auto e = std::current_exception();
    Value * recovery = nullptr;
    try {
        std::rethrow_exception(e);
    } catch (const RecoverableEvalError & e) {
        recovery = allocValue();
    } catch (...) {
    }
    if (recovery) {
        recovery->mkThunk(env, expr);
    }
    v.mkFailed(e, recovery);
}

[[gnu::noinline]]
void EvalState::handleEvalExceptionForApp(Value & v, const Value & savedApp)
{
    auto e = std::current_exception();
    Value * recovery = nullptr;
    try {
        std::rethrow_exception(e);
    } catch (const RecoverableEvalError & e) {
        recovery = allocValue();
    } catch (...) {
    }
    if (recovery) {
        *recovery = savedApp;
    }
    v.mkFailed(e, recovery);
}

[[gnu::noinline]]
void EvalState::handleEvalFailed(Value & v, const PosIdx pos)
{
    assert(v.isFailed());
    if (auto recoveryValue = v.failed().recoveryValue) {
        v = *recoveryValue;
        forceValue(v, pos);
    } else {
        v.failed().rethrow();
    }
}

void EvalState::tryFixupBlackHolePos(Value & v, PosIdx pos)
{
    if (!v.isBlackhole())
        return;
    auto e = std::current_exception();
    try {
        std::rethrow_exception(e);
    } catch (InfiniteRecursionError & e) {
        if (!e.hasPos())
            e.atPos(positions[pos]);
    } catch (...) {
    }
}

void EvalState::forceValueDeep(Value & v)
{
    std::set<const Value *> seen;

    [&, &state(*this)](this const auto & recurse, Value & v) {
        auto _level = state.addCallDepth(v.determinePos(noPos));

        if (!seen.insert(&v).second)
            return;

        state.forceValue(v, v.determinePos(noPos));

        if (v.type() == nAttrs) {
            for (auto & i : *v.attrs())
                try {
                    // If the value is a thunk, we're evaling. Otherwise no trace necessary.
                    auto dts = state.debugRepl && i.value->isThunk() ? makeDebugTraceStacker(
                                                                           state,
                                                                           *i.value->thunk().expr,
                                                                           *i.value->thunk().env,
                                                                           i.pos,
                                                                           "while evaluating the attribute '%1%'",
                                                                           state.symbols[i.name])
                                                                     : nullptr;

                    recurse(*i.value);
                } catch (Error & e) {
                    state.addErrorTrace(e, i.pos, "while evaluating the attribute '%1%'", state.symbols[i.name]);
                    throw;
                }
        }

        else if (v.isList()) {
            size_t index = 0;
            for (auto v2 : v.listView())
                try {
                    recurse(*v2);
                    index++;
                } catch (Error & e) {
                    state.addErrorTrace(e, "while evaluating list element at index %1%", index);
                    throw;
                }
        }
    }(v);
}

NixInt EvalState::forceInt(Value & v, const PosIdx pos, std::string_view errorCtx)
{
    try {
        forceValue(v, pos);
        if (v.type() != nInt)
            error<TypeError>(
                "expected an integer but found %1%: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
                .atPos(pos)
                .debugThrow();
        return v.integer();
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }

    return v.integer();
}

NixFloat EvalState::forceFloat(Value & v, const PosIdx pos, std::string_view errorCtx)
{
    try {
        forceValue(v, pos);
        if (v.type() == nInt)
            return v.integer().value;
        else if (v.type() != nFloat)
            error<TypeError>(
                "expected a float but found %1%: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
                .atPos(pos)
                .debugThrow();
        return v.fpoint();
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }
}

bool EvalState::forceBool(Value & v, const PosIdx pos, std::string_view errorCtx)
{
    try {
        forceValue(v, pos);
        if (v.type() != nBool)
            error<TypeError>(
                "expected a Boolean but found %1%: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
                .atPos(pos)
                .debugThrow();
        return v.boolean();
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }

    return v.boolean();
}

const Attr * EvalState::getAttr(Symbol attrSym, const Bindings * attrSet, std::string_view errorCtx)
{
    auto value = attrSet->get(attrSym);
    if (!value) {
        error<TypeError>("attribute '%s' missing", symbols[attrSym]).withTrace(noPos, errorCtx).debugThrow();
    }
    return value;
}

bool EvalState::isFunctor(const Value & fun) const
{
    return fun.type() == nAttrs && fun.attrs()->get(s.functor);
}

void EvalState::forceFunction(Value & v, const PosIdx pos, std::string_view errorCtx)
{
    try {
        forceValue(v, pos);
        if (v.type() != nFunction && !isFunctor(v))
            error<TypeError>(
                "expected a function but found %1%: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
                .atPos(pos)
                .debugThrow();
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }
}

std::string_view EvalState::forceString(Value & v, const PosIdx pos, std::string_view errorCtx)
{
    try {
        forceValue(v, pos);
        if (v.type() != nString)
            error<TypeError>(
                "expected a string but found %1%: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
                .atPos(pos)
                .debugThrow();
        return v.string_view();
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }
}

void copyContext(const Value & v, NixStringContext & context, const ExperimentalFeatureSettings & xpSettings)
{
    if (auto * ctx = v.context())
        for (auto * elem : *ctx)
            context.insert(NixStringContextElem::parse(elem->view(), xpSettings));
}

std::string_view EvalState::forceString(
    Value & v,
    NixStringContext & context,
    const PosIdx pos,
    std::string_view errorCtx,
    const ExperimentalFeatureSettings & xpSettings)
{
    auto s = forceString(v, pos, errorCtx);
    copyContext(v, context, xpSettings);
    return s;
}

std::string_view EvalState::forceStringNoCtx(Value & v, const PosIdx pos, std::string_view errorCtx)
{
    auto s = forceString(v, pos, errorCtx);
    if (v.context()) {
        auto ctxElem = NixStringContextElem::parse((*v.context()->begin())->view());
        error<EvalError>(
            "the string '%1%' is not allowed to refer to a store path (such as '%2%')",
            v.string_view(),
            ctxElem.display(*store))
            .withTrace(pos, errorCtx)
            .debugThrow();
    }
    return s;
}

bool EvalState::isDerivation(Value & v)
{
    if (v.type() != nAttrs)
        return false;
    auto i = v.attrs()->get(s.type);
    if (!i)
        return false;
    forceValue(*i->value, i->pos);
    if (i->value->type() != nString)
        return false;
    return i->value->string_view().compare("derivation") == 0;
}

std::optional<std::string>
EvalState::tryAttrsToString(const PosIdx pos, Value & v, NixStringContext & context, bool coerceMore, bool copyToStore)
{
    auto i = v.attrs()->get(s.toString);
    if (i) {
        Value v1;
        callFunction(*i->value, v, v1, pos);
        return coerceToString(
                   pos,
                   v1,
                   context,
                   "while evaluating the result of the `__toString` attribute",
                   coerceMore,
                   copyToStore)
            .toOwned();
    }

    return {};
}

BackedStringView EvalState::coerceToString(
    const PosIdx pos,
    Value & v,
    NixStringContext & context,
    std::string_view errorCtx,
    bool coerceMore,
    bool copyToStore,
    bool canonicalizePath)
{
    auto _level = addCallDepth(pos);

    forceValue(v, pos);

    if (v.type() == nString) {
        copyContext(v, context);
        return v.string_view();
    }

    if (v.type() == nPath) {
        if (!canonicalizePath && !copyToStore) {
            // FIXME: hack to preserve path literals that end in a
            // slash, as in /foo/${x}.
            return v.pathStrView();
        } else if (copyToStore) {
            return store->printStorePath(copyPathToStore(context, v.rootedPath()));
        }
        /* `toString` (no copy-to-store) on a path Value: dispatch on
           the SourceRoot's kind set at admission. */
        auto rp = v.rootedPath();
        switch (rp.root->kind) {
        case SourceRootKind::Internal:
            error<EvalError>(
                "cannot coerce a path on an internal accessor to a string at '%1%'",
                rp.root->accessor->showPath(rp.path))
                .withTrace(pos, errorCtx)
                .debugThrow();
        case SourceRootKind::System:
            return std::string{rp.path.abs()};
        case SourceRootKind::Copyable: {
            /* Fetched-tree path: materialise the tree's root (a
               storepath) and append the subpath. Matches what
               `"${...}"` interpolation produces, so `toString p ==
               "${p}"` for fetched-tree paths. The `srcToStore`
               cache short-circuits subsequent calls. */
            auto storePath = [&]() {
                try {
                    return copyPathToStore(context, RootedPath{rp.root, CanonPath::root});
                } catch (Error & e) {
                    e.addTrace(
                        positions[pos], "while coercing path '%s' on a fetched source to a string", rp.path.abs());
                    throw;
                }
            }();
            return store->printStorePath(storePath) + rp.path.absOrEmpty();
        }
        }
        unreachable();
    }

    if (v.type() == nAttrs) {
        auto maybeString = tryAttrsToString(pos, v, context, coerceMore, copyToStore);
        if (maybeString)
            return std::move(*maybeString);
        auto i = v.attrs()->get(s.outPath);
        if (!i) {
            error<TypeError>(
                "cannot coerce %1% to a string: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
                .withTrace(pos, errorCtx)
                .debugThrow();
        }
        return coerceToString(pos, *i->value, context, errorCtx, coerceMore, copyToStore, canonicalizePath);
    }

    if (v.type() == nExternal) {
        try {
            return v.external()->coerceToString(*this, pos, context, coerceMore, copyToStore);
        } catch (Error & e) {
            e.addTrace(nullptr, errorCtx);
            throw;
        }
    }

    if (coerceMore) {
        /* Note that `false' is represented as an empty string for
           shell scripting convenience, just like `null'. */
        if (v.type() == nBool && v.boolean())
            return "1";
        if (v.type() == nBool && !v.boolean())
            return "";
        if (v.type() == nInt)
            return std::to_string(v.integer().value);
        if (v.type() == nFloat)
            return std::to_string(v.fpoint());
        if (v.type() == nNull)
            return "";

        if (v.isList()) {
            std::string result;
            auto listView = v.listView();
            for (auto [n, v2] : enumerate(listView)) {
                try {
                    result += *coerceToString(
                        pos,
                        *v2,
                        context,
                        "while evaluating one element of the list",
                        coerceMore,
                        copyToStore,
                        canonicalizePath);
                } catch (Error & e) {
                    e.addTrace(positions[pos], errorCtx);
                    throw;
                }
                if (n < v.listSize() - 1
                    /* !!! not quite correct */
                    && (!v2->isList() || v2->listSize() != 0))
                    result += " ";
            }
            return result;
        }
    }

    error<TypeError>("cannot coerce %1% to a string: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
        .withTrace(pos, errorCtx)
        .debugThrow();
}

StorePath EvalState::copyPathToStore(NixStringContext & context, const RootedPath & path)
{
    auto sp = path.sourcePath();
    if (nix::isDerivation(sp.path.abs()))
        error<EvalError>("file names are not allowed to end in '%1%'", drvExtension).debugThrow();

    /* Internal-kinded SourceRoots can't be copied to the store. Catch
       the attempt here so string interpolation (`"${...}"`) on an
       internal path errors with the same shape `mkPos` uses for the
       null case. Without this check, `${corepkgsPath}` would silently
       produce a store path of nix-internal helpers. */
    if (path.root->kind == SourceRootKind::Internal)
        error<EvalError>(
            "cannot copy a path on an internal accessor to the store at '%1%'", sp.accessor->showPath(sp.path))
            .debugThrow();

    /* The lint fires on the *request* to copy a fetched source to
       store. Placing it before the cache lookup means a warm
       `srcToStore` cache doesn't silently hide the offending
       callsite. Two narrow predicates gate it:

       - Only Copyable-kinded paths are in scope: System paths
         represent filesystem locations the user explicitly
         requested, not fetched sources.

       - Only requests at the accessor root (`sp.path.isRoot()`)
         are flagged, matching the lint's name and its warning
         text. Per-file copies via `"${./script.sh}"`-style
         interpolation reach this code path with a non-root
         `sp.path`; they materialise just that one file (not the
         "entire contents" of the source), and they're intrinsic
         to the lazy-paths regime — flagging them would generate
         hundreds of warnings per `nix eval .#pkg.drvPath` (each
         derivation referencing each shell hook) and drown the
         actual whole-tree offenders that the lint is supposed to
         surface. */
    if (path.root->kind == SourceRootKind::Copyable && sp.path.isRoot())
        diagnose(fetchSettings.lintFetchWholeSourceToStore, [&](bool fatal) -> std::optional<Error> {
            return Error("reading the entire contents of fetched source '%s' into the store", sp);
        });

    auto dstPathCached = getConcurrent(*srcToStore, sp);

    auto dstPath = dstPathCached ? dstPathCached->first : [&]() {
        /* Kind-aware ancestor walk: Copyable gets
           `StrictAccessorBoundary` — a path whose ancestor
           symlinks escape the accessor root raises
           `AccessorBoundaryEscape` rather than silently splicing
           the spliced target into the copy. Internal is
           unreachable here (rejected above). System falls back to
           the lenient walker.

           Use fetchToStore2 (not fetchToStore) so the same walk
           hands back the NAR hash too — recording it in
           `srcToStore` lets a later `lockInput` narHash force
           skip the re-walk. */
        CanonPath resolved = nix::resolveSymlinks(*path.root, path.path, SymlinkResolution::Ancestors);
        auto [dstPath, narHash] = fetchToStore2(
            fetchSettings,
            *store,
            SourcePath{sp.accessor, resolved},
            settings.isReadOnly() ? FetchMode::DryRun : FetchMode::Copy,
            sp.baseName(),
            ContentAddressMethod::Raw::NixArchive,
            nullptr,
            repair);
        allowPath(dstPath);
        srcToStore->try_emplace(sp, std::make_pair(dstPath, narHash));
        printMsg(lvlChatty, "copied source '%1%' -> '%2%'", sp, store->printStorePath(dstPath));
        return dstPath;
    }();

    context.insert(NixStringContextElem::Opaque{.path = dstPath});
    return dstPath;
}

ref<SourceAccessor> EvalState::storePathAccessor(const StorePath & sp)
{
    auto spStr = store->printStorePath(sp);
    /* Mount table first: anything registered via `mountInput`
       (lazy fetchTree, builtins.fetchTree, etc.) preserves the
       fingerprint the mounter set, which is the most accurate
       identity for that store path. */
    if (auto m = storeFS->getMount(CanonPath(spStr)))
        return ref<SourceAccessor>(m);
    /* Otherwise ask the `Store` itself — works uniformly across
       local, relocated, and remote stores (the virtual method
       does the right thing for each). Throws `InvalidPath` if
       the store path doesn't exist; that's the genuinely
       undecidable case and the right answer per the never-
       return-wrong-answers contract. */
    auto acc = store->requireStoreObjectAccessor(sp);
    /* Set the fingerprint to the store path string if the
       accessor doesn't already carry one. Same content-addressed
       identity that satisfies the fingerprint contract (equal
       store paths ⇒ equal NAR by construction). Without this,
       downstream `fetchToStore` sees an unfingerprinted source
       and trips `_NIX_TEST_BARF_ON_UNCACHEABLE`. */
    if (!acc->fingerprint)
        acc->fingerprint = spStr;
    return acc;
}

SourcePath EvalState::coerceToPath(const PosIdx pos, Value & v, NixStringContext & context, std::string_view errorCtx)
{
    try {
        forceValue(v, pos);
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }

    /* Handle path values directly, without coercing to a string. */
    if (v.type() == nPath)
        return v.path();

    /* Similarly, handle __toString where the result may be a path
       value. */
    if (v.type() == nAttrs) {
        auto i = v.attrs()->get(s.toString);
        if (i) {
            Value v1;
            callFunction(*i->value, v, v1, pos);
            return coerceToPath(pos, v1, context, errorCtx);
        }
    }

    /* Any other value should be coercible to a string, interpreted
       relative to the root filesystem. */
    auto path = coerceToString(pos, v, context, errorCtx, false, false, true).toOwned();
    if (path == "" || path[0] != '/')
        error<EvalError>("string '%1%' doesn't represent an absolute path", path).withTrace(pos, errorCtx).debugThrow();
    return rootPath(CanonPath(path));
}

RootedPath
EvalState::coerceToRootedPath(const PosIdx pos, Value & v, NixStringContext & context, std::string_view errorCtx)
{
    try {
        forceValue(v, pos);
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }

    /* nPath: the kind is sitting right on the Value. Return it
       verbatim. No IO, no walk. */
    if (v.type() == nPath)
        return v.rootedPath();

    if (v.type() == nAttrs) {
        /* `__toString` takes precedence over `outPath` (per the
           language manual's string-interpolation rules). Call it
           and recurse on the result; the recursion preserves the
           inner kind if `__toString` returns an nPath, and falls
           through to the string arm if it returns the documented
           string. */
        if (auto i = v.attrs()->get(s.toString)) {
            Value v1;
            callFunction(*i->value, v, v1, pos);
            return coerceToRootedPath(pos, v1, context, errorCtx);
        }
        /* `outPath`: peel inductively when it's path- or
           attrset-shaped. The documented contract is "outPath must
           be a string"; under lazy paths the runtime actually
           carries an nPath here (`fetchTree { lazy = true; }`),
           and the inner path's `SourceRoot` is the kind we want
           the caller to see. When `outPath` *is* a string (the
           contract case, the eager-fetchTree case, derivations'
           predicted-store-path strings) we fall through to the
           string arm below, which admits under `rootFSRoot`
           (System). */
        if (auto i = v.attrs()->get(s.outPath)) {
            forceValue(*i->value, pos);
            if (i->value->type() == nPath || i->value->type() == nAttrs)
                return coerceToRootedPath(pos, *i->value, context, errorCtx);
        }
    }

    /* Fallback: route through `coerceToString` (no copy-to-store)
       and admit the resulting string under `rootFSRoot` (System).
       Any string-shaped path the language can produce here
       identifies a filesystem location, so System is the correct
       admission kind. */
    auto path = coerceToString(pos, v, context, errorCtx, false, false, true).toOwned();
    if (path == "" || path[0] != '/')
        error<EvalError>("string '%1%' doesn't represent an absolute path", path).withTrace(pos, errorCtx).debugThrow();
    return rootedPath(CanonPath(path));
}

StorePath
EvalState::coerceToStorePath(const PosIdx pos, Value & v, NixStringContext & context, std::string_view errorCtx)
{
    auto path = coerceToString(pos, v, context, errorCtx, false, false, true).toOwned();
    if (auto storePath = store->maybeParseStorePath(path))
        return *storePath;
    error<EvalError>("path '%1%' is not in the Nix store", path).withTrace(pos, errorCtx).debugThrow();
}

std::pair<SingleDerivedPath, std::string_view> EvalState::coerceToSingleDerivedPathUnchecked(
    const PosIdx pos, Value & v, std::string_view errorCtx, const ExperimentalFeatureSettings & xpSettings)
{
    NixStringContext context;
    auto s = forceString(v, context, pos, errorCtx, xpSettings);
    auto csize = context.size();
    if (csize != 1)
        error<EvalError>("string '%s' has %d entries in its context. It should only have exactly one entry", s, csize)
            .withTrace(pos, errorCtx)
            .debugThrow();
    auto derivedPath = std::visit(
        overloaded{
            [&](NixStringContextElem::Opaque && o) -> SingleDerivedPath { return std::move(o); },
            [&](NixStringContextElem::DrvDeep &&) -> SingleDerivedPath {
                error<EvalError>(
                    "string '%s' has a context which refers to a complete source and binary closure. This is not supported at this time",
                    s)
                    .withTrace(pos, errorCtx)
                    .debugThrow();
            },
            [&](NixStringContextElem::Built && b) -> SingleDerivedPath { return std::move(b); },
        },
        ((NixStringContextElem &&) *context.begin()).raw);
    return {
        std::move(derivedPath),
        std::move(s),
    };
}

SingleDerivedPath EvalState::coerceToSingleDerivedPath(const PosIdx pos, Value & v, std::string_view errorCtx)
{
    auto [derivedPath, s_] = coerceToSingleDerivedPathUnchecked(pos, v, errorCtx);
    auto s = s_;
    auto sExpected = mkSingleDerivedPathStringRaw(derivedPath);
    if (s != sExpected) {
        /* `std::visit` is used here just to provide a more precise
           error message. */
        std::visit(
            overloaded{
                [&](const SingleDerivedPath::Opaque & o) {
                    error<EvalError>("path string '%s' has context with the different path '%s'", s, sExpected)
                        .withTrace(pos, errorCtx)
                        .debugThrow();
                },
                [&](const SingleDerivedPath::Built & b) {
                    error<EvalError>(
                        "string '%s' has context with the output '%s' from derivation '%s', but the string is not the right placeholder for this derivation output. It should be '%s'",
                        s,
                        b.output,
                        b.drvPath->to_string(*store),
                        sExpected)
                        .withTrace(pos, errorCtx)
                        .debugThrow();
                }},
            derivedPath.raw());
    }
    return derivedPath;
}

// NOTE: This implementation must match eqValues!
// We accept this burden because informative error messages for
// `assert a == b; x` are critical for our users' testing UX.
void EvalState::assertEqValues(Value & v1, Value & v2, const PosIdx pos, std::string_view errorCtx)
{
    auto _level = addCallDepth(pos);

    // This implementation must match eqValues.
    forceValue(v1, pos);
    forceValue(v2, pos);

    if (&v1 == &v2)
        return;

    // Special case type-compatibility between float and int
    if ((v1.type() == nInt || v1.type() == nFloat) && (v2.type() == nInt || v2.type() == nFloat)) {
        if (eqValues(v1, v2, pos, errorCtx)) {
            return;
        } else {
            error<AssertionError>(
                "%s with value '%s' is not equal to %s with value '%s'",
                showType(v1),
                ValuePrinter(*this, v1, errorPrintOptions),
                showType(v2),
                ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
        }
    }

    if (v1.type() != v2.type()) {
        error<AssertionError>(
            "%s of value '%s' is not equal to %s of value '%s'",
            showType(v1),
            ValuePrinter(*this, v1, errorPrintOptions),
            showType(v2),
            ValuePrinter(*this, v2, errorPrintOptions))
            .debugThrow();
    }

    switch (v1.type()) {
    case nInt:
        if (v1.integer() != v2.integer()) {
            error<AssertionError>("integer '%d' is not equal to integer '%d'", v1.integer(), v2.integer()).debugThrow();
        }
        return;

    case nBool:
        if (v1.boolean() != v2.boolean()) {
            error<AssertionError>(
                "boolean '%s' is not equal to boolean '%s'",
                ValuePrinter(*this, v1, errorPrintOptions),
                ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
        }
        return;

    case nString:
        if (v1.string_view() != v2.string_view()) {
            error<AssertionError>(
                "string '%s' is not equal to string '%s'",
                ValuePrinter(*this, v1, errorPrintOptions),
                ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
        }
        return;

    case nPath: {
        /* assertEqValues is only called after eqValues returned
           false; eqValues' nPath case delegates to
           toStringEqual, so we already know the two paths
           are not toString-equivalent. Surface a coarse reason
           — subpath mismatch vs root mismatch — that covers the
           common cases without trying to reproduce the full
           dispatch. A structural diff is out of scope. */
        auto rp1 = v1.rootedPath();
        auto rp2 = v2.rootedPath();
        const char * reason = rp1.path != rp2.path ? "different subpaths" : "different roots";
        error<AssertionError>(
            "path '%s' is not equal to path '%s' (%s)",
            ValuePrinter(*this, v1, errorPrintOptions),
            ValuePrinter(*this, v2, errorPrintOptions),
            reason)
            .debugThrow();
    }

    case nNull:
        return;

    case nList:
        if (v1.listSize() != v2.listSize()) {
            error<AssertionError>(
                "list of size '%d' is not equal to list of size '%d', left hand side is '%s', right hand side is '%s'",
                v1.listSize(),
                v2.listSize(),
                ValuePrinter(*this, v1, errorPrintOptions),
                ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
        }
        for (size_t n = 0; n < v1.listSize(); ++n) {
            try {
                assertEqValues(*v1.listView()[n], *v2.listView()[n], pos, errorCtx);
            } catch (Error & e) {
                e.addTrace(positions[pos], "while comparing list element %d", n);
                throw;
            }
        }
        return;

    case nAttrs: {
        if (isDerivation(v1) && isDerivation(v2)) {
            auto i = v1.attrs()->get(s.outPath);
            auto j = v2.attrs()->get(s.outPath);
            if (i && j) {
                try {
                    assertEqValues(*i->value, *j->value, pos, errorCtx);
                    return;
                } catch (Error & e) {
                    e.addTrace(positions[pos], "while comparing a derivation by its '%s' attribute", "outPath");
                    throw;
                }
                assert(false);
            }
        }

        if (v1.attrs()->size() != v2.attrs()->size()) {
            error<AssertionError>(
                "attribute names of attribute set '%s' differs from attribute set '%s'",
                ValuePrinter(*this, v1, errorPrintOptions),
                ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
        }

        // Like normal comparison, we compare the attributes in non-deterministic Symbol index order.
        // This function is called when eqValues has found a difference, so to reliably
        // report about its result, we should follow in its literal footsteps and not
        // try anything fancy that could lead to an error.
        Bindings::const_iterator i, j;
        for (i = v1.attrs()->begin(), j = v2.attrs()->begin(); i != v1.attrs()->end(); ++i, ++j) {
            if (i->name != j->name) {
                // A difference in a sorted list means that one attribute is not contained in the other, but we don't
                // know which. Let's find out. Could use <, but this is more clear.
                if (!v2.attrs()->get(i->name)) {
                    error<AssertionError>(
                        "attribute name '%s' is contained in '%s', but not in '%s'",
                        symbols[i->name],
                        ValuePrinter(*this, v1, errorPrintOptions),
                        ValuePrinter(*this, v2, errorPrintOptions))
                        .debugThrow();
                }
                if (!v1.attrs()->get(j->name)) {
                    error<AssertionError>(
                        "attribute name '%s' is missing in '%s', but is contained in '%s'",
                        symbols[j->name],
                        ValuePrinter(*this, v1, errorPrintOptions),
                        ValuePrinter(*this, v2, errorPrintOptions))
                        .debugThrow();
                }
                assert(false);
            }
            try {
                assertEqValues(*i->value, *j->value, pos, errorCtx);
            } catch (Error & e) {
                // The order of traces is reversed, so this presents as
                //  where left hand side is
                //    at <pos>
                //  where right hand side is
                //    at <pos>
                //  while comparing attribute '<name>'
                if (j->pos != noPos)
                    e.addTrace(positions[j->pos], "where right hand side is");
                if (i->pos != noPos)
                    e.addTrace(positions[i->pos], "where left hand side is");
                e.addTrace(positions[pos], "while comparing attribute '%s'", symbols[i->name]);
                throw;
            }
        }
        return;
    }

    case nFunction:
        error<AssertionError>("distinct functions and immediate comparisons of identical functions compare as unequal")
            .debugThrow();

    case nExternal:
        if (!(*v1.external() == *v2.external())) {
            error<AssertionError>(
                "external value '%s' is not equal to external value '%s'",
                ValuePrinter(*this, v1, errorPrintOptions),
                ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
        }
        return;

    case nFloat:
        // !!!
        if (!(v1.fpoint() == v2.fpoint())) {
            error<AssertionError>("float '%f' is not equal to float '%f'", v1.fpoint(), v2.fpoint()).debugThrow();
        }
        return;

    // Cannot be returned by forceValue().
    case nThunk:
    case nFailed:
        unreachable();

    default: // Note that we pass compiler flags that should make `default:` unreachable.
        // Also note that this probably ran after `eqValues`, which implements
        // the same logic more efficiently (without having to unwind stacks),
        // so maybe `assertEqValues` and `eqValues` are out of sync. Check it for solutions.
        error<EvalError>("assertEqValues: cannot compare %1% with %2%", showType(v1), showType(v2))
            .withTrace(pos, errorCtx)
            .panic();
    }
}

// This implementation must match assertEqValues
bool EvalState::eqValues(
    Value & v1, Value & v2, const PosIdx pos, std::string_view errorCtx, PathEquivalenceContext * ctx)
{
    auto _level = addCallDepth(pos);

    forceValue(v1, pos);
    forceValue(v2, pos);

    /* !!! Hack to support some old broken code that relies on pointer
       equality tests between sets.  (Specifically, builderDefs calls
       uniqList on a list of sets.)  Will remove this eventually. */
    if (&v1 == &v2)
        return true;

    // Special case type-compatibility between float and int
    if (v1.type() == nInt && v2.type() == nFloat)
        return v1.integer().value == v2.fpoint();
    if (v1.type() == nFloat && v2.type() == nInt)
        return v1.fpoint() == v2.integer().value;

    /* Under a PathEquivalenceContext, cross-type path × string is
       taken as the toString-equivalent comparison (no throw, a real
       predicate). Enables `genericClosure { pathEquivalent = true; }`'s
       deep dedup to see through lists/attrsets whose elements mix
       paths and their toString-forms. Everywhere else cross-type
       still returns false (the language semantic). */
    if (v1.type() != v2.type()) {
        if (ctx && ((v1.type() == nPath && v2.type() == nString) || (v1.type() == nString && v2.type() == nPath))) {
            Value & pathV = v1.type() == nPath ? v1 : v2;
            Value & strV = v1.type() == nString ? v1 : v2;
            return pathToStringEqual(pathV.path(), pathV.pathRoot()->kind, strV.string_view());
        }
        return false;
    }

    switch (v1.type()) {
    case nInt:
        return v1.integer() == v2.integer();

    case nBool:
        return v1.boolean() == v2.boolean();

    case nString:
        return v1.string_view() == v2.string_view();

    case nPath:
        /* Both operands are paths (the cross-type case was
           handled above). Defer to the unified toString-
           equivalence engine, which decides via:
             - cheap pointer + subpath identity, or
             - eager string reduction for System, or
             - lazy contents compare for Copyable.
           Internal paths throw if the cheap shortcut doesn't
           fire — matching that `toString` is undefined on them. */
        return toStringEqual(v1, v2, pos, errorCtx);

    case nNull:
        return true;

    case nList:
        if (v1.listSize() != v2.listSize())
            return false;
        for (size_t n = 0; n < v1.listSize(); ++n)
            if (!eqValues(*v1.listView()[n], *v2.listView()[n], pos, errorCtx, ctx))
                return false;
        return true;

    case nAttrs: {
        /* If both sets denote a derivation (type = "derivation"),
           then compare their outPaths. */
        if (isDerivation(v1) && isDerivation(v2)) {
            auto i = v1.attrs()->get(s.outPath);
            auto j = v2.attrs()->get(s.outPath);
            if (i && j)
                return eqValues(*i->value, *j->value, pos, errorCtx, ctx);
        }

        if (v1.attrs()->size() != v2.attrs()->size())
            return false;

        /* Otherwise, compare the attributes one by one. */
        Bindings::const_iterator i, j;
        for (i = v1.attrs()->begin(), j = v2.attrs()->begin(); i != v1.attrs()->end(); ++i, ++j)
            if (i->name != j->name || !eqValues(*i->value, *j->value, pos, errorCtx, ctx))
                return false;

        return true;
    }

    /* Functions are incomparable. */
    case nFunction:
        return false;

    case nExternal:
        return *v1.external() == *v2.external();

    case nFloat:
        // !!!
        return v1.fpoint() == v2.fpoint();

    // Cannot be returned by forceValue().
    case nThunk:
    case nFailed:
        unreachable();

    default: // Note that we pass compiler flags that should make `default:` unreachable.
        error<EvalError>("eqValues: cannot compare %1% with %2%", showType(v1), showType(v2))
            .withTrace(pos, errorCtx)
            .panic();
    }
}

/* SHA256 of the accessor's root directory entry names, joined in
   lex order with NUL separators. Lazy and cached: the read happens
   once per accessor per evaluation. */
static Hash computeRootProbe(SourceAccessor & acc)
{
    auto entries = acc.readDirectory(CanonPath::root);
    /* `DirEntries` is `std::map<string, ...>` which already iterates
       in lex order on the key, so we don't need to sort. */
    std::string canonical;
    for (auto & [name, _] : entries) {
        canonical.append(name);
        canonical.push_back('\0');
    }
    return hashString(HashAlgorithm::SHA256, canonical);
}

bool EvalState::accessorsEquivalent(ref<SourceAccessor> a, ref<SourceAccessor> b, std::optional<CanonPath> hint)
{
    /* Pointer identity. The cheapest decisive layer. */
    if (&*a == &*b)
        return true;

    /* Canonical-order key for the symmetric inequality cache: low
       raw pointer first. The cache stores only known-inequivalent
       pairs (proven false). Known-equivalent pairs aren't cached —
       the cheap positive proofs (pointer, fingerprint) are
       essentially free to re-check, and `srcToStore`'s hash table
       already memoises the storePath compute outcome. */
    auto * rawA = &*a;
    auto * rawB = &*b;
    auto cacheKey = rawA < rawB ? std::make_pair(rawA, rawB) : std::make_pair(rawB, rawA);
    if (accessorsKnownInequivalent->contains(cacheKey))
        return false;

    /* Fingerprint match is a one-way positive proof: equal fingerprints
       AND equal rebased paths guarantee NAR-equal trees (that's the
       fingerprint contract, see `SourceAccessor::getFingerprint`).
       Mismatch is no-info — two different fingerprints can back the
       same NAR (different URLs, different cache rebases). Fall through. */
    {
        auto [rebasedA, fpA] = a->getFingerprint(CanonPath::root);
        auto [rebasedB, fpB] = b->getFingerprint(CanonPath::root);
        if (fpA && fpB && *fpA == *fpB && rebasedA == rebasedB)
            return true;
    }

    /* Query-only `srcToStore` probe: if BOTH sides already have a
       computed storePath, decide immediately without paying any
       further probe or walk. The cheap path of `copyPathToStore`
       would also hit this, but checking it explicitly up front
       lets us short-circuit the more expensive root/hint probes
       in the both-cached case. Either side uncached → no info,
       fall through. */
    auto spOf = [&](ref<SourceAccessor> acc) -> std::optional<StorePath> {
        if (auto cached = getConcurrent(*srcToStore, SourcePath{acc, CanonPath::root}))
            return cached->first;
        return std::nullopt;
    };
    {
        auto spA = spOf(a);
        auto spB = spOf(b);
        if (spA && spB) {
            if (*spA == *spB)
                return true;
            accessorsKnownInequivalent->insert(cacheKey);
            return false;
        }
    }

    /* Root probe: SHA256 of each accessor's root-directory entry
       names. Computed lazily per accessor and memoised. Mismatch
       decisively disproves equivalence (the root entry sets
       already disagree). Match is no info — the children could
       still differ in content. */
    auto rootProbeOf = [&](ref<SourceAccessor> acc) -> Hash {
        if (auto cached = getConcurrent(*accessorRootProbeCache, &*acc))
            return *cached;
        auto h = computeRootProbe(*acc);
        accessorRootProbeCache->try_emplace(&*acc, h);
        return h;
    };
    if (rootProbeOf(a) != rootProbeOf(b)) {
        accessorsKnownInequivalent->insert(cacheKey);
        return false;
    }

    /* Hint probe: SHA256 of the file at `hint` on each side, when
       supplied. The caller is presumed to be about to read this
       subpath anyway (e.g. comparing two roots in a context that
       will then `import <a>/x.nix` and `<b>/x.nix`); hashing it
       under the hint contract turns that read into a cheap
       inequality disproof. Mismatched hashes prove the roots are
       inequivalent. Match is no info (one matching file says
       nothing about the rest of the tree). Either side absent or
       non-regular → no info. */
    if (hint) {
        auto hintProbeOf = [&](ref<SourceAccessor> acc) -> std::optional<Hash> {
            auto key = std::make_pair(&*acc, *hint);
            if (auto cached = getConcurrent(*accessorHintProbeCache, key))
                return *cached;
            std::optional<Hash> result;
            auto stat = acc->maybeLstat(*hint);
            if (stat && stat->type == SourceAccessor::Type::tRegular)
                result = hashString(HashAlgorithm::SHA256, acc->readFile(*hint));
            accessorHintProbeCache->try_emplace(std::move(key), result);
            return result;
        };
        auto hA = hintProbeOf(a);
        auto hB = hintProbeOf(b);
        if (hA && hB && *hA != *hB) {
            accessorsKnownInequivalent->insert(cacheKey);
            return false;
        }
    }

    /* The only step that may walk a tree. `copyPathToStore` reads the
       cached entry from `srcToStore` when present; otherwise it walks
       the tree once, computes the storePath, and writes it back. So
       any Copyable accessor pays at most one walk across the whole
       evaluation, regardless of how many comparisons reference it. */
    NixStringContext ctxA, ctxB;
    auto rootA = SourceRoot::make(a, SourceRootKind::Copyable);
    auto rootB = SourceRoot::make(b, SourceRootKind::Copyable);
    auto spA = copyPathToStore(ctxA, RootedPath{rootA, CanonPath::root});
    auto spB = copyPathToStore(ctxB, RootedPath{rootB, CanonPath::root});
    if (spA == spB)
        return true;
    accessorsKnownInequivalent->insert(cacheKey);
    return false;
}

bool EvalState::pathToStringEqual(const SourcePath & p, SourceRootKind kind, std::string_view s)
{
    switch (kind) {
    case SourceRootKind::Internal:
        /* Mirrors coerceToString's Internal arm: an Internal
           path has no defined toString, so equivalence against
           any string is undefined too. Throwing surfaces a
           usage bug; silently returning false would mask it. */
        error<EvalError>(
            "cannot compare a path on an internal accessor at '%1%' to the string '%2%' for equivalence",
            p.accessor->showPath(p.path),
            s)
            .debugThrow();
    case SourceRootKind::System:
        /* `toString` on a System path is just its abspath. */
        return p.path.abs() == s;
    case SourceRootKind::Copyable: {
        /* `toString` on a Copyable path is
           `<storeDir>/<storeBase> + p.path.absOrEmpty()`. Cheap
           rejects on shape and subpath mismatch; otherwise the
           question is "does `p.accessor` materialise to this
           specific storePath?" — a one-sided comparison that
           doesn't fit `accessorsEquivalent`'s two-accessor shape
           (constructing an on-disk accessor side here would just
           re-hash the store path uncacheably, no fingerprint).

           Cheap path: query `srcToStore` for `p.accessor`'s
           root; if cached, compare its storePath to the parsed
           one and we're done.

           Otherwise: compute `p.accessor`'s storePath via
           `copyPathToStore` and compare. The lint trips on the
           compute by design — the same materialisation event
           that `toString` would have caused, just driven by a
           comparison instead of a string coercion. */
        if (!store->isInStore(s))
            return false;
        StorePath storePath{StorePath::dummy};
        CanonPath subpath{""};
        try {
            std::tie(storePath, subpath) = store->toStorePath(s);
        } catch (BadStorePath &) {
            /* Right shape (starts with storeDir) but the
               storeBase isn't a valid store name. Not
               equivalent. */
            return false;
        }
        if (subpath != p.path)
            return false;
        if (auto cached = getConcurrent(*srcToStore, SourcePath{p.accessor, CanonPath::root}))
            return cached->first == storePath;
        /* No catch: any error from `copyPathToStore` (the lint
           trip with fatal, a real fetch error) must propagate.
           Swallowing them would amount to claiming "not
           equivalent" without proof, which violates the
           never-return-wrong-answers contract. */
        NixStringContext ctx;
        auto root = SourceRoot::make(p.accessor, SourceRootKind::Copyable);
        return copyPathToStore(ctx, RootedPath{root, CanonPath::root}) == storePath;
    }
    }
    unreachable();
}

namespace {

/* Why these helpers are hand-rolled rather than reusing
   `Store::toStorePath` + `Store::printStorePath`: the round-trip
   approach would parse via `CanonPath` (which silently
   canonicalises trailing `/`, `/.`, `/..`), then re-render via
   `printStorePath` (allocates a `std::string`), then byte-
   compare against the input to detect non-canonicality. Two
   allocations and a re-render on every Camp A admission probe,
   just to reject non-canonical inputs. The strict per-byte
   rejection below short-circuits on the first non-canonical
   byte — no allocations, no canonicalisation work to throw
   away.

   `Store::isInStore` is also off-limits: it's lenient
   (delegates to `isInDir`'s `lexically_relative`), so a path
   like `/nix/store/./abc` would be accepted as "in store" —
   the exact non-canonical inputs we need to reject. It also
   only returns `bool`, not a cursor into the rest of the
   bytes. The literal `s.starts_with(storeDir + "/")` below is
   strict *and* gives us the cursor for the rest of the parse. */

bool isNix32Hash32(std::string_view s)
{
    if (s.size() != 32)
        return false;
    for (char c : s)
        if (!BaseNix32::lookupReverse(c))
            return false;
    return true;
}

/* Canonical here means "could appear verbatim in `toString`
   output of a Copyable path"; rejects trailing `/`, `//`, `/.`,
   and `/..` components. Empty is canonical (= the root of a
   Copyable, whose toString emits `storeDir/storeBase` with no
   trailing slash). */
bool isCanonicalSubpath(std::string_view s)
{
    if (s.empty())
        return true;
    if (s[0] != '/' || s.size() == 1 || s.back() == '/')
        return false;
    if (s.find("//") != std::string_view::npos)
        return false;
    for (size_t i = 0; (i = s.find('/', i)) != std::string_view::npos;) {
        ++i;
        size_t end = s.find('/', i);
        if (end == std::string_view::npos)
            end = s.size();
        auto comp = s.substr(i, end - i);
        if (comp == "." || comp == "..")
            return false;
        i = end;
    }
    return true;
}

/* If `s` is byte-for-byte the canonical `toString` of some
   Copyable — `storeDir + "/" + 32-nix32-hash + "-source"`
   optionally followed by a canonical subpath — return that
   subpath (`""` for the root). Otherwise nullopt. The name
   slot is restricted to "source" because that's the only name
   a Copyable ever materialises under, so it's the only one our
   equivalence relation admits. */
std::optional<std::string_view> parseCanonicalCopyableToString(std::string_view s, std::string_view storeDir)
{
    std::string prefix = std::string{storeDir};
    prefix += '/';
    if (!s.starts_with(prefix))
        return std::nullopt;
    auto rest = s.substr(prefix.size());
    constexpr size_t hashLen = 32;
    constexpr std::string_view nameTag = "-source";
    if (rest.size() < hashLen + nameTag.size())
        return std::nullopt;
    if (!isNix32Hash32(rest.substr(0, hashLen)))
        return std::nullopt;
    if (rest.substr(hashLen, nameTag.size()) != nameTag)
        return std::nullopt;
    auto subpath = rest.substr(hashLen + nameTag.size());
    if (!isCanonicalSubpath(subpath))
        return std::nullopt;
    return subpath;
}

} // namespace

std::strong_ordering EvalState::compareForToStringEquivalence(
    Value & a, Value & b, const PosIdx pos, std::string_view errorCtx, PathEquivalenceContext * ctx)
{
    forceValue(a, pos);
    forceValue(b, pos);

    auto cmpStrings = [](std::string_view x, std::string_view y) {
        auto c = x.compare(y);
        if (c < 0)
            return std::strong_ordering::less;
        if (c > 0)
            return std::strong_ordering::greater;
        return std::strong_ordering::equal;
    };

    /* Classify into Camp A (canonical Copyable-toString shape)
       or Camp B (everything else). The sub-bytes returned drive
       step 2:
         Camp A → the parsed subpath after `-source` (`""` for
         the root).
         Camp B → the full bytes (System abspath / string). */
    struct Classified
    {
        bool isA;
        std::string_view sub;
    };

    auto classify = [&](Value & v) -> Classified {
        if (v.type() == nString) {
            auto s = v.string_view();
            if (auto sub = parseCanonicalCopyableToString(s, store->storeDir))
                return {true, *sub};
            return {false, s};
        }
        if (v.type() == nPath) {
            auto * root = v.pathRoot();
            switch (root->kind) {
            case SourceRootKind::Internal:
                error<EvalError>(
                    "cannot coerce a path on an internal accessor to a string at '%1%'",
                    root->accessor->showPath(CanonPath(CanonPath::unchecked_t(), std::string(v.pathStrView()))))
                    .withTrace(pos, errorCtx)
                    .debugThrow();
            case SourceRootKind::Copyable: {
                /* Copyable is always Camp A. Its toString-subpath
                   is `absOrEmpty()` — empty for root, `/foo`
                   otherwise. `pathStrView()` returns the raw
                   CanonPath bytes, which are `"/"` for root; map
                   that to `""` to match toString output. */
                auto abs = v.pathStrView();
                if (abs == "/")
                    abs = std::string_view{};
                return {true, abs};
            }
            case SourceRootKind::System: {
                /* System's `toString` is its abspath; parse to
                   decide camp. If the abspath is itself a
                   canonical Copyable-toString shape, the System
                   path is in store and lands in Camp A — its
                   classification is the same as a string with
                   the same bytes. */
                auto abs = v.pathStrView();
                if (auto sub = parseCanonicalCopyableToString(abs, store->storeDir))
                    return {true, *sub};
                return {false, abs};
            }
            }
            unreachable();
        }
        error<EvalError>(
            "cannot compare %s with %s for toString equivalence; only paths and strings are supported",
            showType(a),
            showType(b))
            .withTrace(pos, errorCtx)
            .debugThrow();
    };

    auto ca = classify(a);
    auto cb = classify(b);

    /* Step 1: camp partition. Camp B < Camp A. */
    if (ca.isA != cb.isA)
        return ca.isA ? std::strong_ordering::greater : std::strong_ordering::less;

    /* Step 2: subpath compare (or, for Camp B, full byte compare). */
    auto subCmp = cmpStrings(ca.sub, cb.sub);
    if (subCmp != std::strong_ordering::equal)
        return subCmp;

    /* Step 3: source root equivalence class. Fires only inside
       Camp A and only when a `ctx` is supplied. Step 2's equal
       within Camp B already implies toString-equivalence (same
       bytes ⟺ same toString), so no need to refine there. */
    if (!ctx || !ca.isA)
        return std::strong_ordering::equal;

    /* Both Values are now in Camp A and share the same operative
       subpath (`ca.sub == cb.sub`). Feed that subpath through to
       `accessorsEquivalent` as the hint so its hint probe at the
       shared subpath can disprove inequivalence cheaply. Empty
       subpath (root-level compare) gets no hint — the hint probe
       only helps on regular files, not on the root itself. */
    std::optional<CanonPath> hint;
    if (!ca.sub.empty())
        hint = CanonPath(ca.sub);

    auto classIdForCampA = [&](Value & v) -> size_t {
        if (v.type() == nPath) {
            auto * root = v.pathRoot();
            if (root->kind == SourceRootKind::Copyable)
                return ctx->classOfAccessor(*root, hint);
            /* System in store falls through to the parse +
               bridge path below — same as a Camp A string. */
        }
        auto bytes = v.type() == nString ? v.string_view() : v.pathStrView();
        auto [storePath, _subpath] = store->toStorePath(bytes);
        /* `storePathAccessor` returns a fingerprinted accessor for
           the store path — `storeFS` mount when available,
           `Store::requireStoreObjectAccessor` otherwise (uniform
           across local, relocated, and remote stores). The
           fingerprint lets `accessorsEquivalent`'s downstream
           layers and `fetchToStore`'s cache treat the mounted
           side as a proper source. The bare `InvalidPath` that
           surfaces when the store path is unreachable doesn't
           name the operation — wrap it so the user sees what
           comparison was attempted. The accessor+kind overload
           of `classOfAccessor` skips the SourceRoot::make
           allocation that the SourceRoot& overload above would
           pay per call. */
        try {
            return ctx->classOfAccessor(storePathAccessor(storePath), SourceRootKind::Copyable, hint);
        } catch (InvalidPath & e) {
            e.addTrace(
                positions[pos],
                "while comparing store path '%1%' for equivalence against a fetched source",
                store->printStorePath(storePath));
            throw;
        }
    };

    auto idA = classIdForCampA(a);
    auto idB = classIdForCampA(b);
    if (idA == idB)
        return std::strong_ordering::equal;
    return idA < idB ? std::strong_ordering::less : std::strong_ordering::greater;
}

/* Mental note for future profiling: this primitive runs every
   path × path == comparison, which on a moderately large
   evaluation can be a lot. When two Copyable paths land here
   they fall through to `accessorsEquivalent`, which prefers
   cheap probes (pointer, fingerprint, srcToStore lookup,
   root-name SHA256, hint SHA256) and only computes the
   storePath as last resort — with the result memoised in
   `srcToStore` so subsequent comparisons of the same accessor
   are O(1) lookups. */
bool EvalState::toStringEqual(Value & v1, Value & v2, const PosIdx pos, std::string_view errorCtx)
{
    forceValue(v1, pos);
    forceValue(v2, pos);

    auto isAllowed = [](ValueType t) { return t == nPath || t == nString; };
    if (!isAllowed(v1.type()) || !isAllowed(v2.type())) {
        error<EvalError>(
            "toStringEqual: arguments must be paths or strings, got %1% and %2%", showType(v1), showType(v2))
            .withTrace(pos, errorCtx)
            .debugThrow();
    }

    /* Cheap identity shortcut: identical bytes (for strings),
       or — for paths — same accessor pointer + same kind + same
       subpath. The kind check matters: a System path and a
       Copyable path backed by the same accessor at the same
       subpath have different toStrings (System gives the
       abspath, Copyable gives the store-path + subpath). The
       shortcut also covers Internal × Internal cheaply, which
       otherwise would throw in the reduction.

       Uses `pathRoot()` + `pathStrView()` to stay allocation-
       free: `rootedPath()` constructs a `RootedPath` with a
       heap-allocated subpath string and an atomic refcount bump
       on the SourceRoot — both per call. In tight comparison
       loops (sort/uniq, dedup) this branch dominated the perf
       profile as ~5% self-time in `Value::rootedPath()`. */
    if (v1.type() == nString && v2.type() == nString)
        return v1.string_view() == v2.string_view();
    if (v1.type() == nPath && v2.type() == nPath) {
        auto * r1 = v1.pathRoot();
        auto * r2 = v2.pathRoot();
        bool sameRoot = r1 == r2 || (r1->kind == r2->kind && &*r1->accessor == &*r2->accessor);
        if (sameRoot && v1.pathStrView() == v2.pathStrView())
            return true;
    }

    /* Reduce each operand to its "eager string" — the literal
       toString result — if possible. nString uses its bytes; a
       System path uses its subpath's abspath; a Copyable path
       stays None (computing the storeHash is what we're
       avoiding); an Internal path throws.

       Same allocation discipline as the cheap shortcut above:
       use `pathRoot()` + `pathStrView()` for the common System
       arm, and only build a `RootedPath` on the Internal error
       path (which throws anyway, so the alloc cost is moot). */
    auto eagerOf = [&](Value & v) -> std::optional<std::string> {
        if (v.type() == nString)
            return std::string(v.string_view());
        auto * root = v.pathRoot();
        switch (root->kind) {
        case SourceRootKind::System:
            return std::string(v.pathStrView());
        case SourceRootKind::Copyable:
            return std::nullopt;
        case SourceRootKind::Internal: {
            auto rp = v.rootedPath();
            error<EvalError>(
                "cannot compute toString-equivalence on an internal-accessor path at '%1%'",
                rp.root->accessor->showPath(rp.path))
                .withTrace(pos, errorCtx)
                .debugThrow();
        }
        }
        unreachable();
    };

    auto e1 = eagerOf(v1);
    auto e2 = eagerOf(v2);

    /* Both reduced to a literal string: a straight bytes
       compare. Catches String × String, System × System, and
       String × System. */
    if (e1 && e2)
        return *e1 == *e2;

    /* One reduced, one lazy (the lazy side is necessarily a
       Copyable path). Defer to `pathToStringEqual`. */
    if (e1)
        return pathToStringEqual(v2.path(), v2.pathRoot()->kind, *e1);
    if (e2)
        return pathToStringEqual(v1.path(), v1.pathRoot()->kind, *e2);

    /* Both lazy: both Copyable paths. Subpaths must match (else
       their toStrings differ in the trailing component);
       `accessorsEquivalent` decides whether the storeHashes do,
       routing through cheap probes first (pointer, fingerprint,
       srcToStore lookup, root-name SHA256, hint SHA256 at the
       shared subpath) before paying any walk. The shared subpath
       is the natural hint: the caller is already comparing at
       that point in both trees. */
    auto p1 = v1.path();
    auto p2 = v2.path();
    if (p1.path != p2.path)
        return false;
    return accessorsEquivalent(p1.accessor, p2.accessor, p1.path);
}

namespace {

/* Do the two roots produce the same toString *prefix*?
   Cheap signals only — same SourceAccessor (by `number`) or
   matching fingerprint at root. Internal is not handled
   here; callers reject Internal upfront so this helper
   assumes non-Internal operands. */
bool rootsHaveSamePrefix(const SourceRoot & r1, const SourceRoot & r2)
{
    if (r1.kind != r2.kind)
        return false;
    if (*r1.accessor == *r2.accessor)
        return true;
    auto [p1, fp1] = r1.accessor->getFingerprint(CanonPath::root);
    auto [p2, fp2] = r2.accessor->getFingerprint(CanonPath::root);
    return fp1 && fp2 && *fp1 == *fp2 && p1 == p2;
}

/* Cross-kind System × Copyable cheap classification. The
   Copyable family of toStrings is exactly the strings starting
   with `storeDir + "/"`; we classify the System abspath
   against that family.
     - Less / Greater: System lex-outside the family. Definitive
       order against any Copyable, no Copyable info needed.
     - SamePrefix: System lies inside `storeDir + "/"`. Both
       toStrings share `storeDir + "/"` as a prefix — same shape
       as the "step 1 shared-prefix subpath compare", except
       the Copyable's post-prefix bytes (storeHash+name+subpath)
       require materialisation. Caller defers to the materialise
       arm to fill those in. */
enum class CrossKindCheap { Less, SamePrefix, Greater };

CrossKindCheap classifySystemVsCopyable(std::string_view systemAbs, std::string_view storeDir)
{
    std::string prefix = std::string(storeDir);
    prefix += '/';
    if (systemAbs.starts_with(prefix))
        return CrossKindCheap::SamePrefix;
    return systemAbs.compare(prefix) < 0 ? CrossKindCheap::Less : CrossKindCheap::Greater;
}

std::strong_ordering cmpStrings(std::string_view a, std::string_view b)
{
    auto c = a.compare(b);
    if (c < 0)
        return std::strong_ordering::less;
    if (c > 0)
        return std::strong_ordering::greater;
    return std::strong_ordering::equal;
}

} // namespace

std::strong_ordering EvalState::comparePathsForOrdering(
    Value & v1, Value & v2, const PosIdx pos, std::string_view errorCtx, PathEquivalenceContext * ctx)
{
    forceValue(v1, pos);
    forceValue(v2, pos);

    auto rp1 = v1.rootedPath();
    auto rp2 = v2.rootedPath();

    /* Reject Internal upfront. `toString` on an Internal path
       is undefined, so `<` is too — and rejecting here lets the
       rest of the function assume the cheap shortcuts can
       speak for both operands without special-casing. Mirrors
       `coerceToString`'s Internal arm. */
    for (auto * rp : {&rp1, &rp2}) {
        if (rp->root->kind == SourceRootKind::Internal)
            error<EvalError>(
                "cannot coerce a path on an internal accessor to a string at '%1%'",
                rp->root->accessor->showPath(rp->path))
                .withTrace(pos, errorCtx)
                .debugThrow();
    }

    /* 0. With ctx: classId equality is the cheap toString-
       equivalence witness. Same classId means the two roots'
       toString prefixes agree, so subpath drives the order
       (same as a structural same-prefix). Different classIds
       get an arbitrary but stable order via the classId number
       — total preorder usable as a sorted-container key,
       without firing materialise (and the lint with it). */
    if (ctx) {
        auto id1 = ctx->classOfAccessor(*rp1.root);
        auto id2 = ctx->classOfAccessor(*rp2.root);
        if (id1 == id2)
            return cmpStrings(rp1.path.abs(), rp2.path.abs());
        return id1 < id2 ? std::strong_ordering::less : std::strong_ordering::greater;
    }

    /* 1. Shared root prefix → subpath drives the lex order. Raw-
       string compare (not CanonPath's separator-first `<=>`) to
       match what the full toString comparison would yield.
       Covers same-accessor and matching-fingerprint cases of
       any non-Internal kind. */
    if (rootsHaveSamePrefix(*rp1.root, *rp2.root))
        return cmpStrings(rp1.path.abs(), rp2.path.abs());

    /* 2. Cross-kind System × Copyable. Ternary classification:
         - Less / Greater: System lex-outside the storeDir+/
           range → decisive without touching Copyable.
         - SamePrefix: System lies inside storeDir+/, so it
           shares the storeDir+/ prefix with the Copyable side.
           Same shape as step 1's shared-prefix subpath compare,
           but the Copyable's post-prefix bytes need
           materialisation; defers to step 3. */
    if (rp1.root->kind == SourceRootKind::System && rp2.root->kind == SourceRootKind::Copyable) {
        switch (classifySystemVsCopyable(rp1.path.abs(), store->storeDir)) {
        case CrossKindCheap::Less:
            return std::strong_ordering::less;
        case CrossKindCheap::Greater:
            return std::strong_ordering::greater;
        case CrossKindCheap::SamePrefix:
            break;
        }
    } else if (rp1.root->kind == SourceRootKind::Copyable && rp2.root->kind == SourceRootKind::System) {
        switch (classifySystemVsCopyable(rp2.path.abs(), store->storeDir)) {
        case CrossKindCheap::Less:
            return std::strong_ordering::greater;
        case CrossKindCheap::Greater:
            return std::strong_ordering::less;
        case CrossKindCheap::SamePrefix:
            break;
        }
    }

    /* 3. Materialise both sides and string-compare. Two reach
       paths:
         - Same-kind, different roots, no fingerprint match. The
           two toStrings are independent strings.
         - Cross-kind System × Copyable with SamePrefix from step 2.
           Both toStrings share storeDir+/ as a prefix; the compare
           here is step 1's subpath compare with the Copyable's
           post-prefix bytes filled in by `coerceToString`.
       Materialisation on Copyable fires `lint-fetch-whole-source-
       to-store` — correct: `<` semantically *is* a `toString`
       call. */
    NixStringContext ctxL, ctxR;
    auto sL = coerceToString(pos, v1, ctxL, errorCtx, /*coerceMore=*/false, /*copyToStore=*/false);
    auto sR = coerceToString(pos, v2, ctxR, errorCtx, /*coerceMore=*/false, /*copyToStore=*/false);
    return cmpStrings(*sL, *sR);
}

size_t PathEquivalenceContext::classOfAccessor(const SourceRoot & root, std::optional<CanonPath> hint)
{
    return classOfAccessor(root.accessor, root.kind, std::move(hint));
}

size_t PathEquivalenceContext::classOfAccessor(
    ref<SourceAccessor> accessor, SourceRootKind kind, std::optional<CanonPath> hint)
{
    auto * raw = &*accessor;
    if (auto it = classOf.find(raw); it != classOf.end())
        return it->second;

    switch (kind) {
    case SourceRootKind::System: {
        if (!systemClassId)
            systemClassId = nextClassId++;
        classOf[raw] = *systemClassId;
        return *systemClassId;
    }
    case SourceRootKind::Copyable: {
        /* Scan existing Copyable reps for an NAR-equivalent match
           via `accessorsEquivalent`, which prefers cheap probes
           (pointer, fingerprint, srcToStore lookup, root-name
           SHA256, hint SHA256) over walking and only pays a
           storePath compute as last resort — and that compute
           lands in `srcToStore` so any future comparison of this
           accessor is O(1). Worst case across N reps is O(N)
           probes per new accessor, but in practice the layered
           probes decide most pairs cheaply and the per-accessor
           caches amortise the rest. */
        for (size_t i = 0; i < copyableReps.size(); ++i) {
            if (state.accessorsEquivalent(accessor, copyableReps[i], hint)) {
                auto repId = classOf.at(&*copyableReps[i]);
                classOf[raw] = repId;
                return repId;
            }
        }
        /* No match — new class. */
        auto id = nextClassId++;
        copyableReps.push_back(accessor);
        classOf[raw] = id;
        return id;
    }
    case SourceRootKind::Internal: {
        /* Identity-class per pointer. `toString` is undefined
           for Internal, so cross-accessor equivalence is too —
           but same-pointer identity is well-defined and lets
           same-accessor Internal paths dedup. */
        auto id = nextClassId++;
        classOf[raw] = id;
        return id;
    }
    }
    unreachable();
}

bool EvalState::fullGC()
{
#if NIX_USE_BOEHMGC
    GC_gcollect();
    // Check that it ran. We might replace this with a version that uses more
    // of the boehm API to get this reliably, at a maintenance cost.
    // We use a 1K margin because technically this has a race condition, but we
    // probably won't encounter it in practice, because the CLI isn't concurrent
    // like that.
    return GC_get_bytes_since_gc() < 1024;
#else
    return false;
#endif
}

bool Counter::enabled = getEnv("NIX_SHOW_STATS").value_or("0") != "0";

void EvalState::maybePrintStats()
{
    if (Counter::enabled) {
        // Make the final heap size more deterministic.
#if NIX_USE_BOEHMGC
        if (!fullGC()) {
            warn("failed to perform a full GC before reporting stats");
        }
#endif
        printStatistics();
    }
}

void EvalState::printStatistics()
{
    std::chrono::microseconds cpuTimeDuration = getCpuUserTime();
    float cpuTime = std::chrono::duration_cast<std::chrono::duration<float>>(cpuTimeDuration).count();

    auto & memstats = mem.getStats();

    uint64_t bEnvs = memstats.nrEnvs * sizeof(Env) + memstats.nrValuesInEnvs * sizeof(Value *);
    uint64_t bLists = memstats.nrListElems * sizeof(Value *);
    uint64_t bValues = memstats.nrValues * sizeof(Value);
    uint64_t bAttrsets = memstats.nrAttrsets * sizeof(Bindings) + memstats.nrAttrsInAttrsets * sizeof(Attr);

#if NIX_USE_BOEHMGC
    GC_word heapSize, totalBytes;
    GC_get_heap_usage_safe(&heapSize, 0, 0, 0, &totalBytes);
    double gcFullOnlyTime = ({
        auto ms = GC_get_full_gc_total_time();
        ms * 0.001;
    });
    auto gcCycles = getGCCycles();
#endif

    auto outPath = getEnv("NIX_SHOW_STATS_PATH").value_or("-");
    std::fstream fs;
    if (outPath != "-")
        fs.open(outPath, std::fstream::out);
    json topObj = json::object();
    topObj["cpuTime"] = cpuTime;
    topObj["time"] = {
        {"cpu", cpuTime},
#if NIX_USE_BOEHMGC
        {GC_is_incremental_mode() ? "gcNonIncremental" : "gc", gcFullOnlyTime},
        {GC_is_incremental_mode() ? "gcNonIncrementalFraction" : "gcFraction", gcFullOnlyTime / cpuTime},
#endif
    };
    topObj["envs"] = {
        {"number", memstats.nrEnvs.load()},
        {"elements", memstats.nrValuesInEnvs.load()},
        {"bytes", bEnvs},
    };
    topObj["nrExprs"] = Expr::nrExprs.load();
    topObj["list"] = {
        {"elements", memstats.nrListElems.load()},
        {"bytes", bLists},
        {"concats", nrListConcats.load()},
    };
    topObj["values"] = {
        {"number", memstats.nrValues.load()},
        {"bytes", bValues},
    };
    topObj["symbols"] = {
        {"number", symbols.size()},
        {"bytes", symbols.totalSize()},
    };
    topObj["sets"] = {
        {"number", memstats.nrAttrsets.load()},
        {"bytes", bAttrsets},
        {"elements", memstats.nrAttrsInAttrsets.load()},
    };
    topObj["sizes"] = {
        {"Env", sizeof(Env)},
        {"Value", sizeof(Value)},
        {"Bindings", sizeof(Bindings)},
        {"Attr", sizeof(Attr)},
    };
    topObj["nrOpUpdates"] = nrOpUpdates.load();
    topObj["nrOpUpdateValuesCopied"] = nrOpUpdateValuesCopied.load();
    topObj["nrThunks"] = nrThunks.load();
    topObj["nrAvoided"] = nrAvoided.load();
    topObj["nrLookups"] = nrLookups.load();
    topObj["nrPrimOpCalls"] = nrPrimOpCalls.load();
    topObj["nrFunctionCalls"] = nrFunctionCalls.load();
#if NIX_USE_BOEHMGC
    topObj["gc"] = {
        {"heapSize", heapSize},
        {"totalBytes", totalBytes},
        {"cycles", gcCycles},
    };
#endif

    if (countCalls) {
        topObj["primops"] = primOpCalls;
        {
            auto & list = topObj["functions"];
            list = json::array();
            for (auto & [fun, count] : functionCalls) {
                json obj = json::object();
                if (fun->name)
                    obj["name"] = (std::string_view) symbols[fun->name];
                else
                    obj["name"] = nullptr;
                if (auto pos = positions[fun->pos]) {
                    if (auto path = std::get_if<RootedPath>(&pos.origin))
                        obj["file"] = path->sourcePath().to_string();
                    obj["line"] = pos.line;
                    obj["column"] = pos.column;
                }
                obj["count"] = count;
                list.push_back(obj);
            }
        }
        {
            auto list = topObj["attributes"];
            list = json::array();
            for (auto & i : attrSelects) {
                json obj = json::object();
                if (auto pos = positions[i.first]) {
                    if (auto path = std::get_if<RootedPath>(&pos.origin))
                        obj["file"] = path->sourcePath().to_string();
                    obj["line"] = pos.line;
                    obj["column"] = pos.column;
                }
                obj["count"] = i.second;
                list.push_back(obj);
            }
        }
    }

    if (getEnv("NIX_SHOW_SYMBOLS").value_or("0") != "0") {
        // XXX: overrides earlier assignment
        topObj["symbols"] = json::array();
        auto & list = topObj["symbols"];
        symbols.dump([&](std::string_view s) { list.emplace_back(s); });
    }
    if (outPath == "-") {
        std::cerr << topObj.dump(2) << std::endl;
    } else {
        fs << topObj.dump(2) << std::endl;
    }
}

SourcePath resolveExprPath(SourcePath path, bool addDefaultNix)
{
    unsigned int followCount = 0, maxFollow = 1024;

    /* If `path' is a symlink, follow it.  This is so that relative
       path references work. */
    while (!path.path.isRoot()) {
        // Basic cycle/depth limit to avoid infinite loops.
        if (++followCount >= maxFollow)
            throw Error("too many symbolic links encountered while traversing the path '%s'", path);
        auto p = path.parent().resolveSymlinks() / path.baseName();
        if (p.lstat().type != SourceAccessor::tSymlink)
            break;
        path = {path.accessor, CanonPath(p.readLink(), path.path.parent().value_or(CanonPath::root))};
    }

    /* If `path' refers to a directory, append `/default.nix'. */
    if (addDefaultNix && path.resolveSymlinks().lstat().type == SourceAccessor::tDirectory)
        return path / "default.nix";

    return path;
}

SourcePath resolveExprPath(const RootedPath & rooted, bool addDefaultNix)
{
    /* Internal-kinded paths (corepkgs, derivation-internal, ...) are
       trusted internal helpers; the kind-aware wrapper refuses to
       run on them by design. Fall back to the bare lenient
       resolver. */
    if (rooted.root->kind == SourceRootKind::Internal)
        return resolveExprPath(rooted.sourcePath(), addDefaultNix);

    /* For Copyable / System, replace the bare `resolveSymlinks` at
       each ancestor-walk step with the kind-aware wrapper. Copyable
       gets `StrictAccessorBoundary` — a path whose ancestor chain
       traverses a symlink whose target escapes the accessor root
       (e.g. `/link -> ../../foo`) raises `AccessorBoundaryEscape`
       rather than silently following the spliced target out of the
       tree. System keeps the historical lenient walker. */
    auto path = rooted.sourcePath();
    unsigned int followCount = 0, maxFollow = 1024;

    while (!path.path.isRoot()) {
        if (++followCount >= maxFollow)
            throw Error("too many symbolic links encountered while traversing the path '%s'", path);
        auto parentResolved = nix::resolveSymlinks(*rooted.root, path.path.parent().value());
        auto p = SourcePath{path.accessor, parentResolved} / path.baseName();
        if (p.lstat().type != SourceAccessor::tSymlink)
            break;
        path = {path.accessor, CanonPath(p.readLink(), path.path.parent().value_or(CanonPath::root))};
    }

    if (addDefaultNix) {
        auto resolved = nix::resolveSymlinks(*rooted.root, path.path);
        if (SourcePath{path.accessor, resolved}.lstat().type == SourceAccessor::tDirectory)
            return path / "default.nix";
    }

    return path;
}

Expr * EvalState::parseExprFromFile(const RootedPath & path)
{
    return parseExprFromFile(path, staticBaseEnv);
}

Expr * EvalState::parseExprFromFile(const RootedPath & path, const std::shared_ptr<StaticEnv> & staticEnv)
{
    auto buffer = path.sourcePath().resolveSymlinks().readFile();
    // readFile hopefully have left some extra space for terminators
    buffer.append("\0\0", 2);
    return parse(buffer.data(), buffer.size(), Pos::Origin(path), path.parent(), staticEnv);
}

Expr * EvalState::parseExprFromString(
    std::string s_, const RootedPath & basePath, const std::shared_ptr<StaticEnv> & staticEnv)
{
    // NOTE this method (and parseStdin) must take care to *fully copy* their input
    // into their respective Pos::Origin until the parser stops overwriting its input
    // data.
    auto s = make_ref<std::string>(s_);
    s_.append("\0\0", 2);
    return parse(s_.data(), s_.size(), Pos::String{.source = s}, basePath, staticEnv);
}

Expr * EvalState::parseExprFromString(std::string s, const RootedPath & basePath)
{
    return parseExprFromString(std::move(s), basePath, staticBaseEnv);
}

ExprAttrs *
EvalState::parseReplBindings(std::string s_, const RootedPath & basePath, const std::shared_ptr<StaticEnv> & staticEnv)
{
    return parseReplBindings(s_, s_, basePath, staticEnv);
}

ExprAttrs * EvalState::parseReplBindings(
    std::string s_, std::string errorSource, const RootedPath & basePath, const std::shared_ptr<StaticEnv> & staticEnv)
{
    auto s = make_ref<std::string>(std::move(errorSource));
    // flex requires two NUL terminators for yy_scan_buffer
    s_.append("\0\0", 2);
    return parseReplBindings(s_.data(), s_.size(), Pos::String{.source = s}, basePath, staticEnv);
}

Expr * EvalState::parseStdin()
{
    // NOTE this method (and parseExprFromString) must take care to *fully copy* their
    // input into their respective Pos::Origin until the parser stops overwriting its
    // input data.
    // Activity act(*logger, lvlTalkative, "parsing standard input");
    auto buffer = drainFD(0);
    // drainFD should have left some extra space for terminators
    buffer.append("\0\0", 2);
    auto s = make_ref<std::string>(buffer);
    return parse(buffer.data(), buffer.size(), Pos::Stdin{.source = s}, rootedPath("."), staticBaseEnv);
}

RootedPath EvalState::findFile(const std::string_view path)
{
    return findFile(lookupPath, path);
}

RootedPath EvalState::findFile(const LookupPath & lookupPath, const std::string_view path, const PosIdx pos)
{
    /* Wrap a lookup-path-resolved SourcePath as a RootedPath under
       the appropriate kind. LookupPath entries resolve under
       rootFS (System); the corepkgs hardcoded fallback uses the
       Internal-kinded corepkgsRoot so its positions and string
       coercions surface as nix-internal rather than user-visible. */
    auto wrap = [&](SourcePath sp) -> RootedPath {
        if (&*sp.accessor == &*corepkgsFS.cast<SourceAccessor>())
            return {corepkgsRoot, sp.path};
        return {rootFSRoot, sp.path};
    };

    for (auto & i : lookupPath.elements) {
        auto suffixOpt = i.prefix.suffixIfPotentialMatch(path);

        if (!suffixOpt)
            continue;
        auto suffix = *suffixOpt;

        auto rOpt = resolveLookupPathPath(i.path);
        if (!rOpt)
            continue;
        auto r = *rOpt;

        auto suffixPath = CanonPath(suffix);
        if (auto cachedRes = getConcurrent(*rOpt->resolvedPaths, suffixPath)) {
            if (*cachedRes)
                return wrap(**cachedRes);
            else
                // Cached negative lookup.
                continue;
        }

        auto res = (r.path / suffixPath).resolveSymlinks();
        if (res.pathExists()) {
            r.resolvedPaths->emplace(suffixPath, res);
            return wrap(res);
        }

        // Backward compatibility hack: throw an exception if access
        // to this path is not allowed.
        if (auto accessor = res.accessor.dynamic_pointer_cast<FilteringSourceAccessor>())
            accessor->checkAccess(res.path);

        // Cache negative lookups too.
        r.resolvedPaths->emplace(suffixPath, std::nullopt);
    }

    if (hasPrefix(path, "nix/"))
        return {corepkgsRoot, CanonPath(path.substr(3))};

    error<ThrownError>(
        settings.pureEval ? "cannot look up '<%s>' in pure evaluation mode (use '--impure' to override)"
                          : "file '%s' was not found in the Nix search path (add it using $NIX_PATH or -I)",
        path)
        .atPos(pos)
        .debugThrow();
}

std::shared_ptr<EvalState::LookupPathResolvedState>
EvalState::resolveLookupPathPath(const LookupPath::Path & value0, bool initAccessControl)
{
    auto & value = value0.s;
    if (auto cached = getConcurrent(*lookupPathResolved, value))
        return *cached;

    auto finish = [&](std::optional<SourcePath> maybePath) {
        std::shared_ptr<LookupPathResolvedState> res;
        if (maybePath) {
            debug("resolved search path element '%s' to '%s'", value, *maybePath);
            res = std::make_shared<LookupPathResolvedState>(
                *maybePath, make_ref<decltype(LookupPathResolvedState::resolvedPaths)::element_type>());
        } else {
            debug("failed to resolve search path element '%s'", value);
        }
        lookupPathResolved->emplace(std::string(value), res);
        return res;
    };

    if (EvalSettings::isPseudoUrl(value)) {
        try {
            auto accessor = fetchers::downloadTarball(*store, fetchSettings, EvalSettings::resolvePseudoUrl(value));
            auto storePath = fetchToStore(fetchSettings, *store, SourcePath(accessor), FetchMode::Copy);
            return finish(this->storePath(storePath));
        } catch (Error & e) {
            logWarning({.msg = HintFmt("Nix search path entry '%1%' cannot be downloaded, ignoring", value)});
        }
    }

    if (auto colPos = value.find(':'); colPos != value.npos) {
        auto scheme = value.substr(0, colPos);
        auto rest = value.substr(colPos + 1);
        if (auto * hook = get(settings.lookupPathHooks, scheme)) {
            auto res = (*hook)(*this, rest);
            if (res)
                return finish(std::move(*res));
        }
    }

    {
        auto path = rootPath(value);

        /* Allow access to paths in the search path. */
        if (initAccessControl) {
            allowPathLegacy(path.path.abs());
            if (store->isInStore(path.path.abs())) {
                try {
                    allowClosure(store->toStorePath(path.path.abs()).first);
                } catch (InvalidPath &) {
                }
            }
        }

        if (path.resolveSymlinks().pathExists())
            return finish(std::move(path));
        else {
            // Backward compatibility hack: throw an exception if access
            // to this path is not allowed.
            if (auto accessor = path.accessor.dynamic_pointer_cast<FilteringSourceAccessor>())
                accessor->checkAccess(path.path);

            logWarning({.msg = HintFmt("Nix search path entry '%1%' does not exist, ignoring", value)});
        }
    }

    return finish(std::nullopt);
}

Expr * EvalState::parse(
    char * text,
    size_t length,
    Pos::Origin origin,
    const RootedPath & basePath,
    const std::shared_ptr<StaticEnv> & staticEnv)
{
    auto tmpDocComments = make_ref<DocCommentMap>();

    auto result = parseExprFromBuf(
        text, length, origin, basePath, mem.exprs, symbols, settings, positions, *tmpDocComments, rootFSRoot);

    result->bindVars(*this, staticEnv);

    if (auto rooted = std::get_if<RootedPath>(&origin))
        /* A single file might appear multiple times in PosTable if it's
           parsed by scopedImport. If we are the first then emplace into the map, otherwise
           copy our positions into the existing map. */
        positionToDocComment->emplace_or_visit(rooted->sourcePath(), tmpDocComments, [&tmpDocComments](auto & kv) {
            kv.second->insert(tmpDocComments->begin(), tmpDocComments->end());
        });

    return result;
}

ExprAttrs * EvalState::parseReplBindings(
    char * text,
    size_t length,
    Pos::Origin origin,
    const RootedPath & basePath,
    const std::shared_ptr<StaticEnv> & staticEnv)
{
    auto tmpDocComments = make_ref<DocCommentMap>();

    auto bindings = parseReplBindingsFromBuf(
        text, length, origin, basePath, mem.exprs, symbols, settings, positions, *tmpDocComments, rootFSRoot);
    assert(bindings);

    bindings->bindVars(*this, staticEnv);

    if (auto rooted = std::get_if<RootedPath>(&origin))
        /* A single file might appear multiple times in PosTable if it's
           parsed by scopedImport. If we are the first then emplace into the map, otherwise
           copy our positions into the existing map. */
        positionToDocComment->emplace_or_visit(rooted->sourcePath(), tmpDocComments, [&tmpDocComments](auto & kv) {
            kv.second->insert(tmpDocComments->begin(), tmpDocComments->end());
        });

    return bindings;
}

DocComment EvalState::getDocCommentForPos(PosIdx pos)
{
    auto pos2 = positions[pos];
    auto path = pos2.getSourcePath();
    if (!path)
        return {};

    DocComment result;
    positionToDocComment->visit(*path, [&](const auto & kv) {
        if (auto it = kv.second->find(pos); it != kv.second->end())
            result = it->second;
    });
    return result;
}

std::string ExternalValueBase::coerceToString(
    EvalState & state, const PosIdx & pos, NixStringContext & context, bool copyMore, bool copyToStore) const
{
    state.error<TypeError>("cannot coerce %1% to a string: %2%", showType(), *this).atPos(pos).debugThrow();
}

bool ExternalValueBase::operator==(const ExternalValueBase & b) const noexcept
{
    return false;
}

std::ostream & operator<<(std::ostream & str, const ExternalValueBase & v)
{
    return v.print(str);
}

void forceNoNullByte(std::string_view s, std::function<Pos()> pos)
{
    if (s.find('\0') != s.npos) {
        using namespace std::string_view_literals;
        auto str = replaceStrings(std::string(s), "\0"sv, "␀"sv);
        Error error("input string '%s' cannot be represented as Nix string because it contains null bytes", str);
        if (pos) {
            error.atPos(pos());
        }
        throw std::move(error);
    }
}

} // namespace nix
