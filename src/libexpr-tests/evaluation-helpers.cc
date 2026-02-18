#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "nix/expr/evaluation-helpers.hh"
#include "nix/expr/interpreter.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/util/tests/gmock-matchers.hh"

namespace nix::expr::helpers {

using ObjectAttrMap = std::map<std::string, ref<Object>>;

class EvaluatorHelpersTest : public LibExprTest
{
protected:
    EvaluatorHelpersTest()
        : LibExprTest()
        , evaluator(statePtr)
    {
    }

    Interpreter evaluator;

    ref<Object> evalExpression(const std::string & expr)
    {
        auto e = state.parseExprFromString(expr, state.rootPath(CanonPath::root));
        auto v = state.allocValue();
        state.eval(e, *v);
        return state.toObjectCompat(*v);
    }

    ref<Object> evalExpressionLazy(const std::string & expr)
    {
        auto e = state.parseExprFromString(expr, state.rootPath(CanonPath::root));
        auto v = state.allocValue();
        state.mkThunk_(*v, e);
        return state.toObjectCompat(*v);
    }

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

// Tests for autoApply/autoCall helpers
TEST_F(EvaluatorHelpersTest, autoApply_NonFunction)
{
    // Non-function values pass through unchanged
    auto obj = evalExpression("42");
    ObjectAttrMap args;
    auto result = autoApply(evaluator, ref<Object>(obj), args);
    EXPECT_EQ(result->getType(), nInt);
    EXPECT_EQ(result->getInt(), NixInt(42));
}

TEST_F(EvaluatorHelpersTest, autoApply_SimpleLambda)
{
    // Simple lambdas (no formals) pass through unchanged
    auto obj = evalExpression("x: x + 1");
    ObjectAttrMap args;
    auto result = autoApply(evaluator, ref<Object>(obj), args);
    EXPECT_EQ(result->getType(), nFunction);
}

TEST_F(EvaluatorHelpersTest, autoApply_WithFormals)
{
    auto obj = evalExpression("{ a, b }: a + b");
    ObjectAttrMap args;
    args.insert_or_assign("a", evaluator.mkString("hello"));
    args.insert_or_assign("b", evaluator.mkString(" world"));
    auto result = autoApply(evaluator, ref<Object>(obj), args);
    EXPECT_EQ(result->getType(), nString);
    EXPECT_EQ(result->getStringIgnoreContext(), "hello world");
}

TEST_F(EvaluatorHelpersTest, autoApply_WithEllipsis)
{
    auto obj = evalExpression("{ a, ... }: a");
    ObjectAttrMap args;
    args.insert_or_assign("a", evaluator.mkString("value"));
    args.insert_or_assign("extra", evaluator.mkString("also passed"));
    auto result = autoApply(evaluator, ref<Object>(obj), args);
    EXPECT_EQ(result->getType(), nString);
    EXPECT_EQ(result->getStringIgnoreContext(), "value");
}

TEST_F(EvaluatorHelpersTest, autoApply_OnlyMatchingArgs)
{
    auto obj = evalExpression("{ a }: a");
    ObjectAttrMap args;
    args.insert_or_assign("a", evaluator.mkString("value"));
    args.insert_or_assign("extra", evaluator.mkString("not passed"));
    auto result = autoApply(evaluator, ref<Object>(obj), args);
    EXPECT_EQ(result->getType(), nString);
    EXPECT_EQ(result->getStringIgnoreContext(), "value");
}

TEST_F(EvaluatorHelpersTest, autoApply_DefaultArgs)
{
    auto obj = evalExpression("{ a, b ? \"default\" }: a + b");
    ObjectAttrMap args;
    args.insert_or_assign("a", evaluator.mkString("hello"));
    auto result = autoApply(evaluator, ref<Object>(obj), args);
    EXPECT_EQ(result->getType(), nString);
    EXPECT_EQ(result->getStringIgnoreContext(), "hellodefault");
}

TEST_F(EvaluatorHelpersTest, autoApply_Functor)
{
    // __functor is applied with self, returning the inner function
    auto obj = evalExpression("{ __functor = self: x: self.value + x; value = 10; }");
    ObjectAttrMap args;
    auto result = autoApply(evaluator, ref<Object>(obj), args);
    EXPECT_EQ(result->getType(), nFunction);
}

TEST_F(EvaluatorHelpersTest, autoApply_ResultIsLazy)
{
    // autoApply returns a thunk - not forced until getType()
    auto obj = evalExpression("{ a }: throw \"should not be called yet\"");
    ObjectAttrMap args;
    args.insert_or_assign("a", evaluator.mkString("value"));
    auto result = autoApply(evaluator, ref<Object>(obj), args);
    EXPECT_EQ(result->getTypeLazy(), nThunk);
    EXPECT_THROW(result->getType(), Error);
}

TEST_F(EvaluatorHelpersTest, autoCall_ResultIsEager)
{
    auto obj = evalExpression("{ a }: throw \"called immediately\"");
    ObjectAttrMap args;
    args.insert_or_assign("a", evaluator.mkString("value"));
    EXPECT_THROW(autoCall(evaluator, ref<Object>(obj), args), Error);
}

TEST_F(EvaluatorHelpersTest, autoCall_IsLazy)
{
    // Arguments should not be evaluated if not used
    // nix-instantiate --eval --arg x 'throw "a"' --expr '{ ... }: "hi"' => "hi"
    auto obj = evalExpression("{ ... }: \"hi\"");
    ObjectAttrMap args;
    // Use evalExpressionLazy to create a thunk that isn't forced yet
    args.insert_or_assign("x", evalExpressionLazy("throw \"unused arg\""));
    // This should NOT throw - the arg is not used
    auto result = expr::helpers::autoCall(evaluator, ref<Object>(obj), args);
    EXPECT_EQ(result->getType(), nString);
    EXPECT_EQ(result->getStringIgnoreContext(), "hi");
}

TEST_F(EvaluatorHelpersTest, autoCall_ParsingIsStrict)
{
    // Parsing is strict - syntax errors throw immediately
    // nix-instantiate --eval --arg x 'throw "' --expr '{ ... }: "hi"' => syntax error
    // Syntax error in argument should throw during parsing, not evaluation
    EXPECT_THROW(evalExpressionLazy("throw \""), ParseError);
}

TEST_F(EvaluatorHelpersTest, autoApply_MissingArg)
{
    auto obj = evalExpression("{ a }: a");
    ObjectAttrMap args;
    EXPECT_THROW(autoApply(evaluator, ref<Object>(obj), args), Error);
}

TEST_F(EvaluatorHelpersTest, autoApply_MissingArgWithSomeProvided)
{
    auto obj = evalExpression("{ a, b, c ? 3 }: a + b + c");
    ObjectAttrMap args;
    args.insert_or_assign("a", ref<Object>(evalExpression("1")));
    EXPECT_THROW(autoApply(evaluator, ref<Object>(obj), args), Error);
}

TEST_F(EvaluatorHelpersTest, autoCall_ReturnsAttrset)
{
    // Verify autoCall correctly handles a function that returns an attrset
    auto obj = evalExpression("{ x }: { nested = x.value; }");
    ObjectAttrMap valueAttrs;
    valueAttrs.insert_or_assign("value", evaluator.mkString("hello"));
    auto argValue = evaluator.mkAttrs(valueAttrs);
    ObjectAttrMap args;
    args.insert_or_assign("x", argValue);
    auto result = expr::helpers::autoCall(evaluator, ref<Object>(obj), args);
    // Should return attrset { nested = "hello"; }
    EXPECT_EQ(result->getType(), nAttrs);
    auto nested = result->maybeGetAttr("nested");
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ(nested->getStringIgnoreContext(), "hello");
}

// Tests for findAlongAttrPathWithAutoCall

TEST_F(EvaluatorHelpersTest, findAlongAttrPathWithAutoCall_Simple)
{
    auto obj = evalExpression("{ foo = { bar = 42; }; }");
    ObjectAttrMap args;
    auto result =
        expr::helpers::findAlongAttrPathWithAutoCall(evaluator, ref<Object>(obj), "foo.bar", {"foo", "bar"}, args);
    ASSERT_TRUE(result);
    EXPECT_EQ((*result)->getInt(), NixInt(42));
}

TEST_F(EvaluatorHelpersTest, findAlongAttrPathWithAutoCall_WithAutoCall)
{
    // Auto-call at each step: { x }: { nested = x.value; } with arg x={value="hello"}
    // Uses formals so autoCall will call the function
    auto obj = evalExpression("{ x }: { nested = x.value; }");
    ObjectAttrMap valueAttrs;
    valueAttrs.insert_or_assign("value", evaluator.mkString("hello"));
    auto argValue = evaluator.mkAttrs(valueAttrs);
    ObjectAttrMap args;
    args.insert_or_assign("x", argValue);
    auto result = expr::helpers::findAlongAttrPathWithAutoCall(evaluator, ref<Object>(obj), "nested", {"nested"}, args);
    ASSERT_TRUE(result);
    EXPECT_EQ((*result)->getStringIgnoreContext(), "hello");
}

TEST_F(EvaluatorHelpersTest, findAlongAttrPathWithAutoCall_NestedAutoCall)
{
    // Nested auto-call: function returns attrset with value computed from another arg
    // Navigate through multiple levels with auto-calling at each step
    auto obj = evalExpression("{ x }: { inner = { y }: { result = y.foo; }; }");
    ObjectAttrMap emptyAttrs;
    auto xArg = evaluator.mkAttrs(emptyAttrs);
    ObjectAttrMap yAttrs;
    yAttrs.insert_or_assign("foo", evaluator.mkString("nested"));
    auto yArg = evaluator.mkAttrs(yAttrs);
    ObjectAttrMap args;
    args.insert_or_assign("x", xArg);
    args.insert_or_assign("y", yArg);
    // Navigate to inner.result:
    // 1. Auto-call { x }: ... with args -> { inner = { y }: { result = y.foo; }; }
    // 2. Get inner -> { y }: { result = y.foo; }
    // 3. Auto-call { y }: ... with args -> { result = "nested"; }
    // 4. Get result -> "nested"
    auto result = expr::helpers::findAlongAttrPathWithAutoCall(
        evaluator, ref<Object>(obj), "inner.result", {"inner", "result"}, args);
    ASSERT_TRUE(result);
    EXPECT_EQ((*result)->getStringIgnoreContext(), "nested");
}

TEST_F(EvaluatorHelpersTest, findAlongAttrPathWithAutoCall_MissingAttr)
{
    auto obj = evalExpression("{ foo = 1; }");
    ObjectAttrMap args;
    auto result = expr::helpers::findAlongAttrPathWithAutoCall(evaluator, ref<Object>(obj), "bar", {"bar"}, args);
    EXPECT_FALSE(result);
    // Should have suggestions
    auto suggestions = result.getSuggestions();
    EXPECT_FALSE(suggestions.suggestions.empty());
}

TEST_F(EvaluatorHelpersTest, findAlongAttrPathWithAutoCall_ListIndex)
{
    auto obj = evalExpression("{ items = [ \"a\" \"b\" \"c\" ]; }");
    ObjectAttrMap args;
    auto result =
        expr::helpers::findAlongAttrPathWithAutoCall(evaluator, ref<Object>(obj), "items.1", {"items", "1"}, args);
    ASSERT_TRUE(result);
    EXPECT_EQ((*result)->getStringIgnoreContext(), "b");
}

TEST_F(EvaluatorHelpersTest, findAlongAttrPathWithAutoCall_TypeErrorNotSet)
{
    // Trying to navigate into a string should throw type error
    auto obj = evalExpression("\"not a set\"");
    ObjectAttrMap args;
    EXPECT_THROW(expr::helpers::findAlongAttrPathWithAutoCall(evaluator, ref<Object>(obj), "foo", {"foo"}, args), Error)
        << "Should throw when navigating into non-attrset";
}

TEST_F(EvaluatorHelpersTest, findAlongAttrPathWithAutoCall_TypeErrorNotList)
{
    // Trying to index into an attrset should throw type error
    auto obj = evalExpression("{ foo = 1; }");
    ObjectAttrMap args;
    EXPECT_THROW(
        expr::helpers::findAlongAttrPathWithAutoCall(evaluator, ref<Object>(obj), "foo.0", {"foo", "0"}, args), Error)
        << "Should throw when indexing non-list";
}

TEST_F(EvaluatorHelpersTest, findAlongAttrPathWithAutoCall_ListIndexOutOfRange)
{
    auto obj = evalExpression("{ items = [ \"a\" \"b\" ]; }");
    ObjectAttrMap args;
    EXPECT_THROW(
        expr::helpers::findAlongAttrPathWithAutoCall(evaluator, ref<Object>(obj), "items.5", {"items", "5"}, args),
        Error)
        << "Should throw when list index is out of range";
}

} // namespace nix::expr::helpers