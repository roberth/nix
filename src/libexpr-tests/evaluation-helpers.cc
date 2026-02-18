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

} // namespace nix::expr::helpers