#include <gtest/gtest.h>

#include "nix/expr/contra-object.hh"

namespace nix {

/**
 * Mock resolver: maps queries to predetermined results.
 */
static ContraQueryFn mockResolver(std::map<std::string, trace::ResultVariant> responses)
{
    return [responses = std::move(responses)](const trace::QueryVariant & q) -> trace::ResultVariant {
        // Extract the 'from' or id from the query
        std::string key = std::visit(
            [](const auto & query) -> std::string {
                if constexpr (requires { query.from; })
                    return std::string(query.tag) + ":" + query.from;
                else
                    return std::string(query.tag);
            },
            q);
        auto it = responses.find(key);
        if (it == responses.end())
            throw Error("mock resolver: no response for %s", key);
        return it->second;
    };
}

TEST(ContraObjectTest, GetType)
{
    auto obj = std::make_shared<ContraObject>(
        "0", mockResolver({{"getType:0", trace::ResultType{"int"}}}));
    EXPECT_EQ(obj->getType(), nInt);
}

TEST(ContraObjectTest, GetInt)
{
    auto obj = std::make_shared<ContraObject>(
        "0", mockResolver({{"getInt:0", trace::ResultInt{42}}}));
    EXPECT_EQ(obj->getInt().value, 42);
}

TEST(ContraObjectTest, GetString)
{
    auto obj = std::make_shared<ContraObject>(
        "0", mockResolver({{"getString:0", trace::ResultString{"hello"}}}));
    EXPECT_EQ(obj->getStringIgnoreContext(), "hello");
}

TEST(ContraObjectTest, GetBool)
{
    auto obj = std::make_shared<ContraObject>(
        "0", mockResolver({{"getBool:0", trace::ResultBool{true}}}));
    EXPECT_TRUE(obj->getBool());
}

TEST(ContraObjectTest, GetAttrReturnsChild)
{
    auto obj = std::make_shared<ContraObject>(
        "0",
        mockResolver({
            {"getAttr:0", trace::ResultMaybeType{std::optional<std::string>{"int"}}},
            {"getInt:0.x", trace::ResultInt{99}},
        }));
    auto child = obj->maybeGetAttr("x");
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->getInt().value, 99);
}

TEST(ContraObjectTest, GetAttrMissing)
{
    auto obj = std::make_shared<ContraObject>(
        "0", mockResolver({{"getAttr:0", trace::ResultMaybeType{std::nullopt}}}));
    EXPECT_EQ(obj->maybeGetAttr("missing"), nullptr);
}

TEST(ContraObjectTest, GetListElem)
{
    auto obj = std::make_shared<ContraObject>(
        "0",
        mockResolver({
            {"getListElem:0", trace::ResultType{"string"}},
            {"getString:0[1]", trace::ResultString{"world"}},
        }));
    auto child = obj->getListElem(1);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->getStringIgnoreContext(), "world");
}

TEST(ContraObjectTest, GetAttrNames)
{
    auto obj = std::make_shared<ContraObject>(
        "0", mockResolver({{"getAttrNames:0", trace::ResultListOfStrings{{"a", "b", "c"}}}}));
    auto names = obj->getAttrNames();
    EXPECT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "a");
}

} // namespace nix
