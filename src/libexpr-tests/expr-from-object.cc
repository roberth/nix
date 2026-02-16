#include <gtest/gtest.h>
#include <memory>

#include "nix/expr/expr-from-object.hh"
#include "nix/expr/interpreter-object.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/store/tests/libstore.hh"

namespace nix {

/**
 * Test fixture for ExprFromObject tests.
 */
class ExprFromObjectTest : public LibStoreTest
{
protected:
    std::shared_ptr<EvalState> state;
    bool readOnlyMode = false;
    fetchers::Settings fetchSettings{};
    EvalSettings evalSettings{readOnlyMode};

    static void SetUpTestSuite()
    {
        LibStoreTest::SetUpTestSuite();
        initGC();
    }

    ExprFromObjectTest()
        : LibStoreTest(openStore("dummy://?read-only=false"))
    {
    }

    void SetUp() override
    {
        state = std::make_shared<EvalState>(LookupPath{}, store, fetchSettings, evalSettings, nullptr);
    }

    /**
     * Evaluate a Nix expression and return an Object.
     */
    std::shared_ptr<Object> evalToObject(const std::string & expr)
    {
        auto e = state->parseExprFromString(expr, state->rootPath(CanonPath::root));
        auto v = state->allocValue();
        state->eval(e, *v);
        return std::make_shared<InterpreterObject>(*state, allocRootValue(v));
    }

