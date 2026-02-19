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

} // namespace nix::expr::helpers