#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <atomic>
#include <memory>

#include "nix/expr/evaluator.hh"
#include "nix/expr/interpreter.hh"
#include "nix/expr/provenance-object.hh"
#include "nix/expr/coarse-eval-cache.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/expr/search-path.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/util/hash.hh"
#include "nix/store/tests/libstore.hh"
#include "nix/util/tests/gmock-matchers.hh"

namespace nix::expr {

/**
 * Simple external value for testing purposes
 */
class ExternalValueForTesting : public ExternalValueBase
{
protected:
    std::ostream & print(std::ostream & str) const override
    {
        str << "ExternalValueForTesting";
        return str;
    }
public:
    std::string showType() const override
    {
        return "an external value for testing";
    }

    std::string typeOf() const override
    {
        return "external-test";
    }

    ~ExternalValueForTesting() override = default;
};

/**
 * Parameterized test fixture for testing different Evaluator implementations.
 * This ensures all implementations of the Evaluator interface behave consistently.
 */
class EvaluatorTest : public LibStoreTest, public ::testing::WithParamInterface<std::string>
{
protected:
    std::shared_ptr<Evaluator> evaluator;
    std::shared_ptr<EvalState> evalStateForTestSetupOnly; // Only for evalExpression, not for direct use in tests
    int testRunIteration = 0;                             // Track cold vs warm cache runs
    inline static std::atomic<int> cachePathCounter{0};   // Counter for unique cache paths

    // Settings must be member variables to outlive EvalState
    bool readOnlyMode = false; // Allow writing for derivation tests
    fetchers::Settings fetchSettings{};
    EvalSettings evalSettings{readOnlyMode};

    static void SetUpTestSuite()
    {
        LibStoreTest::SetUpTestSuite();
        initGC();
    }

    // Helper methods for implementation type checks
    bool isInterpreter() const
    {
        auto impl = GetParam();
        return impl == "Interpreter" || impl == "InterpreterWithProvenance";
    }

    bool isCoarseCache() const
    {
        auto impl = GetParam();
        return impl == "CoarseEvalCache" || impl == "CoarseEvalCacheWithPersistence"
               || impl == "CoarseEvalCacheWithPersistenceWithProvenance";
    }

    bool isCoarseCacheWithPersistence() const
    {
        auto impl = GetParam();
        return impl == "CoarseEvalCacheWithPersistence" || impl == "CoarseEvalCacheWithPersistenceWithProvenance";
    }

    bool hasProvenanceLayer() const
    {
        auto impl = GetParam();
        return impl == "InterpreterWithProvenance" || impl == "CoarseEvalCacheWithPersistenceWithProvenance";
    }

    EvaluatorTest()
        : LibStoreTest(openStore("dummy://?read-only=false"))
    {
    }

    void SetUp() override
    {
        // Initialize settings
        evalSettings.nixPath = {};
        evalSettings.applyConfig("");

        // Create a fresh EvalState for testing
        auto state = make_ref<EvalState>(
            LookupPath{}, // Empty search path
            store,
            fetchSettings,
            evalSettings,
            nullptr);

        // Save for evalExpression only - tests should use the Evaluator interface
        evalStateForTestSetupOnly = state;

        std::shared_ptr<Evaluator> baseEvaluator;
        if (isInterpreter()) {
            baseEvaluator = std::make_shared<Interpreter>(state);
        } else if (isCoarseCache()) {
            baseEvaluator = std::make_shared<CoarseEvalCache>(make_ref<Interpreter>(state));
        } else {
            throw std::runtime_error("Unknown evaluator implementation: " + GetParam());
        }

        if (hasProvenanceLayer()) {
            evaluator = std::make_shared<ProvenanceEvaluator>(ref<Evaluator>(baseEvaluator));
        } else {
            evaluator = baseEvaluator;
        }
    }

    /**
     * Get path for test cache database - unique for each evalExpression call.
     * Using atomic counter ensures each EvalCache gets its own DB to avoid lock contention.
     */
    static std::filesystem::path getTestCachePath()
    {
        auto tmpDir = std::filesystem::temp_directory_path() / "nix-eval-cache-tests";
        createDirs(tmpDir);
        // Use unique file per evalExpression call to avoid SQLite lock contention
        int id = cachePathCounter.fetch_add(1);
        return tmpDir / ("test-cache-" + std::to_string(getpid()) + "-" + std::to_string(id) + ".sqlite");
    }

    /**
     * Clear all test cache databases and associated files for this process.
     * Removes matching .sqlite files and any WAL/SHM/journal files.
     */
    static void removeTestCache()
    {
        auto tmpDir = std::filesystem::temp_directory_path() / "nix-eval-cache-tests";
        if (!std::filesystem::exists(tmpDir))
            return;

        auto prefix = "test-cache-" + std::to_string(getpid()) + "-";

        for (const auto & entry : std::filesystem::directory_iterator(tmpDir)) {
            auto filename = entry.path().filename().string();
            if (filename.find(prefix) == 0) {
                std::filesystem::remove(entry.path());
            }
        }
    }

    /**
     * Evaluate a Nix expression and return an Object.
     * For Interpreter, delegates to the evaluator (which handles provenance wrapping).
     * For CoarseEvalCache, creates cache instances to test cursor object behavior.
     */
    ref<Object> evalExpression(const std::string & expr)
    {
        // Interpreter cases can use the evaluator directly
        if (isInterpreter()) {
            return evaluator->evalExpr(expr, evaluator->getEvalState().rootPath(CanonPath::root));
        }

        // CoarseEvalCache is designed for flake initialization via getRoot(EvalCache),
        // so evalExpr() falls back to Interpreter without caching. We manually create
        // cache instances here to test CoarseEvalCacheCursorObject's caching behavior.
        auto & state = *evalStateForTestSetupOnly;
        auto e = state.parseExprFromString(expr, state.rootPath(CanonPath::root));
        auto v = state.allocValue();
        state.eval(e, *v);

        ref<Object> result = [&]() -> ref<Object> {
            if (isCoarseCache() && !isCoarseCacheWithPersistence()) {
                auto cache = std::make_shared<eval_cache::EvalCache>(
                    std::optional<std::filesystem::path>(std::nullopt), state, [v]() { return v; });
                return cache->getRoot()->toObjectCompat();
            } else if (isCoarseCacheWithPersistence()) {
                auto cache =
                    std::make_shared<eval_cache::EvalCache>(getTestCachePath(), state, [v]() { return v; });
                return cache->getRoot()->toObjectCompat();
            } else {
                throw std::runtime_error("Unknown evaluator implementation: " + GetParam());
            }
        }();

        if (hasProvenanceLayer()) {
            result = make_ref<ProvenanceObject>(result, state);
        }
        return result;
    }

