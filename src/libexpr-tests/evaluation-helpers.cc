#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "nix/expr/evaluation-helpers.hh"
#include "nix/expr/interpreter.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/util/tests/gmock-matchers.hh"

namespace nix::expr::helpers {

class EvaluatorHelpersTest : public LibExprTest
{
protected:
    EvaluatorHelpersTest()
        : LibExprTest()
        , evaluator(statePtr)
    {
    }

    Interpreter evaluator;

    Value * makeAttrs(const std::map<std::string, std::string> & attrs)
    {
        auto v = state.allocValue();
        auto bindings = state.buildBindings(attrs.size());
        for (auto & [name, value] : attrs) {
            auto vStr = state.allocValue();
            vStr->mkString(value, state.mem);
            bindings.insert(state.symbols.create(name), vStr);
        }
        v->mkAttrs(bindings.finish());
        return v;
    }

    Value * makeString(const std::string & s)
    {
        auto v = state.allocValue();
        v->mkString(s, state.mem);
        return v;
    }
};

TEST_F(EvaluatorHelpersTest, isDerivation_ReturnsTrueForDerivation)
{
    // Create an attrset with type = "derivation"
    auto v = makeAttrs({{"type", "derivation"}});
    auto obj = state.toObjectCompat(*v);

    EXPECT_TRUE(isDerivation(*obj));
}

TEST_F(EvaluatorHelpersTest, isDerivation_ReturnsFalseForNonDerivation)
{
    // Create an attrset with type = "package"
    auto v = makeAttrs({{"type", "package"}});
    auto obj = state.toObjectCompat(*v);

    EXPECT_FALSE(isDerivation(*obj));
}

TEST_F(EvaluatorHelpersTest, isDerivation_ReturnsFalseWhenTypeAttributeMissing)
{
    // Create an attrset without a type attribute
    auto v = makeAttrs({{"name", "test"}});
    auto obj = state.toObjectCompat(*v);

    EXPECT_FALSE(isDerivation(*obj));
}

TEST_F(EvaluatorHelpersTest, isDerivation_ReturnsFalseWhenNotAnAttrSet)
{
    // Create a string value instead of an attrset
    auto v = makeString("not an attrset");
    auto obj = state.toObjectCompat(*v);

    EXPECT_FALSE(isDerivation(*obj));
}

TEST_F(EvaluatorHelpersTest, isDerivation_ReturnsFalseWhenTypeIsNotString)
{
    // Create an attrset where type is not a string (e.g., a number)
    auto v = state.allocValue();
    auto bindings = state.buildBindings(1);
    auto vNum = state.allocValue();
    vNum->mkInt(42);
    bindings.insert(state.symbols.create("type"), vNum);
    v->mkAttrs(bindings.finish());

    auto obj = state.toObjectCompat(*v);

    EXPECT_FALSE(isDerivation(*obj));
}

// Tests for forceDerivation helper
TEST_F(EvaluatorHelpersTest, forceDerivation_ReturnsDerivationPath)
{
    auto expr = state.parseExprFromString(
        "derivation { name = \"test\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; }", state.rootPath("."));
    auto v = state.allocValue();
    state.eval(expr, *v);
    auto obj = state.toObjectCompat(*v);

    auto drvPath = forceDerivation(evaluator, *obj, *store);

    // Check that we got a derivation path
    EXPECT_TRUE(drvPath.isDerivation());
    // The path should end with .drv
    auto pathStr = store->printStorePath(drvPath);
    EXPECT_TRUE(pathStr.ends_with(".drv"));
    // The path should contain the name "test"
    EXPECT_TRUE(pathStr.find("test") != std::string::npos);
}

TEST_F(EvaluatorHelpersTest, forceDerivation_ThrowsWhenMissingDrvPath)
{
    auto v = makeAttrs({{"name", "test"}, {"type", "derivation"}});
    auto obj = state.toObjectCompat(*v);

    try {
        forceDerivation(evaluator, *obj, *store);
        FAIL() << "Expected Error to be thrown";
    } catch (const Error & e) {
        EXPECT_THAT(e.what(), ::testing::HasSubstr("derivation does not contain a 'drvPath' attribute"));
    }
}

TEST_F(EvaluatorHelpersTest, forceDerivation_ThrowsWhenInvalidDrvPath)
{
    // builtins.toFile returns a store path string that doesn't end in .drv
    auto expr = state.parseExprFromString(
        R"({
            type = "derivation";
            drvPath = builtins.toFile "not-a-drv" "content";
        })",
        state.rootPath("."));
    auto v = state.allocValue();
    state.eval(expr, *v);
    auto obj = state.toObjectCompat(*v);

    try {
        forceDerivation(evaluator, *obj, *store);
        FAIL() << "Expected Error to be thrown";
    } catch (const Error & e) {
        EXPECT_THAT(e.what(), ::testing::HasSubstr("while evaluating the 'drvPath' attribute of a derivation"));
    }
}

