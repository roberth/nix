/**
 * Tests for forceDerivation regeneration capability.
 *
 * When a derivation path has been garbage-collected, forceDerivation should
 * regenerate it. This happens via getStringWithContext() which checks if
 * cached paths are valid and calls forceValue() to re-evaluate if needed.
 */
#include <gtest/gtest.h>
#include <filesystem>
#include <memory>

#include "nix/expr/coarse-eval-cache.hh"
#include "nix/expr/interpreter.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/expr/evaluation-helpers.hh"
#include "nix/expr/search-path.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/store/store-api.hh"
#include "nix/store/store-open.hh"
#include "nix/store/dummy-store-impl.hh"
#include "nix/store/globals.hh"

namespace nix::expr::helpers {

/**
 * Holds EvalState and its required settings with proper lifetime management.
 */
struct EvaluatorContext
{
    bool readOnlyMode = false;
    fetchers::Settings fetchSettings{};
    EvalSettings evalSettings{readOnlyMode};
    ref<EvalState> state;
    ref<Interpreter> interpreter;
    ref<CoarseEvalCache> evaluator;

    EvaluatorContext(ref<Store> store)
        : state(make_ref<EvalState>(LookupPath{}, store, fetchSettings, evalSettings, nullptr))
        , interpreter(make_ref<Interpreter>(state))
        , evaluator(make_ref<CoarseEvalCache>(interpreter))
    {
        evalSettings.nixPath = {};
    }
};

/**
 * Test fixture for forceDerivation regeneration tests.
 */
class ForceDerivationRegenTest : public ::testing::Test
{
protected:
    // Store - shared across Evaluators (like in real nix usage)
    ref<Store> store;

    // Temporary directory for persistent eval cache
    std::filesystem::path cacheDir;

    static void SetUpTestSuite()
    {
        initLibStore(false);
        initGC();
    }

    ForceDerivationRegenTest()
        : store(openStore("dummy://?read-only=false"))
        , cacheDir(std::filesystem::temp_directory_path() / ("nix-test-cache-" + std::to_string(getpid())))
    {
        std::filesystem::create_directories(cacheDir);
    }

    ~ForceDerivationRegenTest()
    {
        std::filesystem::remove_all(cacheDir);
    }

    /**
     * Create a fresh evaluator context with proper lifetime management.
     * Returns unique_ptr because EvalSettings holds a reference to readOnlyMode.
     */
    std::unique_ptr<EvaluatorContext> createEvaluator()
    {
        return std::make_unique<EvaluatorContext>(store);
    }

    /**
     * Create an EvalCache from an expression with persistent storage.
     */
    ref<eval_cache::EvalCache> createEvalCache(EvalState & state, const std::string & expr)
    {
        return make_ref<eval_cache::EvalCache>(cacheDir / "eval-cache.sqlite", state, [&state, expr]() {
            auto e = state.parseExprFromString(expr, state.rootPath(CanonPath::root));
            auto v = state.allocValue();
            state.eval(e, *v);
            return v;
        });
    }
};

// Test: forceDerivation regenerates a GC'd derivation.
// Simulates real-world scenario: evaluate with one EvalState, GC the .drv,
// then load from persistent cache with a fresh EvalState and regenerate.
// Regeneration happens via getStringWithContext() which detects invalid paths.
TEST_F(ForceDerivationRegenTest, RegeneratesDeletedDerivation)
{
    const std::string expr = R"(
        derivation {
            name = "regen-test";
            system = "x86_64-linux";
            builder = "/bin/sh";
        }
    )";

    StorePath drvPath{StorePath::dummy};

    // Phase 1: First evaluator - create derivation and populate cache
    {
        auto ctx1 = createEvaluator();
        auto evalCache1 = createEvalCache(*ctx1->state, expr);
        auto obj1 = ctx1->evaluator->getRoot(evalCache1);

        // Force the derivation - this instantiates the .drv and caches drvPath
        drvPath = forceDerivation(*ctx1->evaluator, *obj1, *store);
        ASSERT_TRUE(drvPath.isDerivation());
        ASSERT_TRUE(store->isValidPath(drvPath)) << "drvPath should be valid after first evaluation";
    }
    // evalCache1 destructor flushes to persistent storage

    // Phase 2: Delete the derivation to simulate GC
    auto * dummyStore = dynamic_cast<DummyStore *>(&*store);
    ASSERT_NE(dummyStore, nullptr) << "Store must be a DummyStore";
    dummyStore->derivations.erase(drvPath);
    store->clearPathInfoCache();
    ASSERT_FALSE(store->isValidPath(drvPath)) << "drvPath should be invalid after deletion";

    // Phase 3: Fresh evaluator - load from cache and regenerate
    {
        auto ctx2 = createEvaluator();
        auto evalCache2 = createEvalCache(*ctx2->state, expr);
        auto obj2 = ctx2->evaluator->getRoot(evalCache2);

        // Verify cache is working: we can read drvPath from cache without re-instantiating
        auto drvPathAttr = obj2->maybeGetAttr("drvPath");
        ASSERT_NE(drvPathAttr, nullptr) << "drvPath attribute should be accessible from cache";
        auto cachedPathStr = drvPathAttr->getStringIgnoreContext();
        EXPECT_EQ(cachedPathStr, store->printStorePath(drvPath)) << "Cached path should match original";
        ASSERT_FALSE(store->isValidPath(drvPath))
            << "Derivation should still be invalid (read from cache, not re-evaluated)";

        // This triggers regeneration via getStringWithContext() detecting invalid path
        auto drvPath2 = forceDerivation(*ctx2->evaluator, *obj2, *store);

        // Verify regeneration worked
        EXPECT_TRUE(store->isValidPath(drvPath2)) << "drvPath should be valid after regeneration";
        EXPECT_EQ(drvPath, drvPath2) << "Regenerated path should match original";
    }
}

} // namespace nix::expr::helpers
