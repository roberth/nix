#include <gtest/gtest.h>

#include "nix/expr/fetch-tree.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/value/context.hh"
#include "nix/fetchers/attrs.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/fetchers/mountable-tree.hh"
#include "nix/store/path.hh"
#include "nix/store/store-api.hh"
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

/* narHash mirrors revCount's lazy-attr emit: when the input carries a
   LazyAttr on `narHash`, emit produces a Nix-side thunk rather than
   forcing the libfetchers LazyAttr eagerly. lockInput installs exactly
   such a LazyAttr; without this path emit would walk the tree at
   emitTreeAttrs time, undoing the lazy-paths deferral. */
TEST_F(LazyFetcherAttrTest, narHashLazyAttrProducesThunk)
{
    int calls = 0;
    auto sriHash = std::string("sha256-a4Xg9Q+RWpHBFQJYw8BN3aaXIlFcqaPSuv9QcjbUutw=");
    fetchers::Input input;
    input.attrs.insert_or_assign("type", std::string("git"));
    input.attrs.insert_or_assign(
        "narHash",
        fetchers::LazyAttr(
            make_ref<fetchers::LazyAttrComputation>(
                fetchers::LazyAttrComputation{.compute = [&calls, sriHash]() -> fetchers::ResolvedAttr {
                    calls++;
                    return sriHash;
                }})));

    Value v;
    emitTreeAttrs(state, dummyTree(), input, v, false, false);
    state.forceValue(v, noPos);

    auto * nhAttr = v.attrs()->get(state.symbols.create("narHash"));
    ASSERT_NE(nhAttr, nullptr);

    EXPECT_EQ(calls, 0) << "emit must not force the narHash LazyAttr";

    state.forceValue(*nhAttr->value, noPos);
    EXPECT_EQ(calls, 1);
    NixStringContext ctx;
    EXPECT_EQ(state.forceString(*nhAttr->value, ctx, noPos, "narHash"), sriHash);
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

/* When `lazy = false`, emit renders `outPath` as the storePath's printed
   string *and* registers the storePath in the string context. The context
   is the load-bearing piece — downstream derivations that interpolate
   `${input.outPath}` rely on it to see the input as a build-time
   dependency. The previous `emitTreeAttrs(StorePath, …)` overload had a
   dedicated test for this; v5 reaches the same code path via
   `MountableTree{ storePath = …, … }` + `lazy = false`. Pin it here. */
TEST_F(LazyFetcherAttrTest, eagerEmitProducesStorePathStringWithContext)
{
    fetchers::Input input;
    input.attrs.insert_or_assign("type", std::string("git"));
    auto sp = dummyPath();

    Value v;
    emitTreeAttrs(
        state,
        fetchers::MountableTree{
            .storePath = sp,
            .accessor = [acc = make_ref<MemorySourceAccessor>().cast<SourceAccessor>()]() { return acc; },
        },
        input,
        v,
        /*emptyRevFallback=*/false,
        /*forceDirty=*/false,
        /*lazy=*/false);
    state.forceValue(v, noPos);

    auto * outPath = v.attrs()->get(state.s.outPath);
    ASSERT_NE(outPath, nullptr);

    /* outPath is a string carrying the printed storePath. */
    NixStringContext context;
    auto s = state.forceString(*outPath->value, context, noPos, "outPath");
    EXPECT_EQ(s, store->printStorePath(sp));

    /* And the context contains an Opaque element naming that storePath
       — silent loss of this is a derivation-graph correctness bug. */
    bool foundStorePath = false;
    for (auto & c : context) {
        if (auto * o = std::get_if<NixStringContextElem::Opaque>(&c.raw))
            if (o->path == sp)
                foundStorePath = true;
    }
    EXPECT_TRUE(foundStorePath);
}

} // namespace nix
