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

    /** Evaluate a Nix expression and return an Object. */
    std::shared_ptr<Object> evalToObject(const std::string & expr)
    {
        auto e = state->parseExprFromString(expr, state->rootedPath(CanonPath::root));
        auto v = state->allocValue();
        state->eval(e, *v);
        return std::make_shared<InterpreterObject>(*state, allocRootValue(v));
    }

    /** Wrap an Object in ExprFromObject and evaluate it to get a Value. */
    Value * evalFromObject(std::shared_ptr<Object> obj)
    {
        auto expr = new ExprFromObject(std::move(obj));
        auto v = state->allocValue();
        expr->eval(*state, state->baseEnv, *v);
        return v;
    }
};

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

TEST_F(ExprFromObjectTest, Null)
{
    auto obj = evalToObject("null");
    auto v = evalFromObject(obj);
    EXPECT_EQ(v->type(), nNull);
}

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
    EXPECT_NE(v->context(), nullptr);
}

TEST_F(ExprFromObjectTest, Path)
{
    auto obj = evalToObject("/some/path");
    auto v = evalFromObject(obj);
    EXPECT_EQ(v->type(), nPath);
    EXPECT_EQ(v->path().path.abs(), "/some/path");
}

TEST_F(ExprFromObjectTest, Float)
{
    auto obj = evalToObject("3.14");
    auto v = evalFromObject(obj);
    EXPECT_EQ(v->type(), nFloat);
    EXPECT_DOUBLE_EQ(v->fpoint(), 3.14);
}

TEST_F(ExprFromObjectTest, HeterogeneousList)
{
    auto obj = evalToObject("[1 \"two\" true]");
    auto v = evalFromObject(obj);
    EXPECT_EQ(v->type(), nList);
    EXPECT_EQ(v->listSize(), 3);

    auto list = v->listView();
    state->forceValue(*list[0], noPos);
    state->forceValue(*list[1], noPos);
    state->forceValue(*list[2], noPos);

    EXPECT_EQ(list[0]->type(), nInt);
    EXPECT_EQ(list[0]->integer().value, 1);
    EXPECT_EQ(list[1]->type(), nString);
    EXPECT_STREQ(list[1]->c_str(), "two");
    EXPECT_EQ(list[2]->type(), nBool);
    EXPECT_TRUE(list[2]->boolean());
}

TEST_F(ExprFromObjectTest, EmptyList)
{
    auto obj = evalToObject("[]");
    auto v = evalFromObject(obj);
    EXPECT_EQ(v->type(), nList);
    EXPECT_EQ(v->listSize(), 0);
}

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

TEST_F(ExprFromObjectTest, AttrsetLaziness)
{
    auto obj = evalToObject("{ a = 1; b = 2; c = 3; }");
    auto v = evalFromObject(obj);
    EXPECT_EQ(v->type(), nAttrs);

    auto attrA = v->attrs()->get(state->symbols.create("a"));
    ASSERT_NE(attrA, nullptr);
    EXPECT_EQ(attrA->value->type(), nThunk);

    state->forceValue(*attrA->value, noPos);
    EXPECT_EQ(attrA->value->type(), nInt);
    EXPECT_EQ(attrA->value->integer().value, 1);
}

TEST_F(ExprFromObjectTest, Show)
{
    auto obj = evalToObject("42");
    auto expr = ExprFromObject(obj);
    std::ostringstream oss;
    expr.show(state->symbols, oss);
    EXPECT_EQ(oss.str(), "<proxy>");
}

TEST_F(ExprFromObjectTest, FunctionValueIsApplicable)
{
    /* Bridged function Value can be applied and produces the right
       result. Structure (primop vs raw lambda) is intentionally
       unspecified — types that need boundary routing override
       `materialiseAsFunctionValue`; the raw-lambda base default
       applies via mkApp directly. */
    auto obj = evalToObject("x: x + 1");
    auto * fnV = evalFromObject(obj);
    auto * argV = state->allocValue();
    argV->mkInt(41);
    auto * resultV = state->allocValue();
    resultV->mkApp(fnV, argV);
    state->forceValue(*resultV, noPos);
    EXPECT_EQ(resultV->type(), nInt);
    EXPECT_EQ(resultV->integer().value, 42);
}

TEST_F(ExprFromObjectTest, FunctionFormalsExposedViaObjectInterface)
{
    /* Bridged function's formals are accessible via the Object
       interface — `obj->getFunctionInfo()` handles both raw-lambda
       and primop wrappings (see `InterpreterObject::getFunctionInfo`),
       so downstream callers see the same formals regardless of
       whether the boundary crossing wrapped in a primop or not. */
    auto obj = evalToObject("{ a, b ? 1 }: a + b");
    /* Force materialisation through ExprFromObject before probing
       formals; the round-trip is what real callers see. */
    (void) evalFromObject(obj);
    auto info = obj->getFunctionInfo();
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->formals.size(), 2u);
    EXPECT_FALSE(info->formals.at("a"));
    EXPECT_TRUE(info->formals.at("b"));
}

} // namespace nix
