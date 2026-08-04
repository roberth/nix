#pragma once
/**
 * @file
 * OuterObject — Object backed by an outer query callback.
 *
 * A value from the outer evaluator, accessed by the local
 * (inner) evaluator through outer queries. Each Object method issues
 * a query through the provided callback and interprets the response.
 */

#include "nix/expr/arg-cell.hh"
#include "nix/expr/observation-set.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/source-root.hh"
#include "nix/expr/trace-ids.hh"
#include "nix/expr/trace-types.hh"

#include <functional>
#include <optional>
#include <string>

namespace nix {

/**
 * Response from an outer query: the result plus an optional child
 * Object for queries that produce child Objects (getAttr, getListElem).
 * The child is the outer's Object at the queried position, which the
 * caller wraps in a new OuterObject.
 */
struct OuterQueryResult
{
    trace::ResultVariant result;
    std::shared_ptr<Object> child; // outer's child Object, if applicable
};

/**
 * Callback type for issuing outer queries. Takes the outer Object
 * to query, the query itself, the caller's producer Selector
 * (identifies the caller for logging), and the caller's argCell
 * (fact-attribution target: observations attribute to this cell,
 * NOT the wrapped `outerObj`'s cell — that one is typically null
 * because outerObj is a non-proxy inner Object). Passing the
 * outer Object directly (rather than an id) reflects that OuterObject
 * wraps a specific Object from a different Interpreter.
 */
using OuterQueryFn = std::function<OuterQueryResult(
    std::shared_ptr<Object> outerObj,
    ref<const trace::Selector> query,
    ref<const trace::Selector> producer,
    std::shared_ptr<ArgCell> callerCell)>;

/**
 * Result of an OuterApplyFn call. `applyResult` is the outer's raw
 * apply-result Object (wrapped by the caller into an OuterObject with
 * `producerFn` as its producer callable). `producerFn` returns the
 * current SelectorNode identifying the apply-result — for callback
 * applies it snapshots the callback firing's runningObsSet into a
 * `SelectorCallbackApply` on demand, so probes at different moments
 * produce distinct compositional Selectors.
 */
struct OuterApplyResult
{
    std::shared_ptr<Object> applyResult;
    std::function<ref<const trace::Selector>()> producerFn;
};

/**
 * Callback type for outer function application. Takes the outer fn
 * Object, the calling OuterObject's producer Selector (identifies
 * the fn for the SelectorApply payload — the outer Object itself
 * typically has no producer, so the wrapping OuterObject provides
 * it), the argument Object, and the caller's argCell (parent of
 * the apply's cell). Returns the outer's raw apply-result Object
 * plus a producer callable for the wrapping OuterObject.
 *
 * Cell-kind rule (#261): the implementation creates its own apply
 * cell parented to `callerScope`. Callback-firing implementations
 * create a `RecordingCallbackArgCell` (with `fnProducer->cachedHash` as
 * `initialFnHex`); pure outer applies with no inner writer create
 * a `RegularArgCell`. Lifting cell creation into the callee is what
 * lets the concrete cell type be picked without post-construction
 * mutation.
 */
using OuterApplyFn = std::function<OuterApplyResult(
    std::shared_ptr<Object> fnObj,
    ref<const trace::Selector> fnProducer,
    std::shared_ptr<Object> argObj,
    std::shared_ptr<ArgCell> callerScope)>;

/**
 * Object implementation backed by outer queries to the outer evaluator.
 * Each method composes a Query, issues it via the callback, and
 * interprets the Result.
 */
class OuterObject : public Object
{
    /** Producer Selector — a callable that returns the current
        SelectorNode whose content hash IS this OuterObject's
        identity. Callable form (not a stored value) because a
        producer may reference live state elsewhere (e.g. a
        `SelectorApply{fn=<fnObj's current hex>}` must recompute the
        fn hex on demand, not bake a snapshot). Static producers wrap
        a literal via `[v]{ return v; }`. */
    std::function<ref<const trace::Selector>()> producer;
    /* The Object from the outer Interpreter this proxy wraps.
       Methods on OuterObject dispatch through this reference: the
       inner side asks OuterObject (via Object interface), OuterObject
       dispatches the equivalent method on `outerObj` (executing in the
       outer's Interpreter), and hands the result back to the inner. */
    std::shared_ptr<Object> outerObj;
    OuterQueryFn queryFn;   ///< Callback to issue outer queries
    OuterApplyFn applyFn;   ///< Callback for function application (may be null)
    /* lazy-paths: stable SourceRoot for paths returned by `getPath`.
       Held as a member so the SourceRoot outlives the Value the
       outer evaluator constructs from the RootedPath (Value stores a
       raw SourceRoot pointer). Resolved at construction by the
       resolver from the outer EvalState's `rootFSRoot`. */
    ref<SourceRoot> outerRootFSRoot;

