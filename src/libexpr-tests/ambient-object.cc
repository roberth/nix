#include <gtest/gtest.h>

#include "nix/expr/ambient-object.hh"

namespace nix {

/**
 * Mock resolver: maps (objectId, tag) to predetermined results.
 * Returns childId = objectId * 100 + 1 for getAttr/getListElem queries.
 */
static AmbientQueryFn mockResolver(std::map<std::string, trace::ResultVariant> responses)
{
    return [responses = std::move(responses)](AmbientId objectId, const trace::QueryVariant & q) -> AmbientQueryResult {
        std::string key = std::visit(
            [&](const auto & query) -> std::string {
                return std::string(query.tag) + ":" + std::to_string(objectId.value());
            },
            q);
        auto it = responses.find(key);
        if (it == responses.end())
            throw Error("mock resolver: no response for %s", key);

        // For queries that produce children, return a child id
        std::optional<AmbientId> childId;
        if (std::holds_alternative<trace::ResultMaybeType>(it->second)) {
            auto & rmt = std::get<trace::ResultMaybeType>(it->second);
            if (rmt.type)
                childId = AmbientId(objectId.value() * 100 + 1);
        }
        if (std::holds_alternative<trace::ResultType>(it->second)) {
            // Could be a getListElem
            childId = AmbientId(objectId.value() * 100 + 1);
        }
        return {it->second, childId};
    };
}

TEST(AmbientObjectTest, GetType)
{
    auto obj = std::make_shared<AmbientObject>(
        AmbientId(0), mockResolver({{"getType:0", trace::ResultType{"int"}}}));
    EXPECT_EQ(obj->getType(), nInt);
}

TEST(AmbientObjectTest, GetInt)
{
    auto obj = std::make_shared<AmbientObject>(
        AmbientId(0), mockResolver({{"getInt:0", trace::ResultInt{42}}}));
    EXPECT_EQ(obj->getInt().value, 42);
}

TEST(AmbientObjectTest, GetString)
{
    auto obj = std::make_shared<AmbientObject>(
        AmbientId(0), mockResolver({{"getString:0", trace::ResultString{"hello"}}}));
    EXPECT_EQ(obj->getStringIgnoreContext(), "hello");
}

TEST(AmbientObjectTest, GetBool)
{
    auto obj = std::make_shared<AmbientObject>(
        AmbientId(0), mockResolver({{"getBool:0", trace::ResultBool{true}}}));
    EXPECT_TRUE(obj->getBool());
}

TEST(AmbientObjectTest, GetAttrReturnsChild)
{
    auto obj = std::make_shared<AmbientObject>(
        AmbientId(0),
        mockResolver({
            {"getAttr:0", trace::ResultMaybeType{std::optional<std::string>{"int"}}},
            {"getInt:1", trace::ResultInt{99}},
        }));
    auto child = obj->maybeGetAttr("x");
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->getInt().value, 99);
}

TEST(AmbientObjectTest, GetAttrMissing)
{
    auto obj = std::make_shared<AmbientObject>(
        AmbientId(0), mockResolver({{"getAttr:0", trace::ResultMaybeType{std::nullopt}}}));
    EXPECT_EQ(obj->maybeGetAttr("missing"), nullptr);
}

TEST(AmbientObjectTest, GetListElem)
{
    auto obj = std::make_shared<AmbientObject>(
        AmbientId(0),
        mockResolver({
            {"getListElem:0", trace::ResultType{"string"}},
            {"getString:1", trace::ResultString{"world"}},
        }));
    auto child = obj->getListElem(1);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->getStringIgnoreContext(), "world");
}

TEST(AmbientObjectTest, GetAttrNames)
{
    auto obj = std::make_shared<AmbientObject>(
        AmbientId(0), mockResolver({{"getAttrNames:0", trace::ResultListOfStrings{{"a", "b", "c"}}}}));
    auto names = obj->getAttrNames();
    EXPECT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "a");
}

} // namespace nix
