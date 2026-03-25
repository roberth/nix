#pragma once
///@file

#include "nix/expr/attr-set.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/eval-profiler.hh"
#include "nix/util/types.hh"
#include "nix/expr/value.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/util/configuration.hh"
#include "nix/util/experimental-features.hh"
#include "nix/util/sync.hh"
#include "nix/util/position.hh"
#include "nix/util/pos-table.hh"
#include "nix/util/source-accessor.hh"
#include "nix/expr/search-path.hh"
#include "nix/expr/repl-exit-status.hh"
#include "nix/util/ref.hh"
#include "nix/expr/counter.hh"

// For `NIX_USE_BOEHMGC`, and if that's set, `GC_THREADS`
#include "nix/expr/config.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/concurrent_flat_map_fwd.hpp>
#include <boost/unordered/concurrent_flat_set_fwd.hpp>

#include <compare>
#include <map>
#include <optional>
#include <functional>
#include <span>

namespace nix {

/**
 * We put a limit on primop arity because it lets us use a fixed size array on
 * the stack. 8 is already an impractical number of arguments. Use an attrset
 * argument for such overly complicated functions.
 */
constexpr size_t maxPrimOpArity = 8;

class Store;
class Environment;
class SystemEnvironment;
class Evaluator;
class Object;

namespace fetchers {
struct Settings;
struct InputCache;
struct Input;
} // namespace fetchers
struct EvalSettings;
class EvalState;
class StorePath;
struct SingleDerivedPath;
enum RepairFlag : bool;
struct MemorySourceAccessor;
struct MountedSourceAccessor;

namespace eval_cache {
class EvalCache;
}

/**
 * Increments a count on construction and decrements on destruction.
 */
class CallDepth
{
    size_t & count;

public:
    CallDepth(size_t & count)
        : count(count)
    {
        ++count;
    }

    ~CallDepth()
    {
        --count;
    }
};

/**
 * Function that implements a primop.
 */
using PrimOpFun = void(EvalState & state, const PosIdx pos, Value ** args, Value & v);

/**
 * Info about a primitive operation, and its implementation
 */
struct PrimOp
{
    /**
     * Name of the primop. `__` prefix is treated specially.
     */
    std::string name;

    /**
     * Names of the parameters of a primop, for primops that take a
     * fixed number of arguments to be substituted for these parameters.
     */
    std::vector<std::string> args;

    /**
     * Aritiy of the primop.
     *
     * If `args` is not empty, this field will be computed from that
     * field instead, so it doesn't need to be manually set.
     */
    size_t arity = 0;

    /**
     * Optional free-form documentation about the primop.
     */
    std::optional<std::string> doc;

    /**
     * Add a trace item, while calling the `<name>` builtin.
     *
     * This is used to remove the redundant item for `builtins.addErrorContext`.
     */
    bool addTrace = true;

    /**
     * Implementation of the primop.
     */
    fun<PrimOpFun> impl;

    /**
     * Optional experimental for this to be gated on.
     */
    std::optional<ExperimentalFeature> experimentalFeature;

    /**
     * If true, this primop is not exposed to the user.
     */
    bool internal = false;

    /**
     * Validity check to be performed by functions that introduce primops,
     * such as RegisterPrimOp() and Value::mkPrimOp().
     */
    void check();
};

std::ostream & operator<<(std::ostream & output, const PrimOp & primOp);

/**
 * Info about a constant
 */
struct Constant
{
    /**
     * Optional type of the constant (known since it is a fixed value).
     *
     * @todo we should use an enum for this.
     */
    ValueType type = nThunk;

    /**
     * Optional free-form documentation about the constant.
     */
    const char * doc = nullptr;

    /**
     * Whether the constant is impure, and not available in pure mode.
     */
    bool impureOnly = false;
};

typedef std::
    map<std::string, Value *, std::less<std::string>, traceable_allocator<std::pair<const std::string, Value *>>>
        ValMap;

typedef boost::unordered_flat_map<PosIdx, DocComment, std::hash<PosIdx>> DocCommentMap;

struct Env
{
    Env * up;
    Value * values[0];
};

void printEnvBindings(const EvalState & es, const Expr & expr, const Env & env);
void printEnvBindings(const SymbolTable & st, const StaticEnv & se, const Env & env, int lvl = 0);

std::unique_ptr<ValMap> mapStaticEnvBindings(const SymbolTable & st, const StaticEnv & se, const Env & env);

void copyContext(
    const Value & v,
    NixStringContext & context,
    const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings);

std::string printValue(EvalState & state, Value & v);
std::ostream & operator<<(std::ostream & os, const ValueType t);

struct RegexCache;

ref<RegexCache> makeRegexCache();

struct DebugTrace
{
    /* WARNING: Converting PosIdx -> Pos should be done with extra care. This is
       due to the fact that operator[] of PosTable is incredibly expensive. */
    std::variant<Pos, PosIdx> pos;
    const Expr & expr;
    const Env & env;
    HintFmt hint;
    bool isError;

    Pos getPos(const PosTable & table) const
    {
        return std::visit(
            overloaded{
                [&](PosIdx idx) {
                    // Prefer direct pos, but if noPos then try the expr.
                    if (!idx)
                        idx = expr.getPos();
                    return table[idx];
                },
                [&](Pos pos) { return pos; },
            },
            pos);
    }
};

struct StaticEvalSymbols
{
    Symbol with, outPath, drvPath, type, meta, name, value, system, overrides, outputs, outputName, ignoreNulls, file,
        line, column, functor, toString, right, wrong, structuredAttrs, json, allowedReferences, allowedRequisites,
        disallowedReferences, disallowedRequisites, maxSize, maxClosureSize, builder, args, contentAddressed, impure,
        outputHash, outputHashAlgo, outputHashMode, recurseForDerivations, description, self, epsilon, startSet,
        operator_, key, path, prefix, outputSpecified;

    Expr::AstSymbols exprSymbols;

    static constexpr auto preallocate()
    {
        StaticSymbolTable alloc;

        StaticEvalSymbols staticSymbols = {
            .with = alloc.create("<with>"),
            .outPath = alloc.create("outPath"),
            .drvPath = alloc.create("drvPath"),
            .type = alloc.create("type"),
            .meta = alloc.create("meta"),
            .name = alloc.create("name"),
            .value = alloc.create("value"),
            .system = alloc.create("system"),
            .overrides = alloc.create("__overrides"),
            .outputs = alloc.create("outputs"),
            .outputName = alloc.create("outputName"),
            .ignoreNulls = alloc.create("__ignoreNulls"),
            .file = alloc.create("file"),
            .line = alloc.create("line"),
            .column = alloc.create("column"),
            .functor = alloc.create("__functor"),
            .toString = alloc.create("__toString"),
            .right = alloc.create("right"),
            .wrong = alloc.create("wrong"),
            .structuredAttrs = alloc.create("__structuredAttrs"),
            .json = alloc.create("__json"),
            .allowedReferences = alloc.create("allowedReferences"),
            .allowedRequisites = alloc.create("allowedRequisites"),
            .disallowedReferences = alloc.create("disallowedReferences"),
            .disallowedRequisites = alloc.create("disallowedRequisites"),
            .maxSize = alloc.create("maxSize"),
            .maxClosureSize = alloc.create("maxClosureSize"),
            .builder = alloc.create("builder"),
            .args = alloc.create("args"),
            .contentAddressed = alloc.create("__contentAddressed"),
            .impure = alloc.create("__impure"),
            .outputHash = alloc.create("outputHash"),
            .outputHashAlgo = alloc.create("outputHashAlgo"),
            .outputHashMode = alloc.create("outputHashMode"),
            .recurseForDerivations = alloc.create("recurseForDerivations"),
            .description = alloc.create("description"),
            .self = alloc.create("self"),
            .epsilon = alloc.create(""),
            .startSet = alloc.create("startSet"),
            .operator_ = alloc.create("operator"),
            .key = alloc.create("key"),
            .path = alloc.create("path"),
            .prefix = alloc.create("prefix"),
            .outputSpecified = alloc.create("outputSpecified"),
            .exprSymbols = {
                .sub = alloc.create("__sub"),
                .lessThan = alloc.create("__lessThan"),
                .mul = alloc.create("__mul"),
                .div = alloc.create("__div"),
                .or_ = alloc.create("or"),
                .findFile = alloc.create("__findFile"),
                .nixPath = alloc.create("__nixPath"),
                .body = alloc.create("body"),
            }};

        return std::pair{staticSymbols, alloc};
    }

