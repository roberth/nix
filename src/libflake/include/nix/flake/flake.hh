#pragma once
///@file

#include "nix/util/fun.hh"
#include "nix/util/types.hh"
#include "nix/flake/flakeref.hh"
#include "nix/flake/lockfile.hh"
#include "nix/expr/value.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/store/path.hh"

namespace nix {

class EvalState;

namespace flake {

/**
 * Materialisation info for one node's source tree. Captures the
 * three pieces of data the lockfile machinery needs about *where*
 * each input lives:
 *
 * - `storePath`: the store object the source tree was mounted at.
 * - `accessor`: thunk producing the underlying SourceAccessor for
 *   that storePath; fired lazily by readers that need to walk the
 *   tree (e.g. `import (input.outPath + "/file")`).
 * - `subdir`: the in-storePath subpath where this node's flake.nix
 *   lives (empty for inputs that *are* the storePath's root). For
 *   relative-path inputs that share their parent's store object,
 *   this includes the parent-subdir prefix so the override `dir`
 *   attribute is unambiguous against `storePath` regardless of how
 *   the SourcePath was constructed.
 */
struct NodeLocation
{
    StorePath storePath;
    fun<ref<SourceAccessor>()> accessor;
    std::string subdir;
};

struct Settings;

struct FlakeInput;

typedef std::map<FlakeId, FlakeInput> FlakeInputs;

/**
 * FlakeInput is the 'Flake'-level parsed form of the "input" entries
 * in the flake file.
 *
 * A FlakeInput is normally constructed by the 'parseFlakeInput'
 * function which parses the input specification in the '.flake' file
 * to create a 'FlakeRef' (a fetcher, the fetcher-specific
 * representation of the input specification, and possibly the fetched
 * local store path result) and then creating this FlakeInput to hold
 * that FlakeRef, along with anything that might override that
 * FlakeRef (like command-line overrides or "follows" specifications).
 *
 * A FlakeInput is also sometimes constructed directly from a FlakeRef
 * instead of starting at the flake-file input specification
 * (e.g. overrides, follows, and implicit inputs).
 *
 * A FlakeInput will usually have one of either "ref" or "follows"
 * set.  If not otherwise specified, a "ref" will be generated to a
 * 'type="indirect"' flake, which is treated as simply the name of a
 * flake to be resolved in the registry.
 */

struct FlakeInput
{
    std::optional<FlakeRef> ref;
    /**
     * true = process flake to get outputs
     *
     * false = (fetched) static source path
     */
    bool isFlake = true;
    std::optional<InputAttrPath> follows;
    FlakeInputs overrides;
};

struct ConfigFile
{
    using ConfigValue = std::variant<std::string, int64_t, Explicit<bool>, std::vector<std::string>>;

    std::map<std::string, ConfigValue> settings;

    void apply(const Settings & settings);
};

/**
 * A flake in context
 */
struct Flake
{
    /**
     * The original flake specification (by the user)
     */
    FlakeRef originalRef;

    /**
     * registry references and caching resolved to the specific underlying flake
     */
    FlakeRef resolvedRef;

    /**
     * the specific local store result of invoking the fetcher
     */
    FlakeRef lockedRef;

    /**
     * The path of `flake.nix`.
     */
    SourcePath path;

    /**
     * Materialisation info for the flake's source tree. Set by
     * `getFlake` (after mounting via the input cache) and by the
     * relative-input branch of `lockFlake`'s `getInputFlake` (which
     * shares its parent's tree). Carries through to `nodePaths` so
     * `callFlake`'s sourceInfo rendering can use it directly without
     * re-deriving anything.
     */
    std::optional<NodeLocation> nodeLocation;

    /**
     * Pretend that `lockedRef` is dirty.
     */
    bool forceDirty = false;

    std::optional<std::string> description;

    FlakeInputs inputs;

    /**
     * Attributes to be retroactively applied to the `self` input
     * (such as `submodules = true`).
     */
    fetchers::Attrs selfAttrs;

    /**
     * 'nixConfig' attribute
     */
    ConfigFile config;

    ~Flake();

    SourcePath lockFilePath()
    {
        return path.parent() / "flake.lock";
    }
};

Flake getFlake(EvalState & state, const FlakeRef & flakeRef, fetchers::UseRegistries useRegistries);

/**
 * Fingerprint of a locked flake; used as a cache key.
 */
typedef Hash Fingerprint;

struct LockedFlake
{
    Flake flake;
    LockFile lockFile;

    /**
     * Source tree materialisation info for nodes that have been
     * fetched in `lockFlake()`; in particular, the root node and the
     * overridden inputs. `callFlake` reads from this when rendering
     * each input's `sourceInfo` and `dir` override slots.
     */
    std::map<ref<Node>, NodeLocation> nodePaths;

    std::optional<Fingerprint> getFingerprint(Store & store, const fetchers::Settings & fetchSettings) const;
};

struct LockFlags
{
    /**
     * Whether to ignore the existing lock file, creating a new one
     * from scratch.
     */
    bool recreateLockFile = false;

    /**
     * Whether to update the lock file at all. If set to false, if any
     * change to the lock file is needed (e.g. when an input has been
     * added to flake.nix), you get a fatal error.
     */
    bool updateLockFile = true;

    /**
     * Whether to write the lock file to disk. If set to true, if the
     * any changes to the lock file are needed and the flake is not
     * writable (i.e. is not a local Git working tree or similar), you
     * get a fatal error. If set to false, Nix will use the modified
     * lock file in memory only, without writing it to disk.
     */
    bool writeLockFile = true;

    /**
     * Throw an exception when the flake has an unlocked input.
     */
    bool failOnUnlocked = false;

    /**
     * Whether to use the registries to lookup indirect flake
     * references like 'nixpkgs'.
     */
    std::optional<bool> useRegistries = std::nullopt;

    /**
     * Whether to apply flake's nixConfig attribute to the configuration
     */

    bool applyNixConfig = false;

    /**
     * Whether unlocked flake references (i.e. those without a Git
     * revision or similar) without a corresponding lock are
     * allowed. Unlocked flake references with a lock are always
     * allowed.
     */
    bool allowUnlocked = true;

    /**
     * Whether to commit changes to flake.lock.
     */
    bool commitLockFile = false;

    /**
     * The path to a lock file to read instead of the `flake.lock` file in the top-level flake
     */
    std::optional<SourcePath> referenceLockFilePath;

    /**
     * The path to a lock file to write to instead of the `flake.lock` file in the top-level flake
     */
    std::optional<std::filesystem::path> outputLockFilePath;

    /**
     * Flake inputs to be overridden.
     */
    std::map<NonEmptyInputAttrPath, FlakeRef> inputOverrides;

    /**
     * Flake inputs to be updated. This means that any existing lock
     * for those inputs will be ignored.
     */
    std::set<NonEmptyInputAttrPath> inputUpdates;
};

/*
 * Compute an in-memory lock file for the specified top-level flake, and optionally write it to file, if the flake is
 * writable.
 */
LockedFlake
lockFlake(const Settings & settings, EvalState & state, const FlakeRef & flakeRef, const LockFlags & lockFlags);

/*
 * @pre `flakeDir.path` must parse as a store-rendered path
 * (`/nix/store/<hash>-<name>[/<subpath>]`). The implementation derives
 * the flake's `nodeLocation` by splitting at the store-path boundary;
 * a `flakeDir` that doesn't parse this way raises a generic
 * `BadStorePath`. Callers in practice satisfy this — `getFlake` on
 * `self.sourceInfo.outPath`-shaped paths, etc. — but the precondition
 * is real.
 */
LockedFlake
lockFlake(const Settings & settings, EvalState & state, const SourcePath & flakeDir, const LockFlags & lockFlags);

void callFlake(EvalState & state, const LockedFlake & lockedFlake, Value & v);

/**
 * Open an evaluation cache for a flake.
 */
ref<eval_cache::EvalCache> openEvalCache(EvalState & state, ref<const LockedFlake> lockedFlake);

} // namespace flake

/**
 * An internal builtin similar to `fetchTree`, except that it
 * always treats the input as final (i.e. no attributes can be
 * added/removed/changed).
 */
void prim_fetchFinalTree(EvalState & state, const PosIdx pos, Value ** args, Value & v);

} // namespace nix
