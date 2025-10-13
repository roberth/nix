#include <gtest/gtest.h>
#include <memory>

#include "nix/expr/coarse-eval-cache.hh"
#include "nix/expr/coarse-eval-cache-cursor-object.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/interpreter.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/expr/search-path.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/store/tests/libstore.hh"

namespace nix::expr {

/**
 * Test fixture for CoarseEvalCache-specific tests
 */
class CoarseEvalCacheTest : public LibStoreTest
{
protected:
    std::shared_ptr<EvalState> state;
    std::shared_ptr<CoarseEvalCache> evaluator;

    // Settings must be member variables to outlive EvalState
    bool readOnlyMode = true;
    fetchers::Settings fetchSettings{};
    EvalSettings evalSettings{readOnlyMode};

    static void SetUpTestSuite()
    {
        LibStoreTest::SetUpTestSuite();
        initGC();
    }

    void SetUp() override
    {
        evalSettings.nixPath = {};

        auto stateRef = make_ref<EvalState>(LookupPath{}, store, fetchSettings, evalSettings, nullptr);
        state = stateRef;
        evaluator = std::make_shared<CoarseEvalCache>(make_ref<Interpreter>(stateRef));
    }

    ref<eval_cache::EvalCache> createEvalCache(const std::string & expr)
    {
        auto e = state->parseExprFromString(expr, state->rootPath(CanonPath::root));
        auto v = state->allocValue();
        state->eval(e, *v);
        return make_ref<eval_cache::EvalCache>(std::nullopt, *state, [v]() { return v; });
    }
};

// Test wrapping an EvalCache as an Object
TEST_F(CoarseEvalCacheTest, WrapEvalCacheAsObject)
{
    auto evalCache = createEvalCache("{ foo = \"bar\"; nested = { x = 42; }; }");
    auto obj = std::make_shared<CoarseEvalCacheCursorObject>(evalCache->getRoot());

    // Verify we can navigate through the Object interface
    auto foo = obj->maybeGetAttr("foo");
    ASSERT_NE(foo, nullptr);
    EXPECT_EQ(foo->getStringIgnoreContext(), "bar");

    auto nested = obj->maybeGetAttr("nested");
    ASSERT_NE(nested, nullptr);

    auto x = nested->maybeGetAttr("x");
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->getInt(), NixInt{42});
}

// Test that CoarseEvalCache::getRoot wraps an EvalCache as Object
TEST_F(CoarseEvalCacheTest, CreateObjectFromEvalCache)
{
    auto evalCache = createEvalCache("{ packages.x86_64-linux.default = \"dummy-package\"; }");
    auto root = evaluator->getRoot(evalCache);

    auto packages = root->maybeGetAttr("packages");
    ASSERT_NE(packages, nullptr);

    auto x86_64 = packages->maybeGetAttr("x86_64-linux");
    ASSERT_NE(x86_64, nullptr);

    auto defaultPkg = x86_64->maybeGetAttr("default");
    ASSERT_NE(defaultPkg, nullptr);
    EXPECT_EQ(defaultPkg->getStringIgnoreContext(), "dummy-package");
}

} // namespace nix::expr