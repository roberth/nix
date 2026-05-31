#include <atomic>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "nix/util/memory-source-accessor.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/util/tests/gmock-matchers.hh"

namespace nix {

namespace {

ref<SourceAccessor> emptyRoot()
{
    auto a = make_ref<MemorySourceAccessor>();
    MemorySink sink{*a};
    sink.createDirectory(CanonPath::root);
    return a;
}

/* Build an accessor with one file at /file containing `contents`. */
ref<SourceAccessor> withFile(std::string contents)
{
    auto a = make_ref<MemorySourceAccessor>();
    a->addFile(CanonPath("/file"), std::move(contents));
    return a;
}

} // namespace

/* The thunk for a non-root mount must not fire just because the mount
   was registered. Pre-laziness, mount() and the underlying accessor
   were the same object — there was no thunk to fire — so this is a
   real new contract.  */
TEST(MountedSourceAccessor, ThunksNotFiredOnMount)
{
    auto fired = std::make_shared<std::atomic<int>>(0);

    auto m = makeMountedSourceAccessor({
        {CanonPath::root, [acc = emptyRoot()]() { return acc; }},
        {CanonPath("/sub"),
         [fired]() -> ref<SourceAccessor> {
             ++*fired;
             return withFile("hi");
         }},
    });

    EXPECT_EQ(fired->load(), 0);
}

/* First lookup through getMount fires the thunk exactly once. */
TEST(MountedSourceAccessor, FirstLookupFiresThunkOnce)
{
    auto fired = std::make_shared<std::atomic<int>>(0);
    auto inner = withFile("hi");

    auto m = makeMountedSourceAccessor({
        {CanonPath::root, [acc = emptyRoot()]() { return acc; }},
        {CanonPath("/sub"),
         [fired, inner]() -> ref<SourceAccessor> {
             ++*fired;
             return inner;
         }},
    });

    auto a = m->getMount(CanonPath("/sub"));
    EXPECT_EQ(fired->load(), 1);
    /* Identity: the cached accessor is the one the thunk returned. */
    EXPECT_EQ(a.get(), &*inner);
}

/* Subsequent lookups reuse the cached accessor without re-firing the
   thunk. Distinguished from `FirstLookupFiresThunkOnce` so a future
   regression that turned the cache into a re-fetch shows up here. */
TEST(MountedSourceAccessor, SubsequentLookupsShareCachedAccessor)
{
    auto fired = std::make_shared<std::atomic<int>>(0);
    auto inner = withFile("hi");

    auto m = makeMountedSourceAccessor({
        {CanonPath::root, [acc = emptyRoot()]() { return acc; }},
        {CanonPath("/sub"),
         [fired, inner]() -> ref<SourceAccessor> {
             ++*fired;
             return inner;
         }},
    });

    auto a1 = m->getMount(CanonPath("/sub"));
    auto a2 = m->getMount(CanonPath("/sub"));
    auto a3 = m->getMount(CanonPath("/sub"));

    EXPECT_EQ(fired->load(), 1);
    EXPECT_EQ(a1.get(), a2.get());
    EXPECT_EQ(a2.get(), a3.get());
}

/* A file lookup that traverses the mount point (via the parent
   MountedSourceAccessor's read methods) also fires the thunk, exactly
   once.  This is the path most production callers actually exercise. */
TEST(MountedSourceAccessor, ThunkFiresOnFileTraversal)
{
    auto fired = std::make_shared<std::atomic<int>>(0);

    auto m = makeMountedSourceAccessor({
        {CanonPath::root, [acc = emptyRoot()]() { return acc; }},
        {CanonPath("/sub"),
         [fired]() -> ref<SourceAccessor> {
             ++*fired;
             return withFile("hi");
         }},
    });

    EXPECT_EQ(fired->load(), 0);

    auto contents = m->readFile(CanonPath("/sub/file"));
    EXPECT_EQ(contents, "hi");
    EXPECT_EQ(fired->load(), 1);

    /* Second read of the same path: cached. */
    auto contents2 = m->readFile(CanonPath("/sub/file"));
    EXPECT_EQ(contents2, "hi");
    EXPECT_EQ(fired->load(), 1);
}

/* invalidateCache must not fire un-fired thunks. Pinning this is the
   reason we kept a separate `fired` atomic on each entry — without it,
   the safe-and-conservative implementation would be to fire every
   thunk during invalidation, defeating the laziness on every
   invalidate call. */
TEST(MountedSourceAccessor, InvalidateCacheDoesNotFireUnfiredThunks)
{
    auto firedA = std::make_shared<std::atomic<int>>(0);
    auto firedB = std::make_shared<std::atomic<int>>(0);

    auto m = makeMountedSourceAccessor({
        {CanonPath::root, [acc = emptyRoot()]() { return acc; }},
        {CanonPath("/a"),
         [firedA]() -> ref<SourceAccessor> {
             ++*firedA;
             return withFile("a");
         }},
        {CanonPath("/b"),
         [firedB]() -> ref<SourceAccessor> {
             ++*firedB;
             return withFile("b");
         }},
    });

    /* Fire /a only. */
    (void) m->getMount(CanonPath("/a"));
    EXPECT_EQ(firedA->load(), 1);
    EXPECT_EQ(firedB->load(), 0);

    m->invalidateCache();

    /* After invalidate: /a stays fired (its cached invalidateCache
       was called, but that doesn't refire its thunk); /b is still
       un-fired. */
    EXPECT_EQ(firedA->load(), 1);
    EXPECT_EQ(firedB->load(), 0);
}

/* getMount on an absent mount point returns nullptr and does not fire
   anything. Pin the negative case so a future refactor doesn't
   accidentally treat "missing" as "register a default thunk". */
TEST(MountedSourceAccessor, GetMountOnUnregisteredPathReturnsNullptr)
{
    auto m = makeMountedSourceAccessor({
        {CanonPath::root, [acc = emptyRoot()]() { return acc; }},
    });

    EXPECT_EQ(m->getMount(CanonPath("/nowhere")), nullptr);
}

/* The thunk's exception must reach the caller verbatim — both via
   `getMount` directly and via reads through the parent accessor
   (production callers typically use the latter, e.g. `readFile`,
   `lstat`, etc.). Pin both the exception type AND a substring of the
   message, so a future implementation can't silently swallow the
   original and substitute a generic "mount failed" wrapper.

   What the entry's post-exception state is (retry vs. cache the
   error vs. anything else) is intentionally not pinned here; that
   choice should be driven by a concrete use case if one appears. */
TEST(MountedSourceAccessor, ThunkExceptionPropagatesThroughGetMount)
{
    auto m = makeMountedSourceAccessor({
        {CanonPath::root, [acc = emptyRoot()]() { return acc; }},
        {CanonPath("/sub"), []() -> ref<SourceAccessor> { throw Error("simulated fetcher failure"); }},
    });

    EXPECT_THAT(
        [&] { (void) m->getMount(CanonPath("/sub")); },
        ::testing::ThrowsMessage<Error>(testing::HasSubstrIgnoreANSIMatcher("simulated fetcher failure")));
}

TEST(MountedSourceAccessor, ThunkExceptionPropagatesThroughReadTraversal)
{
    /* Fresh accessor — we don't want any state from a prior failed
       `getMount` to influence this case. The body's "post-exception
       state intentionally not pinned" promise means the two
       propagation paths must be exercised on independent fixtures. */
    auto m = makeMountedSourceAccessor({
        {CanonPath::root, [acc = emptyRoot()]() { return acc; }},
        {CanonPath("/sub"), []() -> ref<SourceAccessor> { throw Error("simulated fetcher failure"); }},
    });

    EXPECT_THAT(
        [&] { (void) m->readFile(CanonPath("/sub/file")); },
        ::testing::ThrowsMessage<Error>(testing::HasSubstrIgnoreANSIMatcher("simulated fetcher failure")));
}

/* `mount()` called *after* construction (the production-exercised
   path used by `EvalState::mountInput` → `storeFS->mount`) must obey
   the same deferred-thunk semantics as constructor-time mounts. The
   other tests only exercise the constructor's mounts map; this one
   pins the explicit-API path. */
TEST(MountedSourceAccessor, MountAfterConstructionDefersThunk)
{
    auto fired = std::make_shared<std::atomic<int>>(0);

    /* Construct with just root; no mount at /late yet. */
    auto m = makeMountedSourceAccessor({
        {CanonPath::root, [acc = emptyRoot()]() { return acc; }},
    });
    EXPECT_EQ(m->getMount(CanonPath("/late")), nullptr);

    /* Register after construction. */
    m->mount(CanonPath("/late"), [fired]() -> ref<SourceAccessor> {
        ++*fired;
        return withFile("post-construct");
    });

    /* Registration alone must not fire the thunk. */
    EXPECT_EQ(fired->load(), 0);

    /* First lookup fires; subsequent lookups share the cached
       result. */
    auto a1 = m->getMount(CanonPath("/late"));
    auto a2 = m->getMount(CanonPath("/late"));
    EXPECT_EQ(fired->load(), 1);
    ASSERT_NE(a1, nullptr);
    EXPECT_EQ(a1.get(), a2.get());

    /* Reads through the parent accessor also reach the mount. */
    EXPECT_EQ(m->readFile(CanonPath("/late/file")), "post-construct");
    EXPECT_EQ(fired->load(), 1);
}

/* `mount()`-after-construction and `invalidateCache()` must compose
   the same way as constructor-time mounts do. Two interactions:

   1. An un-fired post-construction thunk must survive `invalidateCache`
      without being fired (mirrors `InvalidateCacheDoesNotFireUnfiredThunks`
      for the explicit-API path).
   2. A fired post-construction thunk's cached accessor must survive
      `invalidateCache` without re-firing the thunk — invalidate
      cascades to the inner accessor's caches, but the materialisation
      itself is not redone. */
TEST(MountedSourceAccessor, MountAfterConstructionSurvivesInvalidateCache)
{
    auto fired = std::make_shared<std::atomic<int>>(0);

    auto m = makeMountedSourceAccessor({
        {CanonPath::root, [acc = emptyRoot()]() { return acc; }},
    });
    m->mount(CanonPath("/late"), [fired]() -> ref<SourceAccessor> {
        ++*fired;
        return withFile("post-construct");
    });

    /* invalidateCache on an un-fired post-construction mount: must not
       fire the thunk. */
    m->invalidateCache();
    EXPECT_EQ(fired->load(), 0);

    /* First lookup still fires once. */
    (void) m->getMount(CanonPath("/late"));
    EXPECT_EQ(fired->load(), 1);

    /* invalidateCache after firing: must not refire the thunk. */
    m->invalidateCache();
    (void) m->getMount(CanonPath("/late"));
    EXPECT_EQ(fired->load(), 1);
}

} // namespace nix