TEST_F(EvaluatorHelpersTest, forceDerivation_ThrowsWhenDrvPathNotString)
{
    auto v = state.allocValue();
    auto bindings = state.buildBindings(2);
    auto vType = state.allocValue();
    vType->mkString("derivation", state.mem);
    bindings.insert(state.symbols.create("type"), vType);
    auto vDrvPath = state.allocValue();
    vDrvPath->mkInt(42);
    bindings.insert(state.symbols.create("drvPath"), vDrvPath);
    v->mkAttrs(bindings.finish());

    auto obj = state.toObjectCompat(*v);

    try {
        forceDerivation(evaluator, *obj, *store);
        FAIL() << "Expected Error to be thrown";
    } catch (const Error & e) {
        EXPECT_THAT(
            e.what(), nix::testing::HasSubstrIgnoreANSIMatcher("value is an integer while a string was expected"));
    }
}

TEST_F(EvaluatorHelpersTest, getDerivationOutputs_ReturnsDefaultOut)
{
    auto expr = state.parseExprFromString(
        "derivation { name = \"test\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; }", state.rootPath("."));
    auto v = state.allocValue();
    state.eval(expr, *v);
    auto obj = state.toObjectCompat(*v);

    auto outputs = getDerivationOutputs(*obj);

    EXPECT_EQ(outputs.size(), 1);
    EXPECT_TRUE(outputs.count("out"));
}

TEST_F(EvaluatorHelpersTest, getDerivationOutputs_ReturnsOutputsToInstallFromMeta)
{
    auto expr = state.parseExprFromString(
        R"(
        (derivation {
            name = "test";
            system = "x86_64-linux";
            builder = "/bin/sh";
        }) // { meta = { outputsToInstall = [ "bin" "dev" ]; }; }
    )",
        state.rootPath("."));
    auto v = state.allocValue();
    state.eval(expr, *v);
    auto obj = state.toObjectCompat(*v);

    auto outputs = getDerivationOutputs(*obj);

    EXPECT_EQ(outputs.size(), 2);
    EXPECT_TRUE(outputs.count("bin"));
    EXPECT_TRUE(outputs.count("dev"));
}

TEST_F(EvaluatorHelpersTest, getDerivationOutputs_ReturnsOutputNameWhenOutputSpecified)
{
    auto expr = state.parseExprFromString(
        R"(
        (derivation {
            name = "test";
            system = "x86_64-linux";
            builder = "/bin/sh";
        }) // { outputSpecified = true; outputName = "custom"; }
    )",
        state.rootPath("."));
    auto v = state.allocValue();
    state.eval(expr, *v);
    auto obj = state.toObjectCompat(*v);

    auto outputs = getDerivationOutputs(*obj);

    EXPECT_EQ(outputs.size(), 1);
    EXPECT_TRUE(outputs.count("custom"));
}

TEST_F(EvaluatorHelpersTest, getDerivationOutputs_PrefersOutputSpecifiedOverMeta)
{
    auto expr = state.parseExprFromString(
        R"(
        (derivation {
            name = "test";
            system = "x86_64-linux";
            builder = "/bin/sh";
        }) // {
            outputSpecified = true;
            outputName = "preferred";
            meta = { outputsToInstall = [ "ignored" ]; };
        }
    )",
        state.rootPath("."));
    auto v = state.allocValue();
    state.eval(expr, *v);
    auto obj = state.toObjectCompat(*v);

    auto outputs = getDerivationOutputs(*obj);

    EXPECT_EQ(outputs.size(), 1);
    EXPECT_TRUE(outputs.count("preferred"));
    EXPECT_FALSE(outputs.count("ignored"));
}

TEST_F(EvaluatorHelpersTest, getDerivationOutputs_OutputSpecifiedFalseUsesMeta)
{
    auto expr = state.parseExprFromString(
        R"(
        (derivation {
            name = "test";
            system = "x86_64-linux";
            builder = "/bin/sh";
        }) // {
            outputSpecified = false;
            meta = { outputsToInstall = [ "ignored" ]; };
        }
    )",
        state.rootPath("."));
    auto v = state.allocValue();
    state.eval(expr, *v);
    auto obj = state.toObjectCompat(*v);

    auto outputs = getDerivationOutputs(*obj);

    // outputSpecified=false blocks meta check due to else-if, defaults to "out"
    EXPECT_EQ(outputs.size(), 1);
    EXPECT_TRUE(outputs.count("out"));
}

