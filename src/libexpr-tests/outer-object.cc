#include <gtest/gtest.h>

#include "nix/expr/outer-object.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/util/source-accessor.hh"

namespace nix {

/* lazy-paths: tests don't exercise `getPath`, so any SourceRoot
   suffices for the constructor — just stub one out. */
static ref<SourceRoot> stubOuterRoot()
{
    return SourceRoot::make(getFSSourceAccessor(), SourceRootKind::Internal);
}

/* Producer Selector for a test OuterObject. #183: identity is the
   content hash of the producer Selector. Using SelectorArg with a
   per-test depth distinguishes proxies without hitting real trace
   payloads (SelectorArg's content hash is a pure function of depth).
   `testId(n)` is retained as a distinct-hash source unrelated to
   producers, for tests that need an opaque hash. */
static Hash testId(int n)
{
    return hashString(HashAlgorithm::SHA256, "test:" + std::to_string(n));
}

static trace::SelectorVariant testProducer(int n)
{
    return trace::SelectorArg{n};
}

static std::string producerHex(int n)
{
    return TracingDecisionGraph::computeSelectorHash(testProducer(n))
        .to_string(HashFormat::Base16, false);
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

/**
 * Mock resolver: maps `"tag:producerHex"` strings to predetermined
 * results. Keying on the caller's producer content hash distinguishes
 * the parent OuterObject's queries from derived children's queries.
 */
static OuterQueryFn mockResolver(std::map<std::string, trace::ResultVariant> responses)
{
    return [responses = std::move(responses)](
               std::shared_ptr<Object> /*outerObj*/,
               const trace::SelectorVariant & q,
               trace::SelectorVariant producer) -> OuterQueryResult {
        auto stateHash = TracingDecisionGraph::computeSelectorHash(producer);
        std::string objHex = stateHash.to_string(HashFormat::Base16, false);
        std::string key = std::visit(
            [&](const auto & query) -> std::string {
                return std::string(query.tag) + ":" + objHex;
            },
            q);
        auto it = responses.find(key);
        if (it == responses.end())
            throw Error("mock resolver: no response for %s", key);

        std::shared_ptr<Object> child;
        if (std::holds_alternative<trace::ResultWHNF>(it->second)) {
            child = stubOuter();
        }
        return {it->second, std::move(child)};
    };
}

TEST(OuterObjectTest, GetType)
{
    auto arg = producerHex(0);
    auto obj = std::make_shared<OuterObject>(
        testProducer(0),
        stubOuter(),
        mockResolver({{"arg:" + arg, trace::ResultWHNF{"int", trace::WHNFInt{42}}}}),
        stubOuterRoot());
    EXPECT_EQ(obj->getType(), nInt);
}

TEST(OuterObjectTest, GetInt)
{
    auto arg = producerHex(0);
    auto obj = std::make_shared<OuterObject>(
        testProducer(0),
        stubOuter(),
        mockResolver({{"arg:" + arg, trace::ResultWHNF{"int", trace::WHNFInt{42}}}}),
        stubOuterRoot());
    EXPECT_EQ(obj->getInt().value, 42);
}

TEST(OuterObjectTest, GetString)
{
    auto arg = producerHex(0);
    auto obj = std::make_shared<OuterObject>(
        testProducer(0),
        stubOuter(),
        mockResolver({{"arg:" + arg, trace::ResultWHNF{"string", trace::WHNFString{"hello", {}}}}}),
        stubOuterRoot());
    EXPECT_EQ(obj->getStringIgnoreContext(), "hello");
}

TEST(OuterObjectTest, GetBool)
{
    auto arg = producerHex(0);
    auto obj = std::make_shared<OuterObject>(
        testProducer(0),
        stubOuter(),
        mockResolver({{"arg:" + arg, trace::ResultWHNF{"bool", trace::WHNFBool{true}}}}),
        stubOuterRoot());
    EXPECT_TRUE(obj->getBool());
}

TEST(OuterObjectTest, GetAttrReturnsChild)
{
    auto arg = producerHex(0);
    /* Under the fold, existence is projected from parent WHNFAttrs.names;
       retrieval is a SelectorGetAttr returning child WHNF. */
    auto obj = std::make_shared<OuterObject>(
        testProducer(0),
        stubOuter(),
        mockResolver({
            {"arg:" + arg, trace::ResultWHNF{"set", trace::WHNFAttrs{{"x"}}}},
            {"getAttr:" + arg, trace::ResultWHNF{"int", trace::WHNFInt{99}}},
        }),
        stubOuterRoot());
    auto child = obj->maybeGetAttr("x");
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->getInt().value, 99);
}

TEST(OuterObjectTest, GetAttrMissing)
{
    auto arg = producerHex(0);
    /* Parent has an empty name list — projection yields "missing". No
       getAttr query is issued. */
    auto obj = std::make_shared<OuterObject>(
        testProducer(0), stubOuter(),
        mockResolver({
            {"arg:" + arg, trace::ResultWHNF{"set", trace::WHNFAttrs{{}}}},
        }),
        stubOuterRoot());
    EXPECT_EQ(obj->maybeGetAttr("missing"), nullptr);
}

TEST(OuterObjectTest, GetListElem)
{
    auto arg = producerHex(0);
    /* Under the fold, bounds are projected from parent WHNFList.size;
       retrieval is SelectorGetListElem returning child WHNF. */
    auto obj = std::make_shared<OuterObject>(
        testProducer(0),
        stubOuter(),
        mockResolver({
            {"arg:" + arg, trace::ResultWHNF{"list", trace::WHNFList{5}}},
            {"getListElem:" + arg, trace::ResultWHNF{"string", trace::WHNFString{"world", {}}}},
        }),
        stubOuterRoot());
    auto child = obj->getListElem(1);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->getStringIgnoreContext(), "world");
}

TEST(OuterObjectTest, GetAttrNames)
{
    auto arg = producerHex(0);
    auto obj = std::make_shared<OuterObject>(
        testProducer(0),
        stubOuter(),
        mockResolver({
            {"arg:" + arg, trace::ResultWHNF{"set", trace::WHNFAttrs{{"a", "b", "c"}}}},
        }),
        stubOuterRoot());
    auto names = obj->getAttrNames();
    EXPECT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "a");
}

} // namespace nix