    static consteval StaticEvalSymbols create()
    {
        return preallocate().first;
    }

    static constexpr StaticSymbolTable staticSymbolTable()
    {
        return preallocate().second;
    }
};

class EvalMemory
{
public:
    struct Statistics
    {
        Counter nrEnvs;
        Counter nrValuesInEnvs;
        Counter nrValues;
        Counter nrAttrsets;
        Counter nrAttrsInAttrsets;
        Counter nrListElems;
    };

    EvalMemory();

    EvalMemory(const EvalMemory &) = delete;
    EvalMemory(EvalMemory &&) = delete;
    EvalMemory & operator=(const EvalMemory &) = delete;
    EvalMemory & operator=(EvalMemory &&) = delete;

    inline void * allocBytes(size_t n);
    inline Value * allocValue();
    inline Env & allocEnv(size_t size);

    Bindings * allocBindings(size_t capacity);

    BindingsBuilder buildBindings(SymbolTable & symbols, size_t capacity)
    {
        return BindingsBuilder(*this, symbols, allocBindings(capacity), capacity);
    }

    ListBuilder buildList(size_t size)
    {
        stats.nrListElems += size;
        return ListBuilder(*this, size);
    }

    const Statistics & getStats() const &
    {
        return stats;
    }

    /**
     * Storage for the AST nodes
     */
    Exprs exprs;

private:
    Statistics stats;
};

class EvalState : public std::enable_shared_from_this<EvalState>
{
public:
    static constexpr StaticEvalSymbols s = StaticEvalSymbols::create();

    const fetchers::Settings & fetchSettings;
    const EvalSettings & settings;

    SymbolTable symbols;
    PosTable positions;

    EvalMemory mem;

    /**
     * If set, force copying files to the Nix store even if they
     * already exist there.
     */
    RepairFlag repair;

    /**
     * Environment for evaluation I/O operations.
     */
    ref<Environment> environment;

    /**
     * System environment (deprecated: use environment interface instead).
     * @deprecated Direct access to SystemEnvironment will be removed.
     */
    [[deprecated("Use this->environment interface instead, or keep your own reference to the system environment")]]
    ref<SystemEnvironment> systemEnvironment;

    /**
     * The accessor for the root filesystem.
     *
     * (convenience reference to `environment`'s rootfs)
     *
     * Kept public on the lazy-paths base: lazy-paths' `flake.cc` reads
     * `state.rootFS` directly for accessor-identity comparison. The
     * eval-cache-next intent of routing this through
     * `this->environment->fsRoot()` was aspirational and predates that
     * use; the simpler reconciliation is to leave the API alone.
     */
    const ref<SourceAccessor> rootFS;

    /**
     * The in-memory filesystem for <nix/...> paths.
     */
    const ref<MemorySourceAccessor> corepkgsFS;

    /**
     * In-memory filesystem for internal, non-user-callable Nix
     * expressions like `derivation.nix`.
     */
    const ref<MemorySourceAccessor> internalFS;

    /**
     * The SourceRoot wrapping `rootFS` under which every rootFS-rooted
     * path Value is admitted (System-kinded). Owned here so the same
     * shared `ref<SourceRoot>` flows into every admission seam (the
     * parser, `findFile`, `rootPath`-derived call sites).
     */
    const ref<SourceRoot> rootFSRoot;

    /**
     * SourceRoot for `corepkgsFS` (Internal-kinded). Used to admit
     * Nix-internal helper files into the parser without surfacing
     * them as user-visible path Values.
     */
    const ref<SourceRoot> corepkgsRoot;

    /**
     * SourceRoot for `internalFS` (Internal-kinded).
     */
    const ref<SourceRoot> internalFSRoot;

    /**
     * Memoise the `SourceRoot` wrapping `accessor` under `kind`.
     * Pinned for `EvalState`'s lifetime so the raw `SourceRoot *`
     * stored on path Values (`Value::pathRoot()`) stays live.
     * The Value is GC-managed; the `SourceRoot` lives in a
     * `shared_ptr` side allocation, with the raw pointer being
     * the bridge — the cache is the external anchor that keeps
     * that pointer dereferenceable. `Pos::Origin`'s `RootedPath`
     * arm holds `ref<SourceRoot>` directly and self-anchors;
     * it doesn't depend on the cache.
     *
     * Keyed on `(accessor, kind)`, not on accessor alone: the
     * same accessor can be admitted multiple times under
     * different kinds (e.g. a posix accessor exposed both as a
     * System filesystem view and as a Copyable per-storepath view)
     * — see `SourceRoot`'s docstring. Keying on accessor alone
     * would let the first admission's kind silently win for every
     * subsequent lookup, which makes the cache an unintended
     * claim about the data model rather than a pure optimisation.
     *
     * Today all callers pass `Copyable` (fetcher accessors), but
     * the (accessor, kind) shape keeps the door open for a
     * future System or Internal caller without making the cache
     * structurally wrong in the meantime.
     */
    /**
     * @param unpinnedId Identity claim from the producer (e.g.
     * `Input::toUnpinnedURL()` from `fetchTree`). Stamped onto the
     * SourceRoot on cache miss; ignored on cache hit (the first
     * admission's id is sticky — see `getOrCreateRoot`'s test for
     * the rationale).
     */
    ref<SourceRoot> getOrCreateRoot(
        ref<SourceAccessor> accessor, SourceRootKind kind, std::optional<std::string> unpinnedId = std::nullopt);

    const SourcePath derivationInternal;

    const SourcePath importedDrvToDerivation;

    const ref<fetchers::InputCache> inputCache;

    /**
     * Debugger
     */
    ReplExitStatus (*debugRepl)(ref<EvalState> es, const ValMap & extraEnv);
    bool debugStop;
    bool inDebugger = false;
    int trylevel;
    std::list<DebugTrace> debugTraces;
    boost::unordered_flat_map<const Expr *, const std::shared_ptr<const StaticEnv>> exprEnvs;

    const std::shared_ptr<const StaticEnv> getStaticEnv(const Expr & expr) const
    {
        auto i = exprEnvs.find(&expr);
        if (i != exprEnvs.end())
            return i->second;
        else
            return std::shared_ptr<const StaticEnv>();
        ;
    }

    /** Whether a debug repl can be started. If `false`, `runDebugRepl(error)` will return without starting a repl. */
    bool canDebug();

    /** Use front of `debugTraces`; see `runDebugRepl(error,env,expr)` */
    void runDebugRepl(const Error * error);

    /**
     * Run a debug repl with the given error, environment and expression.
     * @param error The error to debug, may be nullptr.
     * @param env The environment to debug, matching the expression.
     * @param expr The expression to debug, matching the environment.
     */
    void runDebugRepl(const Error * error, const Env & env, const Expr & expr);

    template<class T, typename... Args>
    [[nodiscard, gnu::noinline]]
    EvalErrorBuilder<T> & error(const Args &... args)
    {
        // `EvalErrorBuilder::debugThrow` performs the corresponding `delete`.
        return *new EvalErrorBuilder<T>(*this, args...);
    }