// Tests for findAlongAttrPath helper
TEST_F(EvaluatorHelpersTest, findAlongAttrPath_EmptyPath)
{
    auto v = makeAttrs({{"foo", "bar"}});
    auto obj = state.toObjectCompat(*v);

    auto result = findAlongAttrPath(*obj, {});

    EXPECT_TRUE(result);
    EXPECT_EQ(result->get(), &*obj);
}

TEST_F(EvaluatorHelpersTest, findAlongAttrPath_SingleAttribute)
{
    auto v = state.allocValue();
    auto bindings = state.buildBindings(1);
    auto vNested = state.allocValue();
    vNested->mkString("value", state.mem);
    bindings.insert(state.symbols.create("foo"), vNested);
    v->mkAttrs(bindings.finish());

    auto obj = state.toObjectCompat(*v);

    auto result = findAlongAttrPath(*obj, {"foo"});

    ASSERT_TRUE(result);
    EXPECT_EQ((*result)->getStringIgnoreContext(), "value");
}

TEST_F(EvaluatorHelpersTest, findAlongAttrPath_NestedAttributes)
{
    auto expr = state.parseExprFromString("{ a = { b = { c = \"deep\"; }; }; }", state.rootPath("."));
    auto v = state.allocValue();
    state.eval(expr, *v);
    auto obj = state.toObjectCompat(*v);

    auto result = findAlongAttrPath(*obj, {"a", "b", "c"});

    ASSERT_TRUE(result);
    EXPECT_EQ((*result)->getStringIgnoreContext(), "deep");
}

TEST_F(EvaluatorHelpersTest, findAlongAttrPath_AttributeNotFound)
{
    auto v = makeAttrs({{"foo", "bar"}});
    auto obj = state.toObjectCompat(*v);

    auto result = findAlongAttrPath(*obj, {"missing"});

    EXPECT_FALSE(result);
}

TEST_F(EvaluatorHelpersTest, findAlongAttrPath_MidPathNotFound)
{
    auto expr = state.parseExprFromString("{ a = { b = \"value\"; }; }", state.rootPath("."));
    auto v = state.allocValue();
    state.eval(expr, *v);
    auto obj = state.toObjectCompat(*v);

    auto result = findAlongAttrPath(*obj, {"a", "missing", "c"});

    EXPECT_FALSE(result);
}

TEST_F(EvaluatorHelpersTest, findAlongAttrPath_NotAnAttrSet)
{
    auto v = makeString("not an attrset");
    auto obj = state.toObjectCompat(*v);

    EXPECT_THROW(findAlongAttrPath(*obj, {"foo"}), Error);
}

TEST_F(EvaluatorHelpersTest, findAlongAttrPath_MidPathNotAnAttrSet)
{
    auto expr = state.parseExprFromString("{ a = \"string\"; }", state.rootPath("."));
    auto v = state.allocValue();
    state.eval(expr, *v);
    auto obj = state.toObjectCompat(*v);

    EXPECT_THROW(findAlongAttrPath(*obj, {"a", "b"}), Error);
}

TEST_F(EvaluatorHelpersTest, findAlongAttrPath_SuggestsCloseMatch)
{
    auto expr = state.parseExprFromString("{ foo = \"value\"; bar = \"other\"; }", state.rootPath("."));
    auto v = state.allocValue();
    state.eval(expr, *v);
    auto obj = state.toObjectCompat(*v);

    auto result = findAlongAttrPath(*obj, {"fo"});

    EXPECT_FALSE(result);
    auto suggestions = result.getSuggestions();
    EXPECT_FALSE(suggestions.suggestions.empty());

    bool foundFoo = false;
    for (const auto & suggestion : suggestions.suggestions) {
        if (suggestion.suggestion == "foo") {
            foundFoo = true;
            break;
        }
    }
    EXPECT_TRUE(foundFoo);
}

