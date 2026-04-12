#pragma once
///@file

#include "nix/cmd/installables.hh"
#include "nix/flake/flake.hh"

namespace nix {

struct PackageInfo;
struct SourceExprCommand;

namespace eval_cache {
class EvalCache;
class AttrCursor;
} // namespace eval_cache

class Object;
class Evaluator;

struct App
{
    std::vector<DerivedPath> context;
    std::filesystem::path program;
    // FIXME: add args, sandbox settings, metadata, ...
};

struct UnresolvedApp
{
    App unresolved;
    std::vector<BuiltPathWithResult> build(ref<Store> evalStore, ref<Store> store);
    App resolve(ref<Store> evalStore, ref<Store> store);
};

/**
 * Extra info about a \ref DerivedPath "derived path" that ultimately
 * come from a Nix language value.
 *
 * Invariant: every ExtraPathInfo gotten from an InstallableValue should
 * be possible to downcast to an ExtraPathInfoValue.
 */
struct ExtraPathInfoValue : ExtraPathInfo
{
    /**
     * Extra struct to get around C++ designated initializer limitations
     */
    struct Value
    {
        /**
         * An optional priority for use with "build envs". See Package
         */
        std::optional<NixInt::Inner> priority;

        /**
         * The attribute path associated with this value. The idea is
         * that an installable referring to a value typically refers to
         * a larger value, from which we project a smaller value out
         * with this.
         */
        std::string attrPath;

        /**
         * \todo merge with DerivedPath's 'outputs' field?
         */
        ExtendedOutputsSpec extendedOutputsSpec;
    };

    Value value;

    ExtraPathInfoValue(Value && v)
        : value(std::move(v))
    {
    }

    virtual ~ExtraPathInfoValue() = default;
};

/**
 * An Installable which corresponds a Nix language value, in addition to
 * a collection of \ref DerivedPath "derived paths".
 */
struct InstallableValue : Installable
{
    ref<EvalState> state;

    /**
     * The evaluator to use for this installable.
     * Uses Interpreter for attr-path based evaluation, CoarseEvalCache for flake-based evaluation.
     */
    ref<Evaluator> evaluator;

    InstallableValue(ref<EvalState> state, ref<Evaluator> evaluator)
        : state(state)
        , evaluator(evaluator)
    {
    }

    virtual ~InstallableValue() {}

    virtual std::pair<Value *, PosIdx> toValue(EvalState & state) = 0;

    /**
     * Like toValue, but returns a lazy thunk that preserves caching.
     * The returned Value is an ExprFromObject thunk — forcing it
     * evaluates through the Object interface (cache-aware) rather
     * than defeating the cache.
     *
     * Use this for commands that don't need to inspect the Value
     * structure directly (e.g. nix eval, nix build). Use toValue()
     * for commands that need the real Value (e.g. nix edit).
     */
    virtual std::pair<Value *, PosIdx> toValueCached(EvalState & state);

    /**
     * @deprecated Use Evaluator and Object instead.
     * Get a cursor to each value this Installable could refer to.
     * However if none exists, throw exception instead of returning
     * empty vector.
     */
    virtual std::vector<ref<eval_cache::AttrCursor>> getCursors(EvalState & state);

    /**
     * @deprecated Use Evaluator and Object instead.
     * Get the first and most preferred cursor this Installable could
     * refer to, or throw an exception if none exists.
     */
    virtual ref<eval_cache::AttrCursor> getCursor(EvalState & state);

    /**
     * Retrieve the root value (e.g. wired-up flake).
     */
    virtual ref<Object> getRootObject() = 0;

    UnresolvedApp toApp(EvalState & state);

    static InstallableValue & require(Installable & installable);
    static ref<InstallableValue> require(ref<Installable> installable);

protected:

    /**
     * Handles either a plain path, or a string with a single string
     * context elem in the right format. The latter case is handled by
     * `helpers::coerceToSingleDerivedPath()`; see it for details.
     *
     * @param obj Object that is hopefully a string or path per the above.
     *
     * @param errorCtx Arbitrary message for use in potential error message when something is wrong with `obj`.
     *
     * @result A derived path (with empty info, for now) if the value
     * matched the above criteria.
     */
    std::optional<DerivedPathWithInfo> trySinglePathToDerivedPaths(Object & obj, std::string_view errorCtx);
};

} // namespace nix