    /**
     * A cache for evaluation caches, so as to reuse the same root value if possible
     */
    std::map<const Hash, ref<eval_cache::EvalCache>> evalCaches;

    /**
     * Weak reference to the Evaluator wrapping this EvalState.
     * Weak to avoid a reference cycle (Interpreter holds ref<EvalState>).
     *
     * @deprecated Transitional bridge for migrating Value-based code to Object-based.
     * Remove once all callers use Evaluator/Object directly.
     */
    std::weak_ptr<Evaluator> evaluatorCompat;

    /**
     * Get (or create) an Evaluator wrapping this EvalState.
     *
     * @deprecated Transitional bridge for migrating Value-based code to Object-based.
     * Callers should eventually receive an Evaluator from their constructor instead.
     */
    ref<Evaluator> toEvaluatorCompat();

    /**
     * Wrap a Value as an Object.
     *
     * @deprecated Transitional bridge for migrating Value-based code to Object-based.
     * Callers should eventually receive Objects from Evaluator methods instead.
     */
    ref<Object> toObjectCompat(Value & v);

    /* Set of accessor pairs known to be NAR-inequivalent, populated
       by `accessorsEquivalent` when any of its decisive probes (root
       directory name set, hint subpath SHA256, computed storePath
       compare) returns false. Membership is symmetric; the canonical
       key orders the two raw pointers low-then-high. Avoids repeated
       work across multiple comparisons of the same pair — without
       this, a pair already disproven by a storePath compare could
       still pay a hint read on a later call. Public so test code
       can pin the cache-population contract. */
    const ref<boost::concurrent_flat_set<std::pair<SourceAccessor *, SourceAccessor *>>> accessorsKnownInequivalent;

    /* Per-accessor SHA256 of the sorted name list of its root
       directory's immediate children, computed lazily on first
       probe. Stable byte-level signature: NUL-separated names in
       lex order. Two accessors whose hashes differ cannot be
       NAR-equivalent (their root nodes already differ at the level
       of entry sets), so the hash mismatch decisively disproves
       equivalence. Match is no info — same entry set says nothing
       about the file contents underneath. */
    const ref<boost::concurrent_flat_map<SourceAccessor *, Hash>> accessorRootProbeCache;

    /* Per-(accessor, subpath) SHA256 of a regular file's contents
       at that subpath, computed lazily on first probe. `nullopt`
       means the subpath isn't a regular file on that accessor
       (absent, directory, symlink, etc.); cached identically so
       repeated probes don't re-walk. Used by `accessorsEquivalent`
       as a hint: when the caller is comparing two roots and is
       about to read the same subpath on each anyway, hashing it
       under the hint contract turns the read into a chance to
       disprove root-equivalence cheaply. Mismatched hashes prove
       the roots are inequivalent; matching hashes are no info
       (one matching file says nothing about the rest of the tree). */
    const ref<boost::concurrent_flat_map<std::pair<SourceAccessor *, CanonPath>, std::optional<Hash>>>
        accessorHintProbeCache;

private:

    /* Cache for calls to addToStore(): maps source paths to the store
       paths and the NAR hash from the same walk. Carrying both lets
       `lockInput`'s narHash LazyAttr reuse what a previous
       `copyPathToStore` already computed (and vice versa, mediated by
       fetchToStore2's own cache when the accessor provides a
       fingerprint). */
    const ref<boost::concurrent_flat_map<SourcePath, std::pair<StorePath, Hash>>> srcToStore;

    /**
     * A cache that maps paths to "resolved" paths for importing Nix
     * expressions, i.e. `/foo` to `/foo/default.nix`.
     */
    const ref<boost::concurrent_flat_map<SourcePath, SourcePath>> importResolutionCache;

    /**
     * A cache from resolved paths to values.
     */
    const ref<boost::concurrent_flat_map<
        SourcePath,
        Value *,
        std::hash<SourcePath>,
        std::equal_to<SourcePath>,
        traceable_allocator<std::pair<const SourcePath, Value *>>>>
        fileEvalCache;

    /**
     * Associate source positions of certain AST nodes with their preceding doc comment, if they have one.
     * Grouped by file.
     */
    const ref<boost::concurrent_flat_map<SourcePath, ref<DocCommentMap>>> positionToDocComment;

    LookupPath lookupPath;

    struct LookupPathResolvedState
    {
        SourcePath path;
        const ref<boost::concurrent_flat_map<CanonPath, std::optional<SourcePath>>> resolvedPaths;
    };

public:

    enum class CopyLazyPaths : bool {
        PreserveLazy = false,
        Copy = true,
    };

    struct Doc
    {
        Pos pos;
        std::optional<std::string> name;
        size_t arity;
        std::vector<std::string> args;
        /**
         * Unlike the other `doc` fields in this file, this one should never be
         * `null`.
         */
        const char * doc;
    };

private:

    const ref<
        boost::
            concurrent_flat_map<std::string, std::shared_ptr<LookupPathResolvedState>, StringViewHash, std::equal_to<>>>
        lookupPathResolved;

    /**
     * Cache used by prim_match().
     */
    const ref<RegexCache> regexCache;

    /**
     * Backing map for `getOrCreateRoot`. Pinned for the eval's
     * lifetime so the raw `SourceRoot *` stored on Values and
     * `Pos::Origin`s never dangles. Keyed on `(accessor, kind)` —
     * the same accessor admitted under different kinds is two
     * separate `SourceRoot`s with disjoint cache entries.
     *
     * The map keys on a raw `SourceAccessor *` (paired with
     * `SourceRootKind`), but the value is `ref<SourceRoot>` and
     * `SourceRoot` itself holds a `ref<SourceAccessor>`. So the
     * value owns the accessor that its own key points at: as long
     * as the entry is in the map, the key is dereferenceable. No
     * eviction is ever performed, and `Value::path()` documents
     * how callers extend root lifetimes when needed, so the
     * raw-pointer key is sound by construction.
     */
    const ref<boost::concurrent_flat_map<std::pair<SourceAccessor *, SourceRootKind>, ref<SourceRoot>>> rootCache;

    /**
     * Backing maps for `allocSourceUnpinnedId`. Per-EvalState
     * in-memory; identifiers are not stable across processes (and
     * don't need to be — replay re-derives them by re-running the
     * same eval).
     *
     * `sourceUnpinnedIds` memoises the `(url, accessor) -> identifier`
     * decision so two calls for the same root return the same id.
     * `sourceUnpinnedIdCounters` allocates the next `n` per URL —
     * `Sync<map>` because the increment-and-emit is one atomic step.
     *
     * The accessor pointer in the key is sound for the same reason
     * `rootCache` is sound: the SourceRoot that holds the accessor
     * is itself pinned in `rootCache` for the eval's lifetime.
     */
    const ref<boost::concurrent_flat_map<std::pair<std::string, SourceAccessor *>, std::string>> sourceUnpinnedIds;
    const ref<Sync<std::map<std::string, size_t>>> sourceUnpinnedIdCounters;

private:
    // Helper to support the legacy EvalState constructor
    EvalState(
        const LookupPath & _lookupPath,
        const fetchers::Settings & fetchSettings,
        const EvalSettings & settings,
        ref<SystemEnvironment> systemEnvironment);

public:

    /**
     * Turn a `SourceRoot` into the identifier the eval cache uses to
     * key path values. Returns `nullopt` for roots without a stamped
     * `unpinnedId` (Internal helpers, throwaway probes — they bypass
     * identifier-keyed caching). For stamped roots returns
     * `"<url>#<n>"` where `n` is a per-URL counter scoped to this
     * EvalState; the `#<n>` is always emitted.
     */
    std::optional<std::string> allocSourceUnpinnedId(SourceRoot & root);