TEST_F(EvaluatorHelpersTest, findAlongAttrPath_SuggestsForNestedTypo)
{
    auto expr =
        state.parseExprFromString("{ a = { b = { baz = \"value\"; bar = \"other\"; }; }; }", state.rootPath("."));
    auto v = state.allocValue();
    state.eval(expr, *v);
    auto obj = state.toObjectCompat(*v);

    auto result = findAlongAttrPath(*obj, {"a", "b", "bz"});

    EXPECT_FALSE(result);
    auto suggestions = result.getSuggestions();
    EXPECT_FALSE(suggestions.suggestions.empty());

    bool foundMatch = false;
    for (const auto & suggestion : suggestions.suggestions) {
        if (suggestion.suggestion == "baz" || suggestion.suggestion == "bar") {
            foundMatch = true;
            break;
        }
    }
    EXPECT_TRUE(foundMatch);
}

// Tests for tryAttrPaths helper
TEST_F(EvaluatorHelpersTest, tryAttrPaths_FindsFirstPath)
{
    auto expr = state.parseExprFromString("{ a = 1; b = 2; c = 3; }", state.rootPath("."));
    auto v = state.allocValue();
    state.eval(expr, *v);
    auto obj = state.toObjectCompat(*v);

    auto result = tryAttrPaths(*obj, {"a"}, state);
    ASSERT_TRUE(result);

    auto [foundObj, path] = *result;
    EXPECT_EQ(path, "a");
    EXPECT_EQ(foundObj->getInt("").value, 1);
}

TEST_F(EvaluatorHelpersTest, tryAttrPaths_TriesMultiplePaths)
{
    auto expr = state.parseExprFromString("{ a = { b = 42; }; c = 99; }", state.rootPath("."));
    auto v = state.allocValue();
    state.eval(expr, *v);
    auto obj = state.toObjectCompat(*v);

    auto result = tryAttrPaths(*obj, {"x.y", "a.b"}, state);
    ASSERT_TRUE(result);

    auto [foundObj, path] = *result;
    EXPECT_EQ(path, "a.b");
    EXPECT_EQ(foundObj->getInt("").value, 42);
}

TEST_F(EvaluatorHelpersTest, tryAttrPaths_ReturnsFirstSuccess)
{
    auto expr = state.parseExprFromString("{ a = 1; b = 2; }", state.rootPath("."));
    auto v = state.allocValue();
    state.eval(expr, *v);
    auto obj = state.toObjectCompat(*v);

    auto result = tryAttrPaths(*obj, {"a", "b"}, state);
    ASSERT_TRUE(result);

    auto [foundObj, path] = *result;
    EXPECT_EQ(path, "a");
    EXPECT_EQ(foundObj->getInt("").value, 1);
}

TEST_F(EvaluatorHelpersTest, tryAttrPaths_FailsWhenNoneFound)
{
    auto expr = state.parseExprFromString("{ a = 1; }", state.rootPath("."));
    auto v = state.allocValue();
    state.eval(expr, *v);
    auto obj = state.toObjectCompat(*v);

    auto result = tryAttrPaths(*obj, {"x", "y", "z"}, state);
    EXPECT_FALSE(result);
}

TEST_F(EvaluatorHelpersTest, tryAttrPaths_AccumulatesSuggestions)
{
    auto expr = state.parseExprFromString("{ abc = 1; abd = 2; }", state.rootPath("."));
    auto v = state.allocValue();
    state.eval(expr, *v);
    auto obj = state.toObjectCompat(*v);

    auto result = tryAttrPaths(*obj, {"abx", "aby"}, state);
    ASSERT_FALSE(result);

    auto suggestions = result.getSuggestions();
    EXPECT_GT(suggestions.suggestions.size(), 0);
}

TEST_F(EvaluatorHelpersTest, tryAttrPaths_HandlesNestedPaths)
{
    auto expr = state.parseExprFromString("{ a = { b = { c = 123; }; }; }", state.rootPath("."));
    auto v = state.allocValue();
    state.eval(expr, *v);
    auto obj = state.toObjectCompat(*v);

    auto result = tryAttrPaths(*obj, {"a.b.c"}, state);
    ASSERT_TRUE(result);

    auto [foundObj, path] = *result;
    EXPECT_EQ(path, "a.b.c");
    EXPECT_EQ(foundObj->getInt("").value, 123);
}

TEST_F(EvaluatorHelpersTest, tryAttrPaths_EmptyPathList)
{
    auto expr = state.parseExprFromString("{ a = 1; }", state.rootPath("."));
    auto v = state.allocValue();
    state.eval(expr, *v);
    auto obj = state.toObjectCompat(*v);

    auto result = tryAttrPaths(*obj, {}, state);
    EXPECT_FALSE(result);
}

} // namespace nix::expr::helpers