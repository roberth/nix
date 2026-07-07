#include <gtest/gtest.h>

#include "nix/expr/outer-object.hh"
#include "nix/util/source-accessor.hh"

namespace nix {

/* lazy-paths: tests don't exercise `getPath`, so any SourceRoot
   suffices for the constructor — just stub one out. */
static ref<SourceRoot> stubAmbientRoot()
{
    return SourceRoot::make(getFSSourceAccessor(), SourceRootKind::Internal);
}

/* Tests use PostulatedIdempotentRead to pin the proxy's content id to a
   stable per-test value. The Subject variant exists for cases like
   apply-result args that don't have a positional/derived form. */
static OuterId testId(int n)
{
    return hashString(HashAlgorithm::SHA256, "test:" + std::to_string(n));
}

static Subject testSubject(int n)
{
    return Subject{PostulatedIdempotentRead{testId(n)}};
}

static std::string ambientHex(OuterId id)
{
    return id.to_string(HashFormat::Base16, false);
}

/**
 * Mock resolver: maps `"tag:ambientHex(objectId)"` strings to predetermined
 * results. For child-producing queries, returns
 * `hashString("child:" + ambientHex(objectId))` as the child id.
 */
static OuterQueryFn mockResolver(std::map<std::string, trace::ResultVariant> responses)
{
    return [responses = std::move(responses)](
               OuterId objectId,
               const trace::QueryVariant & q,
               Subject /*subject*/,
               Hash /*argAncestry*/) -> OuterQueryResult {
        std::string objHex = ambientHex(objectId);
        std::string key = std::visit(
            [&](const auto & query) -> std::string {
                return std::string(query.tag) + ":" + objHex;
            },
            q);
        auto it = responses.find(key);
        if (it == responses.end())
            throw Error("mock resolver: no response for %s", key);

        // For queries that produce children, return a deterministic child id
        std::optional<OuterId> childId;
        if (std::holds_alternative<trace::ResultMaybeType>(it->second)) {
            auto & rmt = std::get<trace::ResultMaybeType>(it->second);
            if (rmt.type)
                childId = hashString(HashAlgorithm::SHA256, "child:" + objHex);
        }
        if (std::holds_alternative<trace::ResultType>(it->second)) {
            // Could be a getListElem
            childId = hashString(HashAlgorithm::SHA256, "child:" + objHex);
        }
        return {it->second, childId};
    };
}

TEST(AmbientObjectTest, GetType)
{
    auto arg = testId(0);
    auto obj = std::make_shared<OuterObject>(
        testSubject(0),
        mockResolver({{"getWHNF:" + ambientHex(arg), trace::ResultWHNF{"int", trace::WHNFInt{42}}}}),
        stubAmbientRoot());
    EXPECT_EQ(obj->getType(), nInt);
}

TEST(AmbientObjectTest, GetInt)
{
    auto arg = testId(0);
    auto obj = std::make_shared<OuterObject>(
        testSubject(0),
        mockResolver({{"getWHNF:" + ambientHex(arg), trace::ResultWHNF{"int", trace::WHNFInt{42}}}}),
        stubAmbientRoot());
    EXPECT_EQ(obj->getInt().value, 42);
}

TEST(AmbientObjectTest, GetString)
{
    auto arg = testId(0);
    auto obj = std::make_shared<OuterObject>(
        testSubject(0),
        mockResolver({{"getWHNF:" + ambientHex(arg), trace::ResultWHNF{"string", trace::WHNFString{"hello", {}}}}}),
        stubAmbientRoot());
    EXPECT_EQ(obj->getStringIgnoreContext(), "hello");
}

TEST(AmbientObjectTest, GetBool)
{
    auto arg = testId(0);
    auto obj = std::make_shared<OuterObject>(
        testSubject(0),
        mockResolver({{"getWHNF:" + ambientHex(arg), trace::ResultWHNF{"bool", trace::WHNFBool{true}}}}),
        stubAmbientRoot());
    EXPECT_TRUE(obj->getBool());
}

TEST(AmbientObjectTest, GetAttrReturnsChild)
{
    auto arg = testId(0);
    /* Child scopeStateId is the producer query's queryHash. With Subject-based
       construction the OuterObject derives this from DerivedSubject
       at construction time. */
    auto childCdi = subjectHashAfter(
        Subject{DerivedSubject{
            .parent = std::make_shared<const Subject>(testSubject(0)),
            .kind = DerivedSubject::Kind::GetAttr,
            .name = "x",
        }},
        Hash(HashAlgorithm::SHA256),
        {});
    auto childHex = ambientHex(childCdi);
    auto obj = std::make_shared<OuterObject>(
        testSubject(0),
        mockResolver({
            {"getAttr:" + ambientHex(arg), trace::ResultMaybeType{std::optional<std::string>{"int"}}},
            {"getWHNF:" + childHex, trace::ResultWHNF{"int", trace::WHNFInt{99}}},
        }),
        stubAmbientRoot());
    auto child = obj->maybeGetAttr("x");
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->getInt().value, 99);
}

TEST(AmbientObjectTest, GetAttrMissing)
{
    auto arg = testId(0);
    auto obj = std::make_shared<OuterObject>(
        testSubject(0), mockResolver({{"getAttr:" + ambientHex(arg), trace::ResultMaybeType{std::nullopt}}}), stubAmbientRoot());
    EXPECT_EQ(obj->maybeGetAttr("missing"), nullptr);
}

TEST(AmbientObjectTest, GetListElem)
{
    auto arg = testId(0);
    auto childCdi = subjectHashAfter(
        Subject{DerivedSubject{
            .parent = std::make_shared<const Subject>(testSubject(0)),
            .kind = DerivedSubject::Kind::GetListElem,
            .index = 1,
        }},
        Hash(HashAlgorithm::SHA256),
        {});
    auto childHex = ambientHex(childCdi);
    auto obj = std::make_shared<OuterObject>(
        testSubject(0),
        mockResolver({
            {"getListElem:" + ambientHex(arg), trace::ResultType{"string"}},
            {"getWHNF:" + childHex, trace::ResultWHNF{"string", trace::WHNFString{"world", {}}}},
        }),
        stubAmbientRoot());
    auto child = obj->getListElem(1);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->getStringIgnoreContext(), "world");
}

TEST(AmbientObjectTest, GetAttrNames)
{
    auto arg = testId(0);
    auto obj = std::make_shared<OuterObject>(
        testSubject(0),
        mockResolver({
            {"getWHNF:" + ambientHex(arg), trace::ResultWHNF{"set", trace::WHNFAttrs{{"a", "b", "c"}}}},
        }),
        stubAmbientRoot());
    auto names = obj->getAttrNames();
    EXPECT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "a");
}

} // namespace nix