    /**
     * @param lookupPath     Only used during construction.
     * @param store          The store to use for instantiation
     * @param fetchSettings  Must outlive the lifetime of this EvalState!
     * @param settings       Must outlive the lifetime of this EvalState!
     * @param buildStore     The store to use for builds ("import from derivation", C API `nix_string_realise`)
     */
    EvalState(
        const LookupPath & lookupPath,
        ref<Store> store,
        const fetchers::Settings & fetchSettings,
        const EvalSettings & settings,
        std::shared_ptr<Store> buildStore = nullptr);

    EvalState(
        // TODO move lookupPath and/or individual lookups to environment
        const LookupPath & _lookupPath,
        const fetchers::Settings & fetchSettings,
        const EvalSettings & settings,
        ref<Environment> environment,
        ref<SystemEnvironment> systemEnvironment);

    ~EvalState();

    /**
     * A wrapper around EvalMemory::allocValue() to avoid code churn when it
     * was introduced.
     */
    inline Value * allocValue()
    {
        return mem.allocValue();
    }

    LookupPath getLookupPath()
    {
        return lookupPath;
    }

    /**
     * Return a `SourcePath` that refers to `path` in the root
     * filesystem.
     */
    SourcePath rootPath(CanonPath path);

    /**
     * Variant which accepts relative paths too.
     */
    SourcePath rootPath(std::string_view path);

    /**
     * Like `rootPath`, but admitted under the System-kinded
     * `rootFSRoot`. Use this at sites that produce path Values or
     * `RootedPath` arguments to the parser; the kind reflects that
     * the path is meant to be interpreted as a filesystem path.
     */
    RootedPath rootedPath(CanonPath path);
    RootedPath rootedPath(std::string_view path);

    /**
     * Return a `SourcePath` that refers to `path` in the store.
     *
     * For now, this has to also be within the root filesystem for
     * backwards compat, but for Windows and maybe also pure eval, we'll
     * probably want to do something different.
     */
    SourcePath storePath(const StorePath & path);

    /**
     * Allow access to a path.
     *
     * Only for restrict eval: pure eval just whitelist store paths,
     * never arbitrary paths.
     */
    void allowPathLegacy(const std::string & path);

    /**
     * Allow access to a store path. Note that this gets remapped to
     * the real store path if `store` is a chroot store.
     */
    void allowPath(const StorePath & storePath);

    /**
     * Allow access to the closure of a store path.
     */
    void allowClosure(const StorePath & storePath);

    /**
     * Allow access to a store path and return it as a string.
     */
    void allowAndSetStorePathString(const StorePath & storePath, Value & v);

    void checkURI(const std::string & uri);

    /**
     * Mount an already-locked input (one whose `narHash` is known) on
     * the Nix store. Derives the storePath from the narHash via the
     * fixed-output formula, sets up the `storeFS` mount + allowlist,
     * and pre-populates `srcToStore` so subsequent string coercion of
     * a path-typed value at the accessor's root returns the storePath
     * without walking. The walk+copy is deferred to
     * `ensureLazyPathCopied`.
     *
     * If `originalInput` itself asserts a `narHash` (e.g. from a
     * `builtins.fetchGit { narHash = ...; }` literal), the assertion
     * is verified against `fetchToStore2(DryRun)`. The hash is
     * computed by walking the accessor only on the *first* retrieval
     * of a given fingerprint (typically a git rev); subsequent calls
     * hit the `sourcePathToHash` cache and return without reading any
     * blobs. This keeps the eval-time safety of upstream's sync
     * verification without paying for it on every evaluation.
     *
     * Callers must ensure the input has a narHash; for unlocked
     * inputs, call `lockInput` first.
     */
    StorePath mountInput(fetchers::Input & input, const fetchers::Input & originalInput, ref<SourceAccessor> accessor);

    /**
     * Compute narHash for an input (so the lockfile sees it as
     * locked) and surface it on `input.attrs`, without mounting on
     * `storeFS` or allowlisting a storePath. Use this from flake
     * loading where path values stay accessor-rooted.
     */
    void lockInput(fetchers::Input & input, const fetchers::Input & originalInput, ref<SourceAccessor> accessor);

    /**
     * Parse a Nix expression from the specified file. The
     * `RootedPath`'s `SourceRoot` carries the language-level kind
     * under which the file is admitted (System for filesystem
     * loads, Copyable for fetched-tree loads, Internal for
     * nix-internal helpers).
     */
    Expr * parseExprFromFile(const RootedPath & path);
    Expr * parseExprFromFile(const RootedPath & path, const std::shared_ptr<StaticEnv> & staticEnv);

    /**
     * Parse a Nix expression from the specified string. `basePath`
     * names the file's surrounding directory under its `SourceRoot`.
     */
    Expr *
    parseExprFromString(std::string s, const RootedPath & basePath, const std::shared_ptr<StaticEnv> & staticEnv);
    Expr * parseExprFromString(std::string s, const RootedPath & basePath);

    /**
     * Parse REPL bindings from the specified string.
     * Returns ExprAttrs with bindings to add to scope.
     */
    ExprAttrs *
    parseReplBindings(std::string s, const RootedPath & basePath, const std::shared_ptr<StaticEnv> & staticEnv);
    ExprAttrs * parseReplBindings(
        std::string s,
        std::string errorSource,
        const RootedPath & basePath,
        const std::shared_ptr<StaticEnv> & staticEnv);

    Expr * parseStdin();

    /**
     * Evaluate an expression read from the given file to normal
     * form. Optionally enforce that the top-level expression is
     * trivial (i.e. doesn't require arbitrary computation).
     */
    void evalFile(const RootedPath & path, Value & v, bool mustBeTrivial = false);

    void resetFileCache();

    /**
     * Insert a speculatively parsed file into the file eval cache.
     * When the file is demanded during evaluation, emitTrace is called
     * to emit the deferred file read trace.
     */
    void insertPreloadedParsedFile(const SourcePath & path, Expr * expr, std::function<void()> emitTrace);

    /**
     * Look up a file in the search path.
     */
    RootedPath findFile(const std::string_view path);
    RootedPath findFile(const LookupPath & lookupPath, const std::string_view path, const PosIdx pos = noPos);

    /**
     * Try to resolve a search path value (not the optional key part).
     *
     * If the specified search path element is a URI, download it.
     *
     * If it is not found, return `nullptr`.
     */
    std::shared_ptr<LookupPathResolvedState>
    resolveLookupPathPath(const LookupPath::Path & elem, bool initAccessControl = false);

    /**
     * Evaluate an expression to normal form
     *
     * @param [out] v The resulting is stored here.
     */
    void eval(Expr * e, Value & v);

    /**
     * Evaluation the expression, then verify that it has the expected
     * type.
     */
    inline bool evalBool(Env & env, Expr * e);
    inline bool evalBool(Env & env, Expr * e, const PosIdx pos, std::string_view errorCtx);
    inline void evalAttrs(Env & env, Expr * e, Value & v, const PosIdx pos, std::string_view errorCtx);

    /**
     * If `v` is a thunk, enter it and overwrite `v` with the result
     * of the evaluation of the thunk.  If `v` is a delayed function
     * application, call the function and overwrite `v` with the
     * result.  Otherwise, this is a no-op.
     */
    inline void forceValue(Value & v, const PosIdx pos);

private:

    /**
     * Internal support function for forceValue
     *
     * This code is factored out so that it's not in the heavily inlined hot path.
     */
    void handleEvalExceptionForThunk(Env * env, Expr * expr, Value & v, const PosIdx pos);

    /**
     * Internal support function for forceValue
     *
     * This code is factored out so that it's not in the heavily inlined hot path.
     */
    void handleEvalExceptionForApp(Value & v, const Value & savedApp);