    /* Nearest enclosing apply's cell. Navigation children carry the
       same cell as their parent; apply-result proxies open a fresh
       cell rooted at the fn's cell. Ancestor chain reached via the
       cell's own `parent` field. */
    std::shared_ptr<ArgCell> argCell;

    /* SelectorPool for interning child Selectors constructed here
       (maybeGetAttr → SelectorGetAttr, etc.). Lives on the shared
       TracingDecisionGraph. Required — non-tracing runs skip the
       Tracing{Replay,}Evaluator wrappers and never construct
       OuterObjects, so a proxy without a session pool can't arise. */
    trace::SelectorPool & selectorPool;

    /* Memoized WHNF observation. First call to any of getType / getInt /
       getString / etc. fires `whnf()`, which issues ONE observation
       through `queryFn` keyed on this proxy's `producer`. Subsequent
       calls decode the cached result without re-querying. */
    std::optional<trace::ResultWHNF> cachedWHNF;
    trace::ResultWHNF & whnf();

public:
    OuterObject(std::function<ref<const trace::Selector>()> producer, std::shared_ptr<Object> outerObj, OuterQueryFn queryFn, ref<SourceRoot> outerRootFSRoot, trace::SelectorPool & selectorPool, std::shared_ptr<ArgCell> argCell, OuterApplyFn applyFn = {});

    std::shared_ptr<ArgCell> getProxyArgCell() const override { return argCell; }

    std::shared_ptr<Object> maybeGetAttr(const std::string & name) override;
    std::vector<std::string> getAttrNames() override;
    std::string getStringIgnoreContext() override;
    std::string getStringWithoutContext() override;
    std::pair<std::string, NixStringContext> getStringWithContext() override;
    RootedPath getPath() override;
    bool getBool(std::string_view errorCtx = "") override;
    NixInt getInt(std::string_view errorCtx = "") override;
    NixFloat getFloat(std::string_view errorCtx = "") override;
    size_t getListSize() override;
    std::shared_ptr<Object> getListElem(size_t index) override;
    ObjectType getTypeLazy() override;
    ObjectType getType() override;
    RootValue defeatCache() override;
    RootValue toValueOrProxy(EvalState & state, std::shared_ptr<OuterResolver> resolver) override;
    /** Outer function crosses the boundary via an `<outer-fn>`
        primop that dispatches through `queryApply`. */
    Value * materialiseAsFunctionValue(
        EvalState & state,
        std::shared_ptr<OuterResolver> resolver,
        std::shared_ptr<Evaluator> innerEvaluator) override;
    std::optional<FunctionInfo> getFunctionInfo() override;
    PosIdx getPos() override;
    std::optional<std::vector<std::string>> getAttrPath() override;

    /**
     * Issue a SelectorApply. The resolver registers the arg and creates
     * the lazy application. Returns an OuterObject wrapping the result.
     */
    std::shared_ptr<Object> queryApply(std::shared_ptr<Object> argObj) override;

    /** State-hash hex = content hash of the stored producer Selector.
        Under Q-space identity every OuterObject has a stable identity
        derived from its producer — no separate `producingQHex`
        override needed. */
    std::optional<std::string> getSelectorHashHex() const override
    {
        return producer()->cachedHash.toHex();
    }

    std::optional<ref<const trace::Selector>> getSelector() const override
    {
        return producer();
    }
};

} // namespace nix