    /**
     * Parse a Nix expression and return a lazy thunk Object (not evaluated).
     *
     * WARNING: Avoid if possible, because CoarseEvalCache does not support this
     *          and falls back to Interpreter. You're not testing CoarseEvalCache then.
     */
    ref<Object> evalExpressionLazy(const std::string & expr)
    {
        return evaluator->evalExprLazy(expr, evaluator->getEvalState().rootPath(CanonPath::root));
    }

    /**
     * Run test body, handling cache clearing for persistent cache tests.
     * For CoarseEvalCacheWithPersistence, runs the test twice:
     * - First with cold cache
     * - Second with warm cache (reusing existing data)
     */
    template<typename TestBody>
    void runTestWithCaching(TestBody body)
    {
        if (isCoarseCacheWithPersistence()) {
            // Clear cache before the test case
            removeTestCache();

            // Run twice for persistent cache testing
            for (int run = 1; run <= 2; ++run) {
                testRunIteration = run;
                // Reset counter so warm cache run uses same paths as cold cache run
                cachePathCounter = 0;

                if (run == 1) {
                    SCOPED_TRACE("Cold cache run");
                } else {
                    SCOPED_TRACE("Warm cache run");
                }

                body();
            }

            // Clear cache after the test case
            removeTestCache();
        } else {
            // Single run for non-persistent implementations
            testRunIteration = 1;
            cachePathCounter = 0;
            body();
        }
    }
};

// Macro to simplify writing tests that handle cache runs
#define EVALUATOR_TEST(TestName, TestBody)     \
    TEST_P(EvaluatorTest, TestName)            \
    {                                          \
        runTestWithCaching([this]() TestBody); \
    }

// Prevent accidental use of evalStateForTestSetupOnly in test cases
#define evalStateForTestSetupOnly #error evalStateForTestSetupOnly must not be used in tests

// Test Object::maybeGetAttr
EVALUATOR_TEST(Object_maybeGetAttr_ReturnsAttribute, {
    auto obj = evalExpression("{ foo = \"bar\"; baz = \"qux\"; }");
    auto fooAttr = obj->maybeGetAttr("foo");
    ASSERT_NE(fooAttr, nullptr);
    auto fooStr = fooAttr->getStringIgnoreContext();
    EXPECT_EQ(fooStr, "bar");
})

EVALUATOR_TEST(Object_maybeGetAttr_ReturnsNullForMissingAttribute, {
    auto obj = evalExpression("{ foo = \"bar\"; }");
    auto missingAttr = obj->maybeGetAttr("missing");
    EXPECT_EQ(missingAttr, nullptr);
})

EVALUATOR_TEST(Object_maybeGetAttr_ReturnsNullForNonAttrSet, {
    auto obj = evalExpression("\"not an attrset\"");
    auto attr = obj->maybeGetAttr("anything");
    EXPECT_EQ(attr, nullptr);
})

// Test Object::getAttrNames
EVALUATOR_TEST(Object_getAttrNames_ReturnsAttributeNames, {
    auto obj = evalExpression("{ foo = 1; bar = 2; baz = 3; }");
    auto attrNames = obj->getAttrNames();
    ASSERT_EQ(attrNames.size(), 3);
    // Sort for consistent comparison
    std::sort(attrNames.begin(), attrNames.end());
    EXPECT_EQ(attrNames[0], "bar");
    EXPECT_EQ(attrNames[1], "baz");
    EXPECT_EQ(attrNames[2], "foo");
})

EVALUATOR_TEST(Object_getAttrNames_ReturnsEmptyForEmptyAttrset, {
    auto obj = evalExpression("{ }");
    auto attrNames = obj->getAttrNames();
    EXPECT_EQ(attrNames.size(), 0);
})

EVALUATOR_TEST(Object_getAttrNames_ThrowsForNonAttrset, {
    auto obj = evalExpression("42");
    EXPECT_THROW(obj->getAttrNames(), Error);
})

EVALUATOR_TEST(Object_getAttrNames_WorksWithNestedAttrsets, {
    auto obj = evalExpression("{ a = { b = 1; }; c = 2; }");
    auto attrNames = obj->getAttrNames();
    ASSERT_EQ(attrNames.size(), 2);
    std::sort(attrNames.begin(), attrNames.end());
    EXPECT_EQ(attrNames[0], "a");
    EXPECT_EQ(attrNames[1], "c");
})

// Test Object::getStringIgnoreContext
EVALUATOR_TEST(Object_getStringIgnoreContext_ReturnsStringValue, {
    auto obj = evalExpression("\"hello world\"");
    auto str = obj->getStringIgnoreContext();
    EXPECT_EQ(str, "hello world");
})

EVALUATOR_TEST(Object_getStringIgnoreContext_ThrowsForNonString, {
    auto obj = evalExpression("42");
    EXPECT_THROW(obj->getStringIgnoreContext(), Error);
})

EVALUATOR_TEST(Object_getStringIgnoreContext_ThrowsForAttrSet, {
    auto obj = evalExpression("{ foo = \"bar\"; }");
    EXPECT_THROW(obj->getStringIgnoreContext(), Error);
})

// Test nested attribute access
EVALUATOR_TEST(Object_NestedAttributeAccess, {
    auto obj = evalExpression("{ outer = { inner = \"value\"; }; }");
    auto outer = obj->maybeGetAttr("outer");
    ASSERT_NE(outer, nullptr);
    auto inner = outer->maybeGetAttr("inner");
    ASSERT_NE(inner, nullptr);
    auto value = inner->getStringIgnoreContext();
    EXPECT_EQ(value, "value");
})

// Test Object::getBool
EVALUATOR_TEST(Object_getBool_ReturnsTrue, {
    auto obj = evalExpression("true");
    EXPECT_TRUE(obj->getBool(""));
})

EVALUATOR_TEST(Object_getBool_ReturnsFalse, {
    auto obj = evalExpression("false");
    EXPECT_FALSE(obj->getBool(""));
})

EVALUATOR_TEST(Object_getBool_ThrowsWhenNotABool, {
    auto obj = evalExpression("\"not a bool\"");
    try {
        obj->getBool("");
        FAIL();
    } catch (const Error & e) {
        EXPECT_THAT(
            e.what(),
            ::testing::AnyOf(
                nix::testing::HasSubstrIgnoreANSIMatcher("expected a Boolean but found a string"),
                nix::testing::HasSubstrIgnoreANSIMatcher("'' is not a Boolean")));
    }
})

EVALUATOR_TEST(Object_getBool_IncludesErrorContext, {
    auto obj = evalExpression("42");
    try {
        obj->getBool("while checking some_bool_context");
        FAIL();
    } catch (const Error & e) {
        EXPECT_THAT(e.what(), nix::testing::HasSubstrIgnoreANSIMatcher("while checking some_bool_context"));
        EXPECT_THAT(
            e.what(),
            ::testing::AnyOf(
                nix::testing::HasSubstrIgnoreANSIMatcher("expected a Boolean but found an integer"),
                // CoarseEvalCache shows '' (the root attribute path) in the error.
                // This is a contrived test - in practice we use this on specific flake output attributes, so this isn't
                // a problem.
                nix::testing::HasSubstrIgnoreANSIMatcher("'' is not a Boolean")));
    }
})

// Test Object::getInt
EVALUATOR_TEST(Object_getInt_ReturnsInteger, {
    auto obj = evalExpression("42");
    EXPECT_EQ(obj->getInt("").value, 42);
})

EVALUATOR_TEST(Object_getInt_ReturnsNegativeInteger, {
    auto obj = evalExpression("-123");
    EXPECT_EQ(obj->getInt("").value, -123);
})

EVALUATOR_TEST(Object_getInt_ThrowsWhenNotAnInt, {
    auto obj = evalExpression("\"some_string\"");
    try {
        obj->getInt("");
        FAIL();
    } catch (const Error & e) {
        EXPECT_THAT(
            e.what(),
            ::testing::AnyOf(
                nix::testing::HasSubstrIgnoreANSIMatcher("expected an integer but found a string"),
                nix::testing::HasSubstrIgnoreANSIMatcher("'' is not an integer")));
    }
})

EVALUATOR_TEST(Object_getInt_IncludesErrorContext, {
    auto obj = evalExpression("true");
    try {
        obj->getInt("while evaluating some_int_context");
        FAIL();
    } catch (const Error & e) {
        EXPECT_THAT(e.what(), nix::testing::HasSubstrIgnoreANSIMatcher("while evaluating some_int_context"));
        EXPECT_THAT(
            e.what(),
            ::testing::AnyOf(
                nix::testing::HasSubstrIgnoreANSIMatcher("expected an integer but found a Boolean"),
                // CoarseEvalCache shows '' (the root attribute path) in the error.
                // This is a contrived test - in practice we use this on specific flake output attributes, so this isn't
                // a problem.
                nix::testing::HasSubstrIgnoreANSIMatcher("'' is not an integer")));
    }
})

// Test Object::getFloat
EVALUATOR_TEST(Object_getFloat_ReturnsFloat, {
    auto obj = evalExpression("3.14");
    EXPECT_DOUBLE_EQ(obj->getFloat(""), 3.14);
})

EVALUATOR_TEST(Object_getFloat_ReturnsNegativeFloat, {
    auto obj = evalExpression("-2.5");
    EXPECT_DOUBLE_EQ(obj->getFloat(""), -2.5);
})

EVALUATOR_TEST(Object_getFloat_ThrowsWhenNotAFloat, {
    auto obj = evalExpression("42");
    try {
        obj->getFloat("");
        FAIL();
    } catch (const Error & e) {
        EXPECT_THAT(
            e.what(),
            ::testing::AnyOf(
                nix::testing::HasSubstrIgnoreANSIMatcher("expected a float but found an integer"),
                nix::testing::HasSubstrIgnoreANSIMatcher("'' is not a float")));
    }
})

EVALUATOR_TEST(Object_getFloat_IncludesErrorContext, {
    auto obj = evalExpression("\"not a float\"");
    try {
        obj->getFloat("while evaluating some_float_context");
        FAIL();
    } catch (const Error & e) {
        EXPECT_THAT(e.what(), nix::testing::HasSubstrIgnoreANSIMatcher("some_float_context"));
    }
})

// Test Object::getListSize
EVALUATOR_TEST(Object_getListSize_ReturnsSize, {
    auto obj = evalExpression("[1 2 3 4 5]");
    EXPECT_EQ(obj->getListSize(), 5);
})

EVALUATOR_TEST(Object_getListSize_ReturnsZeroForEmptyList, {
    auto obj = evalExpression("[]");
    EXPECT_EQ(obj->getListSize(), 0);
})

EVALUATOR_TEST(Object_getListSize_ThrowsWhenNotAList, {
    auto obj = evalExpression("42");
    try {
        obj->getListSize();
        FAIL();
    } catch (const Error & e) {
        EXPECT_THAT(e.what(), nix::testing::HasSubstrIgnoreANSIMatcher("expected a list but found an integer"));
    }
})

// Test Object::getListElem
EVALUATOR_TEST(Object_getListElem_ReturnsElement, {
    auto obj = evalExpression("[\"first\" \"second\" \"third\"]");
    auto elem0 = obj->getListElem(0);
    auto elem1 = obj->getListElem(1);
    auto elem2 = obj->getListElem(2);
    EXPECT_EQ(elem0->getStringIgnoreContext(), "first");
    EXPECT_EQ(elem1->getStringIgnoreContext(), "second");
    EXPECT_EQ(elem2->getStringIgnoreContext(), "third");
})

EVALUATOR_TEST(Object_getListElem_WorksWithMixedTypes, {
    auto obj = evalExpression("[42 \"hello\" true]");
    auto elem0 = obj->getListElem(0);
    auto elem1 = obj->getListElem(1);
    auto elem2 = obj->getListElem(2);
    EXPECT_EQ(elem0->getInt("").value, 42);
    EXPECT_EQ(elem1->getStringIgnoreContext(), "hello");
    EXPECT_TRUE(elem2->getBool(""));
})

EVALUATOR_TEST(Object_getListElem_ThrowsWhenIndexOutOfBounds, {
    auto obj = evalExpression("[1 2 3]");
    try {
        obj->getListElem(5);
        FAIL();
    } catch (const Error & e) {
        EXPECT_THAT(e.what(), nix::testing::HasSubstrIgnoreANSIMatcher("list index 5 is out of bounds"));
    }
})

EVALUATOR_TEST(Object_getListElem_ThrowsWhenNotAList, {
    auto obj = evalExpression("42");
    try {
        obj->getListElem(0);
        FAIL();
    } catch (const Error & e) {
        EXPECT_THAT(e.what(), nix::testing::HasSubstrIgnoreANSIMatcher("expected a list but found an integer"));
    }
})

// Test Object::getListOfStringsNoCtx
EVALUATOR_TEST(Object_getListOfStringsNoCtx_ReturnsListOfStrings, {
    auto obj = evalExpression("[\"foo\" \"bar\" \"baz\"]");
    auto result = obj->getListOfStringsNoCtx();
    ASSERT_EQ(result.size(), 3);
    EXPECT_EQ(result[0], "foo");
    EXPECT_EQ(result[1], "bar");
    EXPECT_EQ(result[2], "baz");
})

EVALUATOR_TEST(Object_getListOfStringsNoCtx_ThrowsWhenNotAList, {
    auto obj = evalExpression("\"not a list\"");
    try {
        obj->getListOfStringsNoCtx();
        FAIL();
    } catch (const Error & e) {
        EXPECT_THAT(
            e.what(),
            ::testing::AnyOf(
                nix::testing::HasSubstrIgnoreANSIMatcher("expected a list but found a string"),
                nix::testing::HasSubstrIgnoreANSIMatcher("'' is not a list")));
    }
})

EVALUATOR_TEST(Object_getListOfStringsNoCtx_ThrowsWhenListContainsNonString, {
    auto obj = evalExpression("[\"foo\" 42]");
    try {
        obj->getListOfStringsNoCtx();
        FAIL();
    } catch (const Error & e) {
        EXPECT_THAT(
            e.what(),
            ::testing::AnyOf(
                nix::testing::HasSubstrIgnoreANSIMatcher("value is an integer while a string was expected"),
                nix::testing::HasSubstrIgnoreANSIMatcher("expected a string but found an integer")));
    }
})

EVALUATOR_TEST(Object_getListOfStringsNoCtx_ThrowsWhenStringHasContext, {
    auto obj = evalExpression(R"(
        let drv = derivation { name = "test"; system = "x86_64-linux"; builder = "/bin/sh"; };
        in [ "foo" "${drv}" "bar" ]
    )");
    try {
        obj->getListOfStringsNoCtx();
        FAIL();
    } catch (const Error & e) {
        // All implementations produce this exact message format
        EXPECT_THAT(e.what(), nix::testing::HasSubstrIgnoreANSIMatcher("the string '/nix/store/"));
        EXPECT_THAT(
            e.what(), nix::testing::HasSubstrIgnoreANSIMatcher("' is not allowed to refer to a store path (such as '"));
        EXPECT_THAT(e.what(), nix::testing::HasSubstrIgnoreANSIMatcher("-test.drv"));
        EXPECT_THAT(e.what(), nix::testing::HasSubstrIgnoreANSIMatcher("while evaluating a list element at index 1"));
    }
})

EVALUATOR_TEST(Object_getListOfStringsNoCtx_ReturnsEmptyListForEmptyList, {
    auto obj = evalExpression("[]");
    auto result = obj->getListOfStringsNoCtx();
    EXPECT_EQ(result.size(), 0);
})

// Test Object::getType and getTypeLazy for nThunk
EVALUATOR_TEST(Object_getType_nThunk, {
    // Note: This test only works with Interpreter because CoarseEvalCache
    // always forces values, so it never exposes thunks
    if (!isInterpreter()) {
        GTEST_SKIP() << "Thunk testing only implemented for Interpreter";
    }

    // Create an attrset with a thunk value: the argument to f is a thunk
    auto obj = evalExpression("{ a = (let f = x: x; in f 1); }");
    auto attrA = obj->maybeGetAttr("a");
    ASSERT_NE(attrA, nullptr);

    // For Interpreter, the attribute value should still be a thunk
    // getTypeLazy should return nThunk without forcing
    EXPECT_EQ(attrA->getTypeLazy(), nThunk);

    // getType should force evaluation and return the actual type
    EXPECT_EQ(attrA->getType(), nInt);
})

// Test Object::getType and getTypeLazy for nInt
EVALUATOR_TEST(Object_getType_nInt, {
    auto obj = evalExpression("{ x = (v: v) 42; }")->maybeGetAttr("x");
    ASSERT_NE(obj, nullptr);
    EXPECT_THAT(obj->getTypeLazy(), ::testing::AnyOf(nThunk, nInt));
    EXPECT_EQ(obj->getType(), nInt);
})

// Test Object::getType and getTypeLazy for nFloat
EVALUATOR_TEST(Object_getType_nFloat, {
    auto obj = evalExpression("{ x = (v: v) 3.14; }")->maybeGetAttr("x");
    ASSERT_NE(obj, nullptr);
    EXPECT_THAT(obj->getTypeLazy(), ::testing::AnyOf(nThunk, nFloat));
    EXPECT_EQ(obj->getType(), nFloat);
})

// Test Object::getType and getTypeLazy for nBool
EVALUATOR_TEST(Object_getType_nBool, {
    auto obj = evalExpression("{ x = (v: v) true; }")->maybeGetAttr("x");
    ASSERT_NE(obj, nullptr);
    EXPECT_THAT(obj->getTypeLazy(), ::testing::AnyOf(nThunk, nBool));
    EXPECT_EQ(obj->getType(), nBool);
})

// Test Object::getType and getTypeLazy for nString
EVALUATOR_TEST(Object_getType_nString, {
    auto obj = evalExpression("{ x = (v: v) \"test string\"; }")->maybeGetAttr("x");
    ASSERT_NE(obj, nullptr);
    EXPECT_THAT(obj->getTypeLazy(), ::testing::AnyOf(nThunk, nString));
    EXPECT_EQ(obj->getType(), nString);
})

// Test Object::getType and getTypeLazy for nPath
EVALUATOR_TEST(Object_getType_nPath, {
    auto obj = evalExpression("{ x = (v: v) /some/path; }")->maybeGetAttr("x");
    ASSERT_NE(obj, nullptr);
    // Note: Paths are coerced to strings in the cache, which is undesirable but reflects current behavior
    EXPECT_THAT(obj->getTypeLazy(), ::testing::AnyOf(nThunk, nPath, nString));
    if (!isCoarseCache()) {
        EXPECT_EQ(obj->getType(), nPath);
    } else {
        // CoarseEvalCache coerces paths to strings when caching
        EXPECT_THAT(obj->getType(), ::testing::AnyOf(nPath, nString));
    }
})

// Test Object::getType and getTypeLazy for nNull
EVALUATOR_TEST(Object_getType_nNull, {
    auto obj = evalExpression("{ x = (v: v) null; }")->maybeGetAttr("x");
    ASSERT_NE(obj, nullptr);
    EXPECT_THAT(obj->getTypeLazy(), ::testing::AnyOf(nThunk, nNull));
    EXPECT_EQ(obj->getType(), nNull);
})

// Test Object::getType and getTypeLazy for nAttrs
EVALUATOR_TEST(Object_getType_nAttrs, {
    auto obj = evalExpression("{ foo = \"bar\"; }");
    EXPECT_THAT(obj->getTypeLazy(), ::testing::AnyOf(nThunk, nAttrs));
    EXPECT_EQ(obj->getType(), nAttrs);
})

// Test Object::getType and getTypeLazy for nList
EVALUATOR_TEST(Object_getType_nList, {
    auto obj = evalExpression("[\"foo\"]");
    EXPECT_THAT(obj->getTypeLazy(), ::testing::AnyOf(nThunk, nList));
    EXPECT_EQ(obj->getType(), nList);
})

// Test Object::getType and getTypeLazy for nFunction
EVALUATOR_TEST(Object_getType_nFunction, {
    auto obj = evalExpression("x: x + 1");
    EXPECT_THAT(obj->getTypeLazy(), ::testing::AnyOf(nThunk, nFunction));
    EXPECT_EQ(obj->getType(), nFunction);
})

// Test Object::getType and getTypeLazy for nExternal
EVALUATOR_TEST(Object_getType_nExternal, {
    // External values are plugin-defined values
    // There's no Nix syntax to create them, and we cannot create them
    // through the Object interface without internal state access.
    // Skip this test as external values are not commonly used in practice.
    // TODO: add test for Interpreter only
    (void) this;
    GTEST_SKIP() << "Cannot test external values without internal state access";
})

// Test Object::getStringWithContext
// See also: ForceDerivationRegenTest in force-derivation-regen.cc (tests GC'd path regeneration)
EVALUATOR_TEST(Object_getStringWithContext_PlainString, {
    auto obj = evalExpression("\"hello world\"");
    auto result = obj->getStringWithContext();
    EXPECT_EQ(result.first, "hello world");
    EXPECT_TRUE(result.second.empty());
})

EVALUATOR_TEST(Object_getStringWithContext_WithDerivationContext, {
    auto obj = evalExpression(R"(
        let drv = derivation { name = "test"; system = "x86_64-linux"; builder = "/bin/sh"; };
        in "${drv.drvPath}"
    )");
    auto result = obj->getStringWithContext();
    // String should be the drv path
    EXPECT_TRUE(result.first.ends_with(".drv"));
    // Context should contain the derivation
    EXPECT_FALSE(result.second.empty());
    EXPECT_EQ(result.second.size(), 1);
})

EVALUATOR_TEST(Object_getStringWithContext_WithOutputContext, {
    auto obj = evalExpression(R"(
        let drv = derivation { name = "test"; system = "x86_64-linux"; builder = "/bin/sh"; };
        in "${drv.out}"
    )");
    auto result = obj->getStringWithContext();
    // String should be a store path
    EXPECT_TRUE(result.first.starts_with("/nix/store/"));
    // Context should contain the output path
    EXPECT_FALSE(result.second.empty());
})

EVALUATOR_TEST(Object_getStringWithContext_WithMultipleOutputs, {
    auto obj = evalExpression(R"(
        let drv = derivation {
            name = "multi-output-test";
            system = "x86_64-linux";
            builder = "/bin/sh";
            outputs = [ "out" "dev" "doc" ];
        };
        in "${drv.out} ${drv.dev}"
    )");
    auto result = obj->getStringWithContext();
    // String should contain store paths separated by space
    EXPECT_TRUE(result.first.starts_with("/nix/store/"));
    EXPECT_TRUE(result.first.find(" /nix/store/") != std::string::npos);
    // Context should contain multiple output references
    EXPECT_FALSE(result.second.empty());
    EXPECT_GE(result.second.size(), 2);
})

EVALUATOR_TEST(Object_getStringWithContext_ThrowsForNonString, {
    auto obj = evalExpression("42");
    try {
        obj->getStringWithContext();
        FAIL();
    } catch (const Error & e) {
        EXPECT_THAT(
            e.what(),
            ::testing::AnyOf(
                nix::testing::HasSubstrIgnoreANSIMatcher("value is an integer while a string was expected"),
                nix::testing::HasSubstrIgnoreANSIMatcher("expected a string but found an integer"),
                // CoarseEvalCache shows '' (the root attribute path) in the error
                nix::testing::HasSubstrIgnoreANSIMatcher("'' is not a string")));
    }
})

EVALUATOR_TEST(Object_getStringWithContext_CoercesPath, {
    // Skip for Interpreter - it doesn't coerce paths in getStringWithContext
    // NOTE: Path coercion to string is not actually desirable behavior,
    // but this test documents the current implementation difference.
    if (!isCoarseCache()) {
        GTEST_SKIP() << "Interpreter doesn't coerce paths in getStringWithContext";
    }
    auto obj = evalExpression("/some/path");
    auto result = obj->getStringWithContext();
    EXPECT_EQ(result.first, "/some/path");
    EXPECT_TRUE(result.second.empty());
})

// Test Object::getStringWithoutContext
EVALUATOR_TEST(Object_getStringWithoutContext_PlainString, {
    auto obj = evalExpression("\"hello world\"");
    EXPECT_EQ(obj->getStringWithoutContext(), "hello world");
})

EVALUATOR_TEST(Object_getStringWithoutContext_ThrowsForStringWithContext, {
    auto obj = evalExpression(R"(
        let drv = derivation { name = "test"; system = "x86_64-linux"; builder = "/bin/sh"; };
        in "${drv}"
    )");
    try {
        obj->getStringWithoutContext();
        FAIL() << "Expected an error to be thrown";
    } catch (const EvalError & e) {
        auto msg = filterANSIEscapes(e.info().msg.str(), true);
        EXPECT_EQ(
            msg,
            "the string '/nix/store/d62izaahds46siwr2b7k7q3gan6vw4p0-test' is not allowed to refer to a store path (such as '/nix/store/y1s2fiq89v2h9vkb38w508ir20dwv6v2-test.drv^out')");
    }
})

EVALUATOR_TEST(Object_getStringWithoutContext_ThrowsForNonString, {
    auto obj = evalExpression("42");
    EXPECT_THROW(obj->getStringWithoutContext(), EvalError);
})

// Test Object::getPath
EVALUATOR_TEST(Object_getPath_ReturnsPath, {
    // CoarseEvalCache coerces paths to strings in the cache, which breaks getPath()
    // This is a limitation of the current database format
    if (isCoarseCache()) {
        GTEST_SKIP() << "Path caching not supported in current database format";
    }
    auto obj = evalExpression("/some/path");
    auto path = obj->getPath();
    EXPECT_EQ(path.path.abs(), "/some/path");
})

EVALUATOR_TEST(Object_getPath_ThrowsForNonPath, {
    auto obj = evalExpression("\"not a path\"");
    try {
        obj->getPath();
        FAIL();
    } catch (const Error & e) {
        EXPECT_THAT(
            e.what(),
            ::testing::AnyOf(
                nix::testing::HasSubstrIgnoreANSIMatcher("value is a string while a path was expected"),
                nix::testing::HasSubstrIgnoreANSIMatcher("expected a path but found a string")));
    }
})

EVALUATOR_TEST(Object_getPath_ThrowsForInteger, {
    auto obj = evalExpression("42");
    try {
        obj->getPath();
        FAIL();
    } catch (const Error & e) {
        EXPECT_THAT(
            e.what(),
            ::testing::AnyOf(
                nix::testing::HasSubstrIgnoreANSIMatcher("value is an integer while a path was expected"),
                nix::testing::HasSubstrIgnoreANSIMatcher("expected a path but found an integer")));
    }
})

// Test Object::defeatCache() - bypasses lossy cache to get actual Value
EVALUATOR_TEST(Object_defeatCache_ReturnsValue, {
    auto obj = evalExpression("42");
    auto value = obj->defeatCache();
    ASSERT_NE(value, nullptr);
    EXPECT_EQ((*value)->type(), nInt);
    EXPECT_EQ((*value)->integer().value, 42);
})

EVALUATOR_TEST(Object_defeatCache_WorksWithPaths, {
    // This tests the specific case where defeatCache() is needed:
    // paths are cached as strings without context (lossy)
    auto obj = evalExpression("/some/path");
    auto value = obj->defeatCache();
    ASSERT_NE(value, nullptr);
    // For Interpreter, this should be nPath
    // For CoarseEvalCache, it might be nString (cache is lossy)
    // But defeatCache() should give us the actual type
    if (!isCoarseCache()) {
        EXPECT_EQ((*value)->type(), nPath);
    }
    // Note: CoarseEvalCache defeatCache() forces evaluation, so it should also return nPath
    EXPECT_EQ((*value)->type(), nPath);
})

EVALUATOR_TEST(Object_defeatCache_WorksWithStringsWithContext, {
    // Create a string with context (from a derivation)
    auto obj = evalExpression(R"(
        let drv = derivation { name = "test"; system = "x86_64-linux"; builder = "/bin/sh"; };
        in "${drv}"
    )");
    auto value = obj->defeatCache();
    ASSERT_NE(value, nullptr);
    EXPECT_EQ((*value)->type(), nString);
    // The string should have context (derivation path)
    // context() returns a pointer to Context struct, or nullptr if no context
    EXPECT_NE((*value)->context(), nullptr);
    EXPECT_GT((*value)->context()->size(), 0);
})

// Test Evaluator::mkString - construct a string Object
EVALUATOR_TEST(Evaluator_mkString_CreatesString, {
    auto obj = evaluator->mkString("hello world");
    EXPECT_EQ(obj->getType(), nString);
    EXPECT_EQ(obj->getStringIgnoreContext(), "hello world");
})

EVALUATOR_TEST(Evaluator_mkString_EmptyString, {
    auto obj = evaluator->mkString("");
    EXPECT_EQ(obj->getType(), nString);
    EXPECT_EQ(obj->getStringIgnoreContext(), "");
})

EVALUATOR_TEST(Evaluator_mkString_WithSpecialChars, {
    auto obj = evaluator->mkString("line1\nline2\ttab");
    EXPECT_EQ(obj->getStringIgnoreContext(), "line1\nline2\ttab");
})

// Type alias for mkAttrs tests (template brackets confuse macros)
using ObjectAttrMap = std::map<std::string, ref<Object>>;

// Test Evaluator::mkAttrs - construct an attrset Object
EVALUATOR_TEST(Evaluator_mkAttrs_CreatesAttrset, {
    ObjectAttrMap attrs;
    attrs.insert_or_assign("foo", evaluator->mkString("bar"));
    attrs.insert_or_assign("baz", evaluator->mkString("qux"));
    auto obj = evaluator->mkAttrs(attrs);
    EXPECT_EQ(obj->getType(), nAttrs);
    auto fooAttr = obj->maybeGetAttr("foo");
    ASSERT_NE(fooAttr, nullptr);
    EXPECT_EQ(fooAttr->getStringIgnoreContext(), "bar");
    auto bazAttr = obj->maybeGetAttr("baz");
    ASSERT_NE(bazAttr, nullptr);
    EXPECT_EQ(bazAttr->getStringIgnoreContext(), "qux");
})

EVALUATOR_TEST(Evaluator_mkAttrs_EmptyAttrset, {
    ObjectAttrMap attrs;
    auto obj = evaluator->mkAttrs(attrs);
    EXPECT_EQ(obj->getType(), nAttrs);
    EXPECT_EQ(obj->getAttrNames().size(), 0);
})

EVALUATOR_TEST(Evaluator_mkAttrs_NestedAttrsets, {
    ObjectAttrMap inner;
    inner.insert_or_assign("x", evaluator->mkString("nested"));
    ObjectAttrMap outer;
    outer.insert_or_assign("inner", evaluator->mkAttrs(inner));
    auto obj = evaluator->mkAttrs(outer);
    auto innerObj = obj->maybeGetAttr("inner");
    ASSERT_NE(innerObj, nullptr);
    auto xObj = innerObj->maybeGetAttr("x");
    ASSERT_NE(xObj, nullptr);
    EXPECT_EQ(xObj->getStringIgnoreContext(), "nested");
})

// Test Object::getFunctionInfo - function reflection
EVALUATOR_TEST(Object_getFunctionInfo_SimpleLambda, {
    // Simple lambda without formals: x: x + 1
    auto obj = evalExpression("x: x + 1");
    EXPECT_EQ(obj->getType(), nFunction);
    // Simple lambdas don't have formals
    auto info = obj->getFunctionInfo();
    EXPECT_FALSE(info.has_value());
})

EVALUATOR_TEST(Object_getFunctionInfo_WithFormals, {
    // Lambda with formals: { a, b }: a + b
    auto obj = evalExpression("{ a, b }: a + b");
    EXPECT_EQ(obj->getType(), nFunction);
    auto info = obj->getFunctionInfo();
    ASSERT_TRUE(info.has_value());
    EXPECT_FALSE(info->ellipsis);
    EXPECT_EQ(info->formals.size(), 2);
    // Check both formals are present without defaults
    EXPECT_THAT(
        info->formals, ::testing::UnorderedElementsAre(::testing::Pair("a", false), ::testing::Pair("b", false)));
})

EVALUATOR_TEST(Object_getFunctionInfo_WithEllipsis, {
    // Lambda with formals and ellipsis: { a, ... }: a
    auto obj = evalExpression("{ a, ... }: a");
    EXPECT_EQ(obj->getType(), nFunction);
    auto info = obj->getFunctionInfo();
    ASSERT_TRUE(info.has_value());
    EXPECT_TRUE(info->ellipsis);
    EXPECT_EQ(info->formals.size(), 1);
    EXPECT_TRUE(info->formals.contains("a"));
    EXPECT_FALSE(info->formals.at("a")); // no default
})

EVALUATOR_TEST(Object_getFunctionInfo_NotAFunction, {
    // Not a function
    auto obj = evalExpression("42");
    EXPECT_EQ(obj->getType(), nInt);
    auto info = obj->getFunctionInfo();
    EXPECT_FALSE(info.has_value());
})

EVALUATOR_TEST(Object_getFunctionInfo_WithArgName, {
    // Lambda with arg name and formals: args@{ x, y }: x + y
    auto obj = evalExpression("args@{ x, y }: x + y");
    EXPECT_EQ(obj->getType(), nFunction);
    auto info = obj->getFunctionInfo();
    ASSERT_TRUE(info.has_value());
    EXPECT_FALSE(info->ellipsis);
    EXPECT_THAT(
        info->formals, ::testing::UnorderedElementsAre(::testing::Pair("x", false), ::testing::Pair("y", false)));
})

EVALUATOR_TEST(Object_getFunctionInfo_WithDefaults, {
    // Lambda with some defaults: { a, b ? 42 }: a + b
    auto obj = evalExpression("{ a, b ? 42 }: a + b");
    EXPECT_EQ(obj->getType(), nFunction);
    auto info = obj->getFunctionInfo();
    ASSERT_TRUE(info.has_value());
    EXPECT_FALSE(info->ellipsis);
    EXPECT_EQ(info->formals.size(), 2);
    EXPECT_THAT(
        info->formals, ::testing::UnorderedElementsAre(::testing::Pair("a", false), ::testing::Pair("b", true)));
})

// Test Evaluator::apply - function application
EVALUATOR_TEST(Evaluator_apply_SimpleLambda, {
    // Simple lambda: x: x + 1, applied to 5
    auto fn = evalExpression("x: x + 1");
    auto arg = evalExpression("5");
    auto result = evaluator->apply(ref<Object>(fn), ref<Object>(arg));
    EXPECT_EQ(result->getType(), nInt);
    EXPECT_EQ(result->getInt(), NixInt(6));
})

EVALUATOR_TEST(Evaluator_apply_WithFormals, {
    // Lambda with formals: { a, b }: a + b
    auto fn = evalExpression("{ a, b }: a + b");
    ObjectAttrMap attrs;
    attrs.insert_or_assign("a", evaluator->mkString("hello"));
    attrs.insert_or_assign("b", evaluator->mkString(" world"));
    auto arg = evaluator->mkAttrs(attrs);
    auto result = evaluator->apply(ref<Object>(fn), arg);
    EXPECT_EQ(result->getType(), nString);
    EXPECT_EQ(result->getStringIgnoreContext(), "hello world");
})

EVALUATOR_TEST(Evaluator_apply_WithEllipsis, {
    // Lambda with ellipsis: { a, ... }: a
    auto fn = evalExpression("{ a, ... }: a");
    ObjectAttrMap attrs;
    attrs.insert_or_assign("a", evaluator->mkString("value"));
    attrs.insert_or_assign("extra", evaluator->mkString("ignored"));
    auto arg = evaluator->mkAttrs(attrs);
    auto result = evaluator->apply(ref<Object>(fn), arg);
    EXPECT_EQ(result->getType(), nString);
    EXPECT_EQ(result->getStringIgnoreContext(), "value");
})

EVALUATOR_TEST(Evaluator_apply_Curried, {
    // Curried function: a: b: a + b
    auto fn = evalExpression("a: b: a + b");
    auto arg1 = evalExpression("10");
    auto partial = evaluator->apply(ref<Object>(fn), ref<Object>(arg1));
    EXPECT_EQ(partial->getType(), nFunction);
    auto arg2 = evalExpression("20");
    auto result = evaluator->apply(partial, ref<Object>(arg2));
    EXPECT_EQ(result->getType(), nInt);
    EXPECT_EQ(result->getInt(), NixInt(30));
})

EVALUATOR_TEST(Evaluator_apply_IsLazy, {
    // apply should be lazy - constructing the result Object should not throw
    // even if the result would throw when forced
    auto fn = evalExpression("x: throw \"error\"");
    auto arg = evalExpression("42");
    // This should NOT throw - apply is lazy
    auto result = evaluator->apply(ref<Object>(fn), ref<Object>(arg));
    // The result is a thunk until forced
    EXPECT_EQ(result->getTypeLazy(), nThunk);
    // Only when we force it should we get the error
    EXPECT_THROW(result->getType(), Error);
})

// Test ProvenanceObject, wrapping the other evaluators
EVALUATOR_TEST(ProvenanceObject_RootHasNoPos, {
    auto obj = evalExpression("{ foo = 42; }");
    auto provObj = std::make_shared<ProvenanceObject>(ref<Object>(obj), evaluator->getEvalState());
    EXPECT_EQ(provObj->getPos(), noPos) << "Root ProvenanceObject should have noPos";
})

EVALUATOR_TEST(ProvenanceObject_DelegatesGetType, {
    auto obj = evalExpression("{ foo = 42; }");
    auto provObj = std::make_shared<ProvenanceObject>(ref<Object>(obj), evaluator->getEvalState());
    EXPECT_EQ(provObj->getType(), nAttrs);
})

EVALUATOR_TEST(ProvenanceObject_MaybeGetAttrDelegates, {
    auto obj = evalExpression("{ foo = 42; }");
    auto provObj = std::make_shared<ProvenanceObject>(ref<Object>(obj), evaluator->getEvalState());
    auto foo = provObj->maybeGetAttr("foo");
    ASSERT_NE(foo, nullptr);
    EXPECT_EQ(foo->getType(), nInt);
    EXPECT_EQ(foo->getInt(), NixInt(42));
})

EVALUATOR_TEST(ProvenanceObject_ChildHasPosition, {
    auto obj = evalExpression("{ foo = 42; }");
    auto provObj = std::make_shared<ProvenanceObject>(ref<Object>(obj), evaluator->getEvalState());
    auto foo = provObj->maybeGetAttr("foo");
    ASSERT_NE(foo, nullptr);
    // After navigating via maybeGetAttr, getPos() should return the attr position
    auto pos = foo->getPos();
    EXPECT_NE(pos, noPos) << "Child should have position from attr lookup";
})

EVALUATOR_TEST(ProvenanceObject_NestedNavigation, {
    auto obj = evalExpression("{ a = { b = { c = 123; }; }; }");
    auto provObj = std::make_shared<ProvenanceObject>(ref<Object>(obj), evaluator->getEvalState());
    auto a = provObj->maybeGetAttr("a");
    ASSERT_NE(a, nullptr);
    auto b = a->maybeGetAttr("b");
    ASSERT_NE(b, nullptr);
    auto c = b->maybeGetAttr("c");
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->getInt(), NixInt(123));
    // Each level should have position
    EXPECT_NE(a->getPos(), noPos);
    EXPECT_NE(b->getPos(), noPos);
    EXPECT_NE(c->getPos(), noPos);
})

// Instantiate tests for each implementation
INSTANTIATE_TEST_SUITE_P(
    EvaluatorImplementations,
    EvaluatorTest,
    ::testing::Values(
        "Interpreter",
        "CoarseEvalCache",
        "CoarseEvalCacheWithPersistence",
        "InterpreterWithProvenance",
        "CoarseEvalCacheWithPersistenceWithProvenance"),
    [](const ::testing::TestParamInfo<std::string> & info) { return info.param; });

} // namespace nix::expr