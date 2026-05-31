#include <atomic>
#include <gtest/gtest.h>

#include "nix/fetchers/fetch-settings.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/fetchers/input-cache.hh"
#include "nix/util/memory-source-accessor.hh"

namespace nix {

/* Inserting a CachedInput whose accessor is a counting thunk and then
   reading it back must not fire the thunk: only the *consumer* of the
   looked-up entry decides when (and whether) to materialise. This pins
   the deferral contract for the lazy `accessor` field. */
TEST(InputCache, LookupDoesNotFireAccessorThunk)
{
    auto cache = fetchers::InputCache::create();
    fetchers::Settings settings;

    fetchers::Attrs attrs;
    attrs.insert_or_assign("type", std::string("tarball"));
    attrs.insert_or_assign("url", std::string("https://example.invalid/lazy"));
    auto input = fetchers::Input::fromAttrs(settings, std::move(attrs));

    auto fired = std::make_shared<std::atomic<int>>(0);

    cache->upsert(
        input,
        fetchers::InputCache::CachedInput{
            .lockedInput = input,
            .accessor = [fired]() -> ref<SourceAccessor> {
                ++*fired;
                return make_ref<MemorySourceAccessor>();
            },
        });

    EXPECT_EQ(fired->load(), 0);

    auto looked_up = cache->lookup(input);
    ASSERT_TRUE(looked_up.has_value());

    /* lookup must not fire; the consumer (us) decides. */
    EXPECT_EQ(fired->load(), 0);

    /* Firing yields the accessor and counts. */
    auto a1 = looked_up->accessor();
    EXPECT_EQ(fired->load(), 1);
}

/* The cache's accessor field is a `fun` — each call invokes the
   closure. Without an internal `memo`, two consumer-side `()` calls
   re-run the closure. This is the documented contract: lazy callers
   that wrap real work should `memo()` their thunk themselves, while
   identity-shape thunks (the input-cache.cc case) are naturally
   idempotent. Pin both sides of that contract. */
TEST(InputCache, ThunkIsNotMemoisedByCache)
{
    auto cache = fetchers::InputCache::create();
    fetchers::Settings settings;

    fetchers::Attrs attrs;
    attrs.insert_or_assign("type", std::string("tarball"));
    attrs.insert_or_assign("url", std::string("https://example.invalid/raw"));
    auto input = fetchers::Input::fromAttrs(settings, std::move(attrs));

    auto fired = std::make_shared<std::atomic<int>>(0);

    cache->upsert(
        input,
        fetchers::InputCache::CachedInput{
            .lockedInput = input,
            .accessor = [fired]() -> ref<SourceAccessor> {
                ++*fired;
                return make_ref<MemorySourceAccessor>();
            },
        });

    auto looked_up = cache->lookup(input);
    ASSERT_TRUE(looked_up.has_value());

    (void) looked_up->accessor();
    (void) looked_up->accessor();
    (void) looked_up->accessor();

    /* Three calls => three closure runs (the cache does not memo). */
    EXPECT_EQ(fired->load(), 3);
}

/* The identity-thunk wrapping `input-cache.cc` uses on cache misses
   captures the materialised accessor by value, so multiple consumers
   that fire it observe the same accessor instance. This is what makes
   the no-memo contract above safe for the real production callers. */
TEST(InputCache, IdentityThunkReturnsStableAccessor)
{
    auto cache = fetchers::InputCache::create();
    fetchers::Settings settings;

    fetchers::Attrs attrs;
    attrs.insert_or_assign("type", std::string("tarball"));
    attrs.insert_or_assign("url", std::string("https://example.invalid/stable"));
    auto input = fetchers::Input::fromAttrs(settings, std::move(attrs));

    auto inner = make_ref<MemorySourceAccessor>();

    cache->upsert(
        input,
        fetchers::InputCache::CachedInput{
            .lockedInput = input,
            .accessor = [inner]() -> ref<SourceAccessor> { return inner; },
        });

    auto looked_up = cache->lookup(input);
    ASSERT_TRUE(looked_up.has_value());

    auto a1 = looked_up->accessor();
    auto a2 = looked_up->accessor();

    EXPECT_EQ(&*a1, &*inner);
    EXPECT_EQ(&*a1, &*a2);
}

} // namespace nix
