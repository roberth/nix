#include <gtest/gtest.h>

#include "nix/expr/fetch-tree.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/fetchers/attrs.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/fetchers/mountable-tree.hh"
#include "nix/store/path.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/source-path.hh"

namespace nix {

class LazyFetcherAttrTest : public LibExprTest
{
protected:
    StorePath dummyPath()
    {
        return StorePath{"g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-test"};
    }

    /* A MountableTree backed by the dummy storePath and an empty
       accessor thunk. emit's eager rendering only uses storePath, so
       these tests don't fire the accessor — making this the right
       shape for tests that don't care about lazy reads. */
    fetchers::MountableTree dummyTree()
    {
        return fetchers::MountableTree{
            .storePath = dummyPath(),
            .accessor = [acc = make_ref<MemorySourceAccessor>().cast<SourceAccessor>()]() { return acc; },
        };
    }
};

TEST_F(LazyFetcherAttrTest, nonLazyAttrProducesImmediateValue)
{
    fetchers::Input input;
    input.attrs.insert_or_assign("type", std::string("git"));
    input.attrs.insert_or_assign("revCount", uint64_t(5));

    Value v;
    emitTreeAttrs(state, dummyTree(), input, v, false, false);
    state.forceValue(v, noPos);

    auto * rcAttr = v.attrs()->get(state.symbols.create("revCount"));
    ASSERT_NE(rcAttr, nullptr);
    state.forceValue(*rcAttr->value, noPos);
    EXPECT_EQ(rcAttr->value->integer().value, 5);
}

TEST_F(LazyFetcherAttrTest, lazyAttrProducesThunk)
{
    int calls = 0;
    fetchers::Input input;
    input.attrs.insert_or_assign("type", std::string("git"));
    input.attrs.insert_or_assign(
        "revCount",
        fetchers::LazyAttr(
            make_ref<fetchers::LazyAttrComputation>(
                fetchers::LazyAttrComputation{.compute = [&calls]() -> fetchers::ResolvedAttr {
                    calls++;
                    return uint64_t(42);
                }})));

    Value v;
    emitTreeAttrs(state, dummyTree(), input, v, false, false);
    state.forceValue(v, noPos);

    auto * rcAttr = v.attrs()->get(state.symbols.create("revCount"));
    ASSERT_NE(rcAttr, nullptr);

    // Not yet forced, so the lazy function should not have been called
    EXPECT_EQ(calls, 0);

    // Force the thunk
    state.forceValue(*rcAttr->value, noPos);
    EXPECT_EQ(rcAttr->value->integer().value, 42);
    EXPECT_EQ(calls, 1);
}

TEST_F(LazyFetcherAttrTest, lazyFunctionOnlyCalledOnAccess)
{
    int calls = 0;
    fetchers::Input input;
    input.attrs.insert_or_assign("type", std::string("git"));
    input.attrs.insert_or_assign("lastModified", uint64_t(1000));
    input.attrs.insert_or_assign(
        "revCount",
        fetchers::LazyAttr(
            make_ref<fetchers::LazyAttrComputation>(
                fetchers::LazyAttrComputation{.compute = [&calls]() -> fetchers::ResolvedAttr {
                    calls++;
                    return uint64_t(99);
                }})));

    Value v;
    emitTreeAttrs(state, dummyTree(), input, v, false, false);
    state.forceValue(v, noPos);

    // Access lastModified, so should not trigger lazy revCount
    auto * lmAttr = v.attrs()->get(state.symbols.create("lastModified"));
    ASSERT_NE(lmAttr, nullptr);
    state.forceValue(*lmAttr->value, noPos);
    EXPECT_EQ(lmAttr->value->integer().value, 1000);
    EXPECT_EQ(calls, 0);

    // Now access revCount
    auto * rcAttr = v.attrs()->get(state.symbols.create("revCount"));
    ASSERT_NE(rcAttr, nullptr);
    state.forceValue(*rcAttr->value, noPos);
    EXPECT_EQ(rcAttr->value->integer().value, 99);
    EXPECT_EQ(calls, 1);
}

/* When `lazy = true`, emit renders `outPath` as a path-typed Value
   rooted on the accessor — with metadata identical to the eager
   rendering. */
TEST_F(LazyFetcherAttrTest, lazyEmitProducesPathTypedOutPath)
{
    auto accessor = make_ref<MemorySourceAccessor>();
    accessor->addFile(CanonPath("/file"), "content");

    fetchers::Input input;
    input.attrs.insert_or_assign("type", std::string("git"));
    input.attrs.insert_or_assign("revCount", uint64_t(7));

    Value v;
    emitTreeAttrs(
        state,
        fetchers::MountableTree{
            .storePath = std::nullopt,
            .accessor = [acc = accessor.cast<SourceAccessor>()]() { return acc; },
        },
        input,
        v,
        /*emptyRevFallback=*/false,
        /*forceDirty=*/false,
        /*lazy=*/true);
    state.forceValue(v, noPos);

    /* outPath is a path-typed Value rooted at the accessor's root. */
    auto * outPath = v.attrs()->get(state.s.outPath);
    ASSERT_NE(outPath, nullptr);
    state.forceValue(*outPath->value, noPos);
    EXPECT_EQ(outPath->value->type(), nPath);
    EXPECT_EQ(&*outPath->value->path().accessor, &*accessor);
    EXPECT_EQ(outPath->value->path().path.abs(), "/");

    /* Metadata layer agrees with the eager shape. */
    auto * rcAttr = v.attrs()->get(state.symbols.create("revCount"));
    ASSERT_NE(rcAttr, nullptr);
    state.forceValue(*rcAttr->value, noPos);
    EXPECT_EQ(rcAttr->value->integer().value, 7);
}

} // namespace nix