    void handleEvalFailed(Value & v, PosIdx pos);

    void tryFixupBlackHolePos(Value & v, PosIdx pos);

public:

    /**
     * Force a value, then recursively force list elements and
     * attributes.
     */
    void forceValueDeep(Value & v);

    /**
     * Force `v`, and then verify that it has the expected type.
     */
    NixInt forceInt(Value & v, const PosIdx pos, std::string_view errorCtx);
    NixFloat forceFloat(Value & v, const PosIdx pos, std::string_view errorCtx);
    bool forceBool(Value & v, const PosIdx pos, std::string_view errorCtx);

    void forceAttrs(Value & v, const PosIdx pos, std::string_view errorCtx);

    template<typename Callable>
    inline void forceAttrs(Value & v, Callable getPos, std::string_view errorCtx);

    inline void forceList(Value & v, const PosIdx pos, std::string_view errorCtx);
    /**
     * @param v either lambda or primop
     */
    void forceFunction(Value & v, const PosIdx pos, std::string_view errorCtx);
    std::string_view forceString(Value & v, const PosIdx pos, std::string_view errorCtx);
    std::string_view forceString(
        Value & v,
        NixStringContext & context,
        const PosIdx pos,
        std::string_view errorCtx,
        const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings);
    std::string_view forceStringNoCtx(Value & v, const PosIdx pos, std::string_view errorCtx);

    /**
     * Get attribute from an attribute set and throw an error if it doesn't exist.
     */
    const Attr * getAttr(Symbol attrSym, const Bindings * attrSet, std::string_view errorCtx);

    template<typename... Args>
    [[gnu::noinline]]
    void addErrorTrace(Error & e, const Args &... formatArgs) const;
    template<typename... Args>
    [[gnu::noinline]]
    void addErrorTrace(Error & e, const PosIdx pos, const Args &... formatArgs) const;

public:
    /**
     * @return true iff the value `v` denotes a derivation (i.e. a
     * set with attribute `type = "derivation"`).
     *
     * Intentional duplication: expr::helpers::isDerivation(Object &) is
     * semantically equivalent but uses Object interface. This version stays
     * for hot-path callers (eqValues, print.cc, etc.) to avoid Object overhead
     * (symbol interning, heap allocation, GC root).
     */
    bool isDerivation(Value & v);

    std::optional<std::string> tryAttrsToString(
        const PosIdx pos, Value & v, NixStringContext & context, bool coerceMore = false, bool copyToStore = true);

    /**
     * For efficiency reasons, some store paths (as seen by the evaluator) in
     * the storeFS at their content-addressed locations don't get copied to the
     * store eagerly. This saves on needless I/O and possibly IPC if all the
     * evaluator does is just evaluate nix expressions from those locations.
     * This function copies such store objects to the store if they aren't already valid.
     */
    void ensureLazyPathCopied(const StorePath & path);

    /**
     * Ensure that all NixStringContextElem::Opaque context elements get fetched
     * to the store.
     */
    void ensureLazyPathsCopied(const NixStringContext & context);

    /**
     * String coercion.
     *
     * Converts strings, paths and derivations to a
     * string.  If `coerceMore` is set, also converts nulls, integers,
     * booleans and lists to a string.  If `copyToStore` is set,
     * referenced paths are copied to the Nix store as a side effect.
     *
     * `copyToStore=false` does *not* universally mean "no store
     * write": for a path value whose `SourceRoot.kind` is
     * `Copyable` (the typical `fetchTree` / flake-input case), the
     * Copyable arm always materialises the accessor's root via
     * `copyPathToStore` and appends the subpath as text — the
     * rendered `<storePath>/<subpath>` string only names a
     * reachable location once the root has been copied. This is
     * the same walk that `lint-fetch-whole-source-to-store`
     * surfaces. `System`-kinded paths render as the raw absolute
     * canon path with no IO; `Internal`-kinded paths raise an
     * `EvalError`.
     */
    BackedStringView coerceToString(
        const PosIdx pos,
        Value & v,
        NixStringContext & context,
        std::string_view errorCtx,
        bool coerceMore = false,
        bool copyToStore = true,
        bool canonicalizePath = true);

    StorePath copyPathToStore(NixStringContext & context, const RootedPath & path);

    /**
     * Return an accessor whose root is the given store path.
     *
     * First consults `storeFS`'s mount table — used by lazy
     * fetchTree and friends that register an accessor (with
     * fingerprint) for the store path they expose. Falls back
     * to the `Store`'s own per-store-path accessor
     * (`Store::requireStoreObjectAccessor`), which works
     * uniformly across local, relocated, and remote stores.
     *
     * If the fallback accessor lacks a fingerprint, it gets
     * one: `printStorePath(sp)`. The store path string is a
     * content-addressed identity that satisfies the fingerprint
     * contract — equal store paths ⇔ equal NAR by construction.
     * Without this, downstream `fetchToStore` sees an
     * unfingerprinted source and trips
     * `_NIX_TEST_BARF_ON_UNCACHEABLE`.
     *
     * Throws `InvalidPath` if the store path is unreachable via
     * either route — the genuinely undecidable case.
     */
    ref<SourceAccessor> storePathAccessor(const StorePath & sp);

    /**
     * Path coercion.
     *
     * Converts strings, paths and derivations to a
     * path.  The result is guaranteed to be a canonicalised, absolute
     * path.
     *
     * A bare path value (`nPath`) is returned directly via `v.path()`
     * with no store IO. Other shapes — strings, and attrsets coerced
     * via their `outPath` / `__toString` member — route through
     * `coerceToString` with `copyToStore=false`, which for an
     * `outPath` that resolves to a `Copyable`-rooted path triggers
     * the same accessor-root copy described on `coerceToString`. The
     * "no store IO" intuition only holds for the bare-path case.
     */
    SourcePath coerceToPath(const PosIdx pos, Value & v, NixStringContext & context, std::string_view errorCtx);

    /**
     * Like `coerceToPath`, but returns a `RootedPath` so the caller
     * can see the language-level `SourceRootKind` the path lives
     * under. The kind is found inductively by peeling the input:
     *
     * - `nPath`: kind is the path Value's own `SourceRoot`.
     * - `nAttrs` with `__toString`: call it, recurse on the result.
     * - `nAttrs` with `outPath`: if `outPath` is itself path- or
     *   attrset-shaped, recurse on it (this is the lazy-paths
     *   regime where `fetchTree { lazy = true; }.outPath` is an
     *   nPath); if it's a string, fall through to the string arm.
     * - Any string-shaped value (including `outPath` strings, the
     *   documented contract): the string identifies a filesystem
     *   location, so the result is admitted under `rootFSRoot`
     *   (System).
     *
     * Use this at sites that route on the path's `SourceRootKind`
     * (e.g. `realisePath`'s kind-aware symlink resolver). For sites
     * that only want a bare `SourcePath`, `coerceToPath` is fine —
     * it stays on its historical route through `coerceToString` for
     * the non-bare-path cases (notably `builtins.storePath` relies
     * on the Copyable-walk-to-storepath-string rewrap).
     */
    RootedPath coerceToRootedPath(const PosIdx pos, Value & v, NixStringContext & context, std::string_view errorCtx);

    /**
     * Like coerceToPath, but the result must be a store path.
     */
    StorePath coerceToStorePath(const PosIdx pos, Value & v, NixStringContext & context, std::string_view errorCtx);

    /**
     * Part of `coerceToSingleDerivedPath()` without any store IO which is exposed for unit testing only.
     */
    std::pair<SingleDerivedPath, std::string_view> coerceToSingleDerivedPathUnchecked(
        const PosIdx pos,
        Value & v,
        std::string_view errorCtx,
        const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings);

    /**
     * Coerce to `SingleDerivedPath`.
     *
     * Must be a string which is either a literal store path or a
     * "placeholder (see `DownstreamPlaceholder`).
     *
     * Even more importantly, the string context must be exactly one
     * element, which is either a `NixStringContextElem::Opaque` or
     * `NixStringContextElem::Built`. (`NixStringContextEleme::DrvDeep`
     * is not permitted).
     *
     * The string is parsed based on the context --- the context is the
     * source of truth, and ultimately tells us what we want, and then
     * we ensure the string corresponds to it.
     */
    SingleDerivedPath coerceToSingleDerivedPath(const PosIdx pos, Value & v, std::string_view errorCtx);

#if NIX_USE_BOEHMGC
    /** A GC root for the baseEnv reference. */
    const std::shared_ptr<Env *> baseEnvP;
#endif

public:

    /**
     * The base environment, containing the builtin functions and
     * values.
     */
    Env & baseEnv;

    /**
     * The same, but used during parsing to resolve variables.
     */
    const std::shared_ptr<StaticEnv> staticBaseEnv; // !!! should be private

    /**
     * Internal primops not exposed to the user.
     */
    boost::unordered_flat_map<
        std::string,
        Value *,
        StringViewHash,
        std::equal_to<>,
        traceable_allocator<std::pair<const std::string, Value *>>>
        internalPrimOps;

    /**
     * Name and documentation about every constant.
     *
     * Constants from primops are hard to crawl, and their docs will go
     * here too.
     */
    std::vector<std::pair<std::string, Constant>> constantInfos;

private:

    unsigned int baseEnvDispl = 0;

    void createBaseEnv(const EvalSettings & settings);

    Value * addConstant(const std::string & name, Value & v, Constant info);

    void addConstant(const std::string & name, Value * v, Constant info);

    Value * addPrimOp(PrimOp && primOp);

public:

    /**
     * Retrieve a specific builtin, equivalent to evaluating `builtins.${name}`.
     * @param name The attribute name of the builtin to retrieve.
     * @throws EvalError if the builtin does not exist.
     */
    Value & getBuiltin(const std::string & name);

    /**
     * Retrieve the `builtins` attrset, equivalent to evaluating the reference `builtins`.
     * Always returns an attribute set value.
     */
    Value & getBuiltins();

    /**
     * Retrieve the documentation for a value. This will evaluate the value if
     * it is a thunk, and it will partially apply __functor if applicable.
     *
     * @param v The value to get the documentation for.
     */
    std::optional<Doc> getDoc(Value & v);

private:

    inline Value * lookupVar(Env * env, const ExprVar & var, bool noEval);

    friend struct ExprVar;
    friend struct ExprAttrs;
    friend struct ExprLet;

    Expr * parse(
        char * text,
        size_t length,
        Pos::Origin origin,
        const RootedPath & basePath,
        const std::shared_ptr<StaticEnv> & staticEnv);

    ExprAttrs * parseReplBindings(
        char * text,
        size_t length,
        Pos::Origin origin,
        const RootedPath & basePath,
        const std::shared_ptr<StaticEnv> & staticEnv);

    /**
     * Current Nix call stack depth, used with `max-call-depth` setting to throw stack overflow hopefully before we run
     * out of system stack.
     */
    size_t callDepth = 0;

public:

    /**
     * Check that the call depth is within limits, and increment it, until the returned object is destroyed.
     */
    inline CallDepth addCallDepth(const PosIdx pos);

    /**
     * Do a deep equality test between two values.  That is, list
     * elements and attributes are compared recursively.
     *
     * When `ctx` is supplied, cross-type `nPath × nString` (in
     * either order) is taken as path↔string equivalent under the
     * `toString` semantic rather than the language `==`'s strict
     * "different type => false". Recursion into `nList`/`nAttrs`
     * threads the same ctx through, so the relaxation reaches
     * arbitrary nesting depth. Only `PathEquivalentDedup` ever
     * passes one; user-visible `==` / `assert` / `builtins.sort`
     * / plain `genericClosure` keep their strict typing.
     */
    bool eqValues(
        Value & v1,
        Value & v2,
        const PosIdx pos,
        std::string_view errorCtx,
        struct PathEquivalenceContext * ctx = nullptr);

    /**
     * Like `eqValues`, but throws an `AssertionError` if not equal.
     *
     * WARNING:
     * Callers should call `eqValues` first and report if `assertEqValues` behaves
     * incorrectly. (e.g. if it doesn't throw if eqValues returns false or vice versa)
     */
    void assertEqValues(Value & v1, Value & v2, const PosIdx pos, std::string_view errorCtx);

    /**
     * Are these two values equivalent under the path-aware
     * `toString` semantic, i.e. would `toString a == toString b`
     * have held — but computed without ever invoking `toString`
     * on a Copyable path (which would hash the whole tree)?
     *
     * Defined for the four combinations of `nString` and `nPath`.
     * The implementation reduces each operand to a "static
     * string" if possible (nString uses its bytes; nPath with
     * kind System uses its subpath's abspath); a Copyable nPath
     * stays lazy and is matched via a structural check on
     * subpath + `accessorsEquivalent` on the roots. Internal-
     * kinded paths have no defined toString and throw.
     *
     * Backs both `eqValues` for the nPath × nPath case and
     * `builtins.isPathEquivalent` for the cross-type cases.
     * Errors on unsupported argument types (int, attrset, etc.).
     */
    bool toStringEqual(Value & v1, Value & v2, const PosIdx pos, std::string_view errorCtx);

    /**
     * Path × store-path-string equivalence: "is the string what
     * `toString p` would produce?", kind-dispatched on `p`. The
     * Copyable arm parses the string as a store path with a
     * trailing subpath, requires the subpath to match `p.path`,
     * and decides via `srcToStore` lookup (cheap when cached)
     * or a fresh `copyPathToStore` of the accessor root (which
     * trips `lint-fetch-whole-source-to-store` exactly as
     * `toString` would have).
     *
     * Lower-level than `toStringEqual`: callers that already
     * have a path Value and a literal string in hand (e.g. the
     * cross-type dedup scan in `genericClosure`'s pathEquivalent
     * arm) use this directly to skip the reduction step.
     */
    bool pathToStringEqual(const SourcePath & p, SourceRootKind kind, std::string_view s);

    /**
     * Are two Copyable accessors NAR-equivalent — i.e. would
     * `toString` on roots backed by them produce the same store
     * path? Always returns a correct answer; never guesses.
     *
     * Layered probes, cheapest decisive first:
     *   1. Pointer identity (positive).
     *   2. `accessorsKnownInequivalent` cache hit (negative).
     *   3. Fingerprint match (positive).
     *   4. `srcToStore` query-only: both cached → compare
     *      storePaths (decisive both ways).
     *   5. Root-name-set SHA256 mismatch (negative, per-accessor
     *      cache).
     *   6. Hint subpath SHA256 mismatch when `hint` is supplied
     *      (negative, per-(accessor, subpath) cache).
     *   7. `copyPathToStore` and compare storePaths. The lint
     *      trips here by design — same materialisation point
     *      `toString` would have hit. Result lands in
     *      `srcToStore`, so subsequent comparisons of either
     *      accessor are O(1).
     *
     * Worst-case cost across N accessors is O(N) tree walks
     * (each accessor walked at most once), not the O(N²)
     * pairwise walks a per-call NAR-compare would exhibit
     * inside a dedup loop.
     */
    bool
    accessorsEquivalent(ref<SourceAccessor> a, ref<SourceAccessor> b, std::optional<CanonPath> hint = std::nullopt);

