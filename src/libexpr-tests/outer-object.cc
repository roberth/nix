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
static Hash testId(int n)
{
    return hashString(HashAlgorithm::SHA256, "test:" + std::to_string(n));
}

static Subject testSubject(int n)
{
    return Subject{PostulatedIdempotentRead{testId(n)}};
}

/* Stub outer Object — OuterObject requires holding a shared_ptr to
   "the outer Object it wraps." Tests don't dispatch through it;
   the mock queryFn ignores it. Object's virtuals are pure, so we
   need a minimal concrete subclass. */
struct StubOuterObject : Object
{
    std::shared_ptr<Object> maybeGetAttr(const std::string &) override { return nullptr; }
    std::vector<std::string> getAttrNames() override { return {}; }
    std::string getStringIgnoreContext() override { return {}; }
    std::string getStringWithoutContext() override { return {}; }
    std::pair<std::string, NixStringContext> getStringWithContext() override { return {}; }
    RootedPath getPath() override { throw Error("stub"); }
    bool getBool(std::string_view = "") override { return false; }
    NixInt getInt(std::string_view = "") override { return NixInt{0}; }
    NixFloat getFloat(std::string_view = "") override { return 0.0; }
    size_t getListSize() override { return 0; }
    std::shared_ptr<Object> getListElem(size_t) override { return nullptr; }
    ObjectType getTypeLazy() override { return nThunk; }
    ObjectType getType() override { return nThunk; }
    RootValue defeatCache() override { throw Error("stub"); }
    std::optional<FunctionInfo> getFunctionInfo() override { return std::nullopt; }
};

static std::shared_ptr<Object> stubOuter()
{
    return std::make_shared<StubOuterObject>();
}

static std::string ambientHex(Hash id)
{
    return id.to_string(HashFormat::Base16, false);
}

/**
 * Mock resolver: maps `"tag:ambientHex(subject-state-hash)"` strings to
 * predetermined results. Keying on the caller's Subject's state hash
 * distinguishes the parent OuterObject's queries from derived
 * children's queries.
 */
static OuterQueryFn mockResolver(std::map<std::string, trace::ResultVariant> responses)
{
    return [responses = std::move(responses)](
               std::shared_ptr<Object> /*outerObj*/,
               const trace::QueryVariant & q,
               Subject subject,
               Hash argAncestry) -> OuterQueryResult {
        auto stateHash = stateHashAfterSubject(subject, argAncestry, {});
        std::string objHex = ambientHex(stateHash);
        std::string key = std::visit(
            [&](const auto & query) -> std::string {
                return std::string(query.tag) + ":" + objHex;
            },
            q);
        auto it = responses.find(key);
        if (it == responses.end())
            throw Error("mock resolver: no response for %s", key);

        std::shared_ptr<Object> child;
        if (std::holds_alternative<trace::ResultMaybeType>(it->second)) {
            auto & rmt = std::get<trace::ResultMaybeType>(it->second);
            if (rmt.type)
                child = stubOuter();
        }
        if (std::holds_alternative<trace::ResultType>(it->second)) {
            child = stubOuter();
        }
        return {it->second, std::move(child)};
    };
}

TEST(AmbientObjectTest, GetType)
{
    auto arg = stateHashAfterSubject(testSubject(0), Hash(HashAlgorithm::SHA256), {});
    auto obj = std::make_shared<OuterObject>(
        testSubject(0),
        stubOuter(),
        mockResolver({{"getWHNF:" + ambientHex(arg), trace::ResultWHNF{"int", trace::WHNFInt{42}}}}),
        stubAmbientRoot());
    EXPECT_EQ(obj->getType(), nInt);
}

TEST(AmbientObjectTest, GetInt)
{
    auto arg = stateHashAfterSubject(testSubject(0), Hash(HashAlgorithm::SHA256), {});
    auto obj = std::make_shared<OuterObject>(
        testSubject(0),
        stubOuter(),
        mockResolver({{"getWHNF:" + ambientHex(arg), trace::ResultWHNF{"int", trace::WHNFInt{42}}}}),
        stubAmbientRoot());
    EXPECT_EQ(obj->getInt().value, 42);
}

TEST(AmbientObjectTest, GetString)
{
    auto arg = stateHashAfterSubject(testSubject(0), Hash(HashAlgorithm::SHA256), {});
    auto obj = std::make_shared<OuterObject>(
        testSubject(0),
        stubOuter(),
        mockResolver({{"getWHNF:" + ambientHex(arg), trace::ResultWHNF{"string", trace::WHNFString{"hello", {}}}}}),
        stubAmbientRoot());
    EXPECT_EQ(obj->getStringIgnoreContext(), "hello");
}

TEST(AmbientObjectTest, GetBool)
{
    auto arg = stateHashAfterSubject(testSubject(0), Hash(HashAlgorithm::SHA256), {});
    auto obj = std::make_shared<OuterObject>(
        testSubject(0),
        stubOuter(),
        mockResolver({{"getWHNF:" + ambientHex(arg), trace::ResultWHNF{"bool", trace::WHNFBool{true}}}}),
        stubAmbientRoot());
    EXPECT_TRUE(obj->getBool());
}

TEST(AmbientObjectTest, GetAttrReturnsChild)
{
    auto arg = stateHashAfterSubject(testSubject(0), Hash(HashAlgorithm::SHA256), {});
    /* Child scopeStateId is the producer query's queryHash. With Subject-based
       construction the OuterObject derives this from DerivedSubject
       at construction time. */
    auto childStateHash = stateHashAfterSubject(
        Subject{DerivedSubject{
            .parent = std::make_shared<const Subject>(testSubject(0)),
            .kind = DerivedSubject::Kind::GetAttr,
            .name = "x",
        }},
        Hash(HashAlgorithm::SHA256),
        {});
    auto childHex = ambientHex(childStateHash);
    auto obj = std::make_shared<OuterObject>(
        testSubject(0),
        stubOuter(),
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
    auto arg = stateHashAfterSubject(testSubject(0), Hash(HashAlgorithm::SHA256), {});
    auto obj = std::make_shared<OuterObject>(
        testSubject(0), stubOuter(),
        mockResolver({{"getAttr:" + ambientHex(arg), trace::ResultMaybeType{std::nullopt}}}), stubAmbientRoot());
    EXPECT_EQ(obj->maybeGetAttr("missing"), nullptr);
}

TEST(AmbientObjectTest, GetListElem)
{
    auto arg = stateHashAfterSubject(testSubject(0), Hash(HashAlgorithm::SHA256), {});
    auto childStateHash = stateHashAfterSubject(
        Subject{DerivedSubject{
            .parent = std::make_shared<const Subject>(testSubject(0)),
            .kind = DerivedSubject::Kind::GetListElem,
            .index = 1,
        }},
        Hash(HashAlgorithm::SHA256),
        {});
    auto childHex = ambientHex(childStateHash);
    auto obj = std::make_shared<OuterObject>(
        testSubject(0),
        stubOuter(),
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
    auto arg = stateHashAfterSubject(testSubject(0), Hash(HashAlgorithm::SHA256), {});
    auto obj = std::make_shared<OuterObject>(
        testSubject(0),
        stubOuter(),
        mockResolver({
            {"getWHNF:" + ambientHex(arg), trace::ResultWHNF{"set", trace::WHNFAttrs{{"a", "b", "c"}}}},
        }),
        stubAmbientRoot());
    auto names = obj->getAttrNames();
    EXPECT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "a");
}

} // namespace nix
