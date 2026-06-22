#include <gtest/gtest.h>

#include "nix/expr/ambient-object.hh"
#include "nix/util/source-accessor.hh"

namespace nix {

/* lazy-paths: tests don't exercise `getPath`, so any SourceRoot
   suffices for the constructor — just stub one out. */
static ref<SourceRoot> stubAmbientRoot()
{
    return SourceRoot::make(getFSSourceAccessor(), SourceRootKind::Internal);
}

/* Tests use OpaqueContentSubject to pin the proxy's content id to a
   stable per-test value. The Subject variant exists for cases like
   apply-result args that don't have a positional/derived form. */
static AmbientId testId(int n)
{
    return hashString(HashAlgorithm::SHA256, "test:" + std::to_string(n));
}

static cidasks::Subject testSubject(int n)
{
    return cidasks::Subject{cidasks::OpaqueContentSubject{testId(n)}};
}

static std::string hex(AmbientId id)
{
    return id.to_string(HashFormat::Base16, false);
}

/**
 * Mock resolver: maps `"tag:hex(objectId)"` strings to predetermined
 * results. For child-producing queries, returns
 * `hashString("child:" + hex(objectId))` as the child id.
 */
static AmbientQueryFn mockResolver(std::map<std::string, trace::ResultVariant> responses)
{
    return [responses = std::move(responses)](
               AmbientId objectId,
               const trace::QueryVariant & q) -> AmbientQueryResult {
        std::string objHex = hex(objectId);
        std::string key = std::visit(
            [&](const auto & query) -> std::string {
                return std::string(query.tag) + ":" + objHex;
            },
            q);
        auto it = responses.find(key);
        if (it == responses.end())
            throw Error("mock resolver: no response for %s", key);

        // For queries that produce children, return a deterministic child id
        std::optional<AmbientId> childId;
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
    auto seed = testId(0);
    auto obj = std::make_shared<AmbientObject>(
        testSubject(0), mockResolver({{"getType:" + hex(seed), trace::ResultType{"int"}}}), stubAmbientRoot());
    EXPECT_EQ(obj->getType(), nInt);
}

TEST(AmbientObjectTest, GetInt)
{
    auto seed = testId(0);
    auto obj = std::make_shared<AmbientObject>(
        testSubject(0), mockResolver({{"getInt:" + hex(seed), trace::ResultInt{42}}}), stubAmbientRoot());
    EXPECT_EQ(obj->getInt().value, 42);
}

TEST(AmbientObjectTest, GetString)
{
    auto seed = testId(0);
    auto obj = std::make_shared<AmbientObject>(
        testSubject(0), mockResolver({{"getString:" + hex(seed), trace::ResultString{"hello"}}}), stubAmbientRoot());
    EXPECT_EQ(obj->getStringIgnoreContext(), "hello");
}

TEST(AmbientObjectTest, GetBool)
{
    auto seed = testId(0);
    auto obj = std::make_shared<AmbientObject>(
        testSubject(0), mockResolver({{"getBool:" + hex(seed), trace::ResultBool{true}}}), stubAmbientRoot());
    EXPECT_TRUE(obj->getBool());
}

TEST(AmbientObjectTest, GetAttrReturnsChild)
{
    auto seed = testId(0);
    /* Child cdi is the producer query's queryHash. With Subject-based
       construction the AmbientObject derives this from DerivedSubject
       at construction time. */
    auto childCdi = cidasks::contentIdAfter(
        cidasks::Subject{cidasks::DerivedSubject{
            .parent = std::make_shared<const cidasks::Subject>(testSubject(0)),
            .kind = cidasks::DerivedSubject::Kind::GetAttr,
            .name = "x",
        }},
        {});
    auto childHex = hex(childCdi);
    auto obj = std::make_shared<AmbientObject>(
        testSubject(0),
        mockResolver({
            {"getAttr:" + hex(seed), trace::ResultMaybeType{std::optional<std::string>{"int"}}},
            {"getInt:" + childHex, trace::ResultInt{99}},
        }),
        stubAmbientRoot());
    auto child = obj->maybeGetAttr("x");
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->getInt().value, 99);
}

TEST(AmbientObjectTest, GetAttrMissing)
{
    auto seed = testId(0);
    auto obj = std::make_shared<AmbientObject>(
        testSubject(0), mockResolver({{"getAttr:" + hex(seed), trace::ResultMaybeType{std::nullopt}}}), stubAmbientRoot());
    EXPECT_EQ(obj->maybeGetAttr("missing"), nullptr);
}

TEST(AmbientObjectTest, GetListElem)
{
    auto seed = testId(0);
    auto childCdi = cidasks::contentIdAfter(
        cidasks::Subject{cidasks::DerivedSubject{
            .parent = std::make_shared<const cidasks::Subject>(testSubject(0)),
            .kind = cidasks::DerivedSubject::Kind::GetListElem,
            .index = 1,
        }},
        {});
    auto childHex = hex(childCdi);
    auto obj = std::make_shared<AmbientObject>(
        testSubject(0),
        mockResolver({
            {"getListElem:" + hex(seed), trace::ResultType{"string"}},
            {"getString:" + childHex, trace::ResultString{"world"}},
        }),
        stubAmbientRoot());
    auto child = obj->getListElem(1);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->getStringIgnoreContext(), "world");
}

TEST(AmbientObjectTest, GetAttrNames)
{
    auto seed = testId(0);
    auto obj = std::make_shared<AmbientObject>(
        testSubject(0),
        mockResolver({{"getAttrNames:" + hex(seed), trace::ResultListOfStrings{{"a", "b", "c"}}}}),
        stubAmbientRoot());
    auto names = obj->getAttrNames();
    EXPECT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "a");
}

} // namespace nix