    /**
     * Total preorder on path-or-string Values whose equivalence
     * classes match toString-equivalence. The between-class
     * ordering itself is NOT toString lex — it's a cheap layered
     * order chosen so the comparator never has to materialise:
     *
     *   1. Camp partition.
     *      - Camp A: byte-for-byte canonical Copyable `toString`
     *        shape, i.e. `storeDir + "/" + 32-nix32-hash +
     *        "-source"` optionally followed by a canonical
     *        subpath. Copyable nPath is always Camp A (its
     *        would-be toString is canonical by construction).
     *      - Camp B: everything else, including non-canonical
     *        store-shape strings (trailing slash, `/.`, `/..`,
     *        non-`source` name, short hash).
     *      - Camp B < Camp A.
     *   2. Subpath compare.
     *      - Camp A: byte lex on the parsed subpath after
     *        `-source`.
     *      - Camp B: byte lex on the full bytes (System abspath /
     *        string).
     *   3. Source root equivalence class. Fires only when a
     *      `ctx` is supplied and step 2 returned equal within
     *      Camp A. Each side resolves to a class id via
     *      `ctx->classOfAccessor` (Copyable nPath uses its
     *      accessor directly; nString / System-in-store parse
     *      the bytes to a `StorePath` and bridge through
     *      `storeFS->getMount` to obtain an accessor). Same id
     *      → Equivalent; different ids → ordered by id number
     *      (arbitrary but stable within one ctx). Throws if a
     *      bridged store path isn't mounted in `storeFS`.
     *
     * Throws on Internal-kinded paths and on types other than
     * `nPath` / `nString`.
     */
    std::strong_ordering compareForToStringEquivalence(
        Value & a, Value & b, PosIdx pos, std::string_view errorCtx, struct PathEquivalenceContext * ctx = nullptr);

    /**
     * Invoke `CompareValues`'s comparator. Returns true iff `v1 < v2`
     * under the same semantics as `builtins.lessThan` (no ctx) or
     * `PathEquivalentDedup`'s otherMap (ctx provided). Exposed for
     * unit testing the comparator's strict-weak-order properties
     * (transitivity, antisymmetry, irreflexivity); the comparator
     * itself stays in `primops.cc`.
     */
    bool compareValues(
        Value & v1, Value & v2, PosIdx pos, std::string_view errorCtx, PathEquivalenceContext * ctx = nullptr);

    /**
     * Order two path Values under the language-level `<` semantic
     * (`toString a < toString b`), committing to a definitive
     * answer — no `Expensive` state. Cheap branches first;
     * otherwise materialises via `coerceToString` and string-
     * compares.
     *
     * Internal-kinded operands are rejected upfront with the
     * same diagnostic `coerceToString` would raise, since their
     * `toString` is undefined. This lets the cheap branches
     * speak unconditionally for both operands.
     *
     * Cheap discriminations:
     *   - Same root prefix (same accessor or matching fingerprint
     *     at root) → subpath compare drives the order, since the
     *     prefix is shared. Covers the same-path case (returns
     *     Equal) by construction.
     *   - Cross-kind System × Copyable: if the System abspath
     *     lex-precedes `storeDir + "/"`, System < Copyable; if
     *     lex-after and not starting with the prefix, System >
     *     Copyable; if it does start with the prefix (System is
     *     in store) we can't decide without knowing the
     *     Copyable's hash, so fall through.
     *
     * The string-context produced by coercion is discarded;
     * comparison is about the resulting string, not its
     * provenance. Materialisation on Copyable fires the
     * `lint-fetch-whole-source-to-store` knob, as `<` is
     * semantically a `toString` call.
     *
     * Counterpart for equality predicates is `eqValues` /
     * `toStringEqual` (which return `bool`); they share the
     * cheap-discrimination strategy but have a different result
     * space.
     *
     * When `ctx` is supplied (only `PathEquivalentDedup`'s
     * comparator does so), same-classId is taken as a cheap
     * equivalence witness — two Copyable accessors that
     * classify into the same class return `equal` without
     * materialising. Different classes get an arbitrary but
     * stable order via the classId number, so the result remains
     * a total preorder usable as a sorted-container key without
     * firing the materialise-fallback (and the lint with it).
     */
    std::strong_ordering comparePathsForOrdering(
        Value & v1, Value & v2, PosIdx pos, std::string_view errorCtx, struct PathEquivalenceContext * ctx = nullptr);

    bool isFunctor(const Value & fun) const;

    void callFunction(Value & fun, std::span<Value *> args, Value & vRes, const PosIdx pos);

    void callFunction(Value & fun, Value & arg, Value & vRes, const PosIdx pos)
    {
        Value * args[] = {&arg};
        callFunction(fun, args, vRes, pos);
    }

    /**
     * Automatically call a function for which each argument has a
     * default value or has a binding in the `args` map.
     */
    void autoCallFunction(const Bindings & args, Value & fun, Value & res);

    BindingsBuilder buildBindings(size_t capacity)
    {
        return mem.buildBindings(symbols, capacity);
    }

    ListBuilder buildList(size_t size)
    {
        return mem.buildList(size);
    }

    /**
     * Return a boolean `Value *` without allocating.
     */
    Value * getBool(bool b);

    void mkThunk_(Value & v, Expr * expr);
    void mkPos(Value & v, PosIdx pos);

    /**
     * Create a string representing a store path.
     *
     * The string is the printed store path with a context containing a
     * single `NixStringContextElem::Opaque` element of that store path.
     */
    void mkStorePathString(const StorePath & storePath, Value & v);

    /**
     * Create a string representing a `SingleDerivedPath::Built`.
     *
     * The string is the printed store path with a context containing a
     * single `NixStringContextElem::Built` element of the drv path and
     * output name.
     *
     * @param value Value we are settings
     *
     * @param b the drv whose output we are making a string for, and the
     * output
     *
     * @param optStaticOutputPath Optional output path for that string.
     * Must be passed if and only if output store object is
     * input-addressed or fixed output. Will be printed to form string
     * if passed, otherwise a placeholder will be used (see
     * `DownstreamPlaceholder`).
     *
     * @param xpSettings Stop-gap to avoid globals during unit tests.
     */
    void mkOutputString(
        Value & value,
        const SingleDerivedPath::Built & b,
        std::optional<StorePath> optStaticOutputPath,
        const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings);

    /**
     * Create a string representing a `SingleDerivedPath`.
     *
     * A combination of `mkStorePathString` and `mkOutputString`.
     */
    void mkSingleDerivedPathString(const SingleDerivedPath & p, Value & v);

    /**
     * @brief Concatenate values with an n-ary version of the `++` operator.
     */
    void concatLists(Value & v, std::span<Value * const> lists, const PosIdx pos, std::string_view errorCtx);

    /**
     * Print statistics, if enabled.
     *
     * Performs a full memory GC before printing the statistics, so that the
     * GC statistics are more accurate.
     */
    void maybePrintStats();

    /**
     * Print statistics, unconditionally, cheaply, without performing a GC first.
     */
    void printStatistics();

    /**
     * Perform a full memory garbage collection - not incremental.
     *
     * @return true if Nix was built with GC and a GC was performed, false if not.
     *              The return value is currently not thread safe - just the return value.
     */
    bool fullGC();

    /**
     * Realise the given context
     * @param[in] context the context to realise
     * @param[out] maybePaths if not nullptr, all built or referenced store paths will be added to this set
     * @return a mapping from the placeholders used to construct the associated value to their final store path.
     */
    [[nodiscard]] StringMap
    realiseContext(const NixStringContext & context, StorePathSet * maybePaths = nullptr, bool isIFD = true);