    /**
     * Wrap an Object in ExprFromObject and evaluate it to get a Value.
     */
    Value * evalFromObject(std::shared_ptr<Object> obj)
    {
        auto expr = new ExprFromObject(std::move(obj));
        auto v = state->allocValue();
        expr->eval(*state, state->baseEnv, *v);
        return v;
    }
};

// Test integer conversion
TEST_F(ExprFromObjectTest, Int)
{
    auto obj = evalToObject("42");
    auto v = evalFromObject(obj);
    EXPECT_EQ(v->type(), nInt);
    EXPECT_EQ(v->integer().value, 42);
}

TEST_F(ExprFromObjectTest, NegativeInt)
{
    auto obj = evalToObject("-123");
    auto v = evalFromObject(obj);
    EXPECT_EQ(v->type(), nInt);
    EXPECT_EQ(v->integer().value, -123);
}

// Test boolean conversion
TEST_F(ExprFromObjectTest, BoolTrue)
{
    auto obj = evalToObject("true");
    auto v = evalFromObject(obj);
    EXPECT_EQ(v->type(), nBool);
    EXPECT_TRUE(v->boolean());
}

TEST_F(ExprFromObjectTest, BoolFalse)
{
    auto obj = evalToObject("false");
    auto v = evalFromObject(obj);
    EXPECT_EQ(v->type(), nBool);
    EXPECT_FALSE(v->boolean());
}

// Test null conversion
TEST_F(ExprFromObjectTest, Null)
{
    auto obj = evalToObject("null");
    auto v = evalFromObject(obj);
    EXPECT_EQ(v->type(), nNull);
}

// Test string conversion
TEST_F(ExprFromObjectTest, String)
{
    auto obj = evalToObject("\"hello world\"");
    auto v = evalFromObject(obj);
    EXPECT_EQ(v->type(), nString);
    EXPECT_STREQ(v->c_str(), "hello world");
}

TEST_F(ExprFromObjectTest, StringWithContext)
{
    auto obj = evalToObject(R"(
        let drv = derivation { name = "test"; system = "x86_64-linux"; builder = "/bin/sh"; };
        in "${drv}"
    )");
    auto v = evalFromObject(obj);
    EXPECT_EQ(v->type(), nString);
    // Should have context
    EXPECT_NE(v->context(), nullptr);
}

// Test path conversion
TEST_F(ExprFromObjectTest, Path)
{
    auto obj = evalToObject("/some/path");
    auto v = evalFromObject(obj);
    EXPECT_EQ(v->type(), nPath);
    EXPECT_EQ(v->path().path.abs(), "/some/path");
}

// Test list conversion (string list)
TEST_F(ExprFromObjectTest, ListOfStrings)
{
    auto obj = evalToObject("[\"foo\" \"bar\" \"baz\"]");
    auto v = evalFromObject(obj);
    EXPECT_EQ(v->type(), nList);
    EXPECT_EQ(v->listSize(), 3);

    auto list = v->listView();
    state->forceValue(*list[0], noPos);
    state->forceValue(*list[1], noPos);
    state->forceValue(*list[2], noPos);

    EXPECT_STREQ(list[0]->c_str(), "foo");
    EXPECT_STREQ(list[1]->c_str(), "bar");
    EXPECT_STREQ(list[2]->c_str(), "baz");
}

TEST_F(ExprFromObjectTest, EmptyList)
{
    auto obj = evalToObject("[]");
    auto v = evalFromObject(obj);
    EXPECT_EQ(v->type(), nList);
    EXPECT_EQ(v->listSize(), 0);
}

// Test attrset conversion
TEST_F(ExprFromObjectTest, Attrset)
{
    auto obj = evalToObject("{ foo = \"bar\"; }");
    auto v = evalFromObject(obj);
    EXPECT_EQ(v->type(), nAttrs);

    auto attr = v->attrs()->get(state->symbols.create("foo"));
    ASSERT_NE(attr, nullptr);

    state->forceValue(*attr->value, noPos);
    EXPECT_EQ(attr->value->type(), nString);
    EXPECT_STREQ(attr->value->c_str(), "bar");
}

TEST_F(ExprFromObjectTest, EmptyAttrset)
{
    auto obj = evalToObject("{ }");
    auto v = evalFromObject(obj);
    EXPECT_EQ(v->type(), nAttrs);
    EXPECT_EQ(v->attrs()->size(), 0);
}

TEST_F(ExprFromObjectTest, NestedAttrset)
{
    auto obj = evalToObject("{ outer = { inner = 42; }; }");
    auto v = evalFromObject(obj);
    EXPECT_EQ(v->type(), nAttrs);

    auto outer = v->attrs()->get(state->symbols.create("outer"));
    ASSERT_NE(outer, nullptr);

    state->forceValue(*outer->value, noPos);
    EXPECT_EQ(outer->value->type(), nAttrs);

    auto inner = outer->value->attrs()->get(state->symbols.create("inner"));
    ASSERT_NE(inner, nullptr);

    state->forceValue(*inner->value, noPos);
    EXPECT_EQ(inner->value->type(), nInt);
    EXPECT_EQ(inner->value->integer().value, 42);
}

// Test lazy evaluation of attrset attributes
TEST_F(ExprFromObjectTest, AttrsetLaziness)
{
    auto obj = evalToObject("{ a = 1; b = 2; c = 3; }");
    auto v = evalFromObject(obj);
    EXPECT_EQ(v->type(), nAttrs);

    // Attributes should be thunks initially
    auto attrA = v->attrs()->get(state->symbols.create("a"));
    ASSERT_NE(attrA, nullptr);
    EXPECT_EQ(attrA->value->type(), nThunk);

    // Force and check
    state->forceValue(*attrA->value, noPos);
    EXPECT_EQ(attrA->value->type(), nInt);
    EXPECT_EQ(attrA->value->integer().value, 1);
}

// Test ExprProxy::show
TEST_F(ExprFromObjectTest, Show)
{
    auto obj = evalToObject("42");
    auto expr = ExprFromObject(obj);
    std::ostringstream oss;
    expr.show(state->symbols, oss);
    EXPECT_EQ(oss.str(), "<proxy>");
}

// Test that functions throw an error
TEST_F(ExprFromObjectTest, FunctionThrows)
{
    auto obj = evalToObject("x: x + 1");
    EXPECT_THROW(evalFromObject(obj), Error);
}

} // namespace nix