    /**
     * Coerce `v` to a path and realise it, i.e. build anything in the value's string context using `realiseContext()`.
     * @param copyLazyPaths When encountering a lazy path (i.e. a string with Opaque context that's also "mounted" on
     * the storeFS), fetch the store path to the store.
     */
    SourcePath realisePath(
        const PosIdx pos,
        Value & v,
        std::optional<SymlinkResolution> resolveSymlinks = SymlinkResolution::Full,
        CopyLazyPaths copyLazyPaths = CopyLazyPaths::PreserveLazy);

    /**
     * Like `realisePath`, but returns a `RootedPath` so callers that
     * route on `SourceRootKind` (e.g. `import`'s downstream
     * `evalFile`, `readDir`'s per-entry mkPath) get the kind without
     * re-peeling. The kind is inherited from `coerceToRootedPath` —
     * the same inductive peel `realisePath` itself uses.
     */
    RootedPath realiseRootedPath(
        const PosIdx pos,
        Value & v,
        std::optional<SymlinkResolution> resolveSymlinks = SymlinkResolution::Full,
        CopyLazyPaths copyLazyPaths = CopyLazyPaths::PreserveLazy);

    /**
     * Realise the given string with context, and return the string with outputs instead of downstream output
     * placeholders.
     * @param[in] str the string to realise
     * @param[out] paths all referenced store paths will be added to this set
     * @return the realised string
     * @throw EvalError if the value is not a string, path or derivation (see `coerceToString`)
     */
    std::string
    realiseString(Value & str, StorePathSet * storePathsOutMaybe, bool isIFD = true, const PosIdx pos = noPos);

    /* Call the binary path filter predicate used builtins.path etc. */
    bool callPathFilter(Value * filterFun, const SourcePath & path, PosIdx pos);

    DocComment getDocCommentForPos(PosIdx pos);

    /**
     * Render a `SingleDerivedPath` as the string that a Nix string value
     * containing that path would evaluate to.
     */
    std::string mkSingleDerivedPathStringRaw(const SingleDerivedPath & p);

private:

    /**
     * Like `mkOutputString` but just creates a raw string, not an
     * string Value, which would also have a string context.
     */
    std::string mkOutputStringRaw(
        const SingleDerivedPath::Built & b,
        std::optional<StorePath> optStaticOutputPath,
        const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings);

    Counter nrLookups;
    Counter nrAvoided;
    Counter nrOpUpdates;
    Counter nrOpUpdateValuesCopied;
    Counter nrListConcats;
    Counter nrPrimOpCalls;
    Counter nrFunctionCalls;

    bool countCalls;

    typedef boost::unordered_flat_map<std::string, size_t, StringViewHash, std::equal_to<>> PrimOpCalls;
    PrimOpCalls primOpCalls;

    typedef boost::unordered_flat_map<ExprLambda *, size_t> FunctionCalls;
    FunctionCalls functionCalls;

    /** Evaluation/call profiler. */
    MultiEvalProfiler profiler;

    void incrFunctionCall(ExprLambda * fun);

    typedef boost::unordered_flat_map<PosIdx, size_t, std::hash<PosIdx>> AttrSelects;
    AttrSelects attrSelects;

    friend struct ExprOpUpdate;
    friend struct ExprOpConcatLists;
    friend struct ExprVar;
    friend struct ExprString;
    friend struct ExprInt;
    friend struct ExprFloat;
    friend struct ExprPath;
    friend struct ExprSelect;
    friend void prim_getAttr(EvalState & state, const PosIdx pos, Value ** args, Value & v);
    friend void prim_match(EvalState & state, const PosIdx pos, Value ** args, Value & v);
    friend void prim_split(EvalState & state, const PosIdx pos, Value ** args, Value & v);

    friend struct Value;
    friend class ListBuilder;
};

struct DebugTraceStacker
{
    DebugTraceStacker(EvalState & evalState, DebugTrace t);

    ~DebugTraceStacker()
    {
        evalState.debugTraces.pop_front();
    }

    EvalState & evalState;
    DebugTrace trace;
};

/**
 * @return A string representing the type of the value `v`.
 *
 * @param withArticle Whether to begin with an english article, e.g. "an
 * integer" vs "integer".
 */
std::string_view showType(ValueType type, bool withArticle = true);
std::string showType(const Value & v);

/**
 * If `path` refers to a directory, then append "/default.nix".
 *
 * @param addDefaultNix Whether to append "/default.nix" after resolving symlinks.
 */
SourcePath resolveExprPath(SourcePath path, bool addDefaultNix = true);

/**
 * Kind-aware variant of `resolveExprPath`: the ancestor symlink
 * walk routes through the kind-aware `resolveSymlinks` wrapper
 * (see `<nix/expr/source-root.hh>`). For Copyable accessors a
 * symlink whose target escapes the accessor root raises
 * `AccessorBoundaryEscape`; System keeps the historical lenient
 * walk; Internal-rooted paths bypass the wrapper (the wrapper
 * refuses Internal by design) and fall back to the bare resolver.
 *
 * Prefer this overload at sites that have a `RootedPath` in hand —
 * the import / scopedImport choke point in particular, where it
 * actually catches escapes that the bare overload silently clamps.
 */
SourcePath resolveExprPath(const RootedPath & path, bool addDefaultNix = true);

/**
 * Whether a URI is allowed, assuming restrictEval is enabled
 */
bool isAllowedURI(std::string_view uri, const Strings & allowedPaths);

/**
 * Per-invocation equivalence-class classifier for
 * `builtins.genericClosure { pathEquivalent = true; … }`'s
 * comparator. Same classId means "the two roots' toStrings
 * would agree" — established cheaply via accessor identity,
 * fingerprint match, srcToStore lookup, or one of the
 * inequality probes in `EvalState::accessorsEquivalent`, with
 * a storePath compute as last resort (paid at most once per
 * accessor across the evaluation).
 *
 * The framework: a total preorder over `Value` whose
 * equivalence classes are the dedup classes. Class IDs are the
 * cheap witness for path equivalence; the ctx-aware comparator
 * uses them to discriminate without materialising.
 *
 * Only `PathEquivalentDedup`'s comparator passes a ctx;
 * user-visible Nix comparisons (`==`, `assert`, `builtins.sort`,
 * plain `genericClosure`) keep their current strict typing.
 */
struct PathEquivalenceContext
{
    EvalState & state;

    /** Cached equivalence-class id per accessor pointer. */
    std::unordered_map<const SourceAccessor *, size_t> classOf;
    /** Representative accessors per Copyable class, scanned via
        `accessorsEquivalent` when classifying a new accessor. */
    std::vector<ref<SourceAccessor>> copyableReps;
    /** All System accessors share one id (their toString does
        not depend on the accessor — see `SourceRootKind::System`'s
        singleton assumption). Assigned on first encounter. */
    std::optional<size_t> systemClassId;
    /** Monotonic id allocator. */
    size_t nextClassId = 0;

    /** Look up or assign the equivalence-class id for `root`.
        Two roots share an id iff their toStrings would agree at
        the root: System → singleton id, Copyable → NAR-equivalence
        class via `accessorsEquivalent` against existing reps,
        Internal → identity-class per pointer (toString is
        undefined for Internal, so the only well-defined relation
        is pointer identity). */
    size_t classOfAccessor(const SourceRoot & root, std::optional<CanonPath> hint = std::nullopt);

    /** Same lookup as above without requiring a `SourceRoot`
        wrapper. Used by `compareForToStringEquivalence`'s step 3
        bridge, where the accessor comes from
        `storeFS->getMount` and wrapping it in a fresh
        `SourceRoot` per comparison would be a per-call
        allocation. */
    size_t
    classOfAccessor(ref<SourceAccessor> accessor, SourceRootKind kind, std::optional<CanonPath> hint = std::nullopt);
};

} // namespace nix

#include "nix/expr/eval-inline.hh"
