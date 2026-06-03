#include <gtest/gtest.h>

#include "nix/expr/tests/libexpr.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/tests/counting-source-accessor.hh"

namespace nix {

class LockInputTest : public LibExprTest
{
protected:
    ref<MemorySourceAccessor> accessor = make_ref<MemorySourceAccessor>();

    void SetUp() override
    {
        accessor->addFile(CanonPath("/file"), "content");
    }

    /* Construct an Input that lockInput's `fetchToStore2 + name` lookups
       accept, and whose scheme renders via `to_string()` — the mismatch
       diagnostic identifies the input by its URL form. `tarball` is gate-
       free (no experimental feature required) and the walk goes through
       our explicit `accessor` rather than the URL. Route through
       `fromAttrs` so the scheme dispatch fires (a directly constructed
       Input has a null scheme and can't render). */
    fetchers::Input makeInput()
    {
        fetchers::Attrs attrs;
        attrs.insert_or_assign("type", std::string("tarball"));
        attrs.insert_or_assign("url", std::string("https://example.com/test.tar"));
        attrs.insert_or_assign("name", std::string("test"));
        return fetchers::Input::fromAttrs(fetchSettings, std::move(attrs));
    }
};

/* lockInput records narHash on input.attrs so downstream code can treat
   the input as locked. Distinguishing this from mountInput on the
   visible-attrs side: this is the bit lockInput *keeps*. */
TEST_F(LockInputTest, RecordsNarHashOnInputAttrs)
{
    auto input = makeInput();
    ASSERT_FALSE(input.getNarHash().has_value());

    state.lockInput(input, input, accessor);

    EXPECT_TRUE(input.getNarHash().has_value());
}

/* lockInput throws when originalInput asserts a narHash that disagrees
   with the walked tree's narHash. Pins the exit-102 mismatch contract
   carried over from mountInput; flake input updates rely on it firing
   before a stale narHash is committed. Detection is deferred: lockInput
   installs a LazyAttr whose force does the walk-and-compare. Any
   consumer (lockfile write, computeStorePath, attrsToJSON, getNarHash)
   forces the value and surfaces the throw. We assert both the exit
   status (102 — flake refresh keys off it) and the message shape so a
   refactor that fell back to a generic `Error(status=1, "...")` here
   would surface immediately. */
TEST_F(LockInputTest, ThrowsOnNarHashAssertionMismatch)
{
    auto input = makeInput();

    auto asserted = input;
    /* A plausible-shaped but definitely-wrong SHA-256. */
    asserted.attrs.insert_or_assign("narHash", std::string("sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="));

    state.lockInput(input, asserted, accessor);

    try {
        (void) input.getNarHash();
        FAIL() << "expected getNarHash to throw on the deferred mismatch";
    } catch (const Error & e) {
        EXPECT_EQ(e.info().status, 102u);
        EXPECT_NE(e.what(), nullptr);
        EXPECT_NE(std::string{e.what()}.find("NAR hash mismatch in input"), std::string::npos)
            << "actual message: " << e.what();
    }
}

/* lockInput's narHash LazyAttr memoises: forcing the attribute multiple
   times walks the tree at most once. Matters for non-fingerprinted
   accessors (dirty trees, in-memory), where `fetchToStore2`'s
   sourcePathToHash cache never kicks in — without memoisation each
   force re-walks. With fingerprinted accessors the sqlite cache
   already shares; this test pins the parity. */
TEST_F(LockInputTest, NarHashLazyAttrMemoises)
{
    auto counted = make_ref<CountingSourceAccessor>(accessor);
    auto input = makeInput();

    state.lockInput(input, input, counted);
    EXPECT_EQ(counted->readFileCount.load(), 0u) << "lockInput must not walk";
    input.getNarHash();
    EXPECT_EQ(counted->readFileCount.load(), 1u) << "first force walks the one-file tree";
    input.getNarHash();
    input.getNarHash();
    EXPECT_EQ(counted->readFileCount.load(), 1u) << "subsequent forces are memoised";
}

/* Idempotency: a second lockInput call on an already-locked input
   should be a no-op on input.attrs (the recorded narHash stays the
   same) and must not throw. This pins the property that downstream
   callers can re-lock without worrying about state churn. */
TEST_F(LockInputTest, IdempotentOnAlreadyLockedInput)
{
    auto input = makeInput();
    state.lockInput(input, input, accessor);
    auto firstHash = input.getNarHash();
    ASSERT_TRUE(firstHash.has_value());

    EXPECT_NO_THROW(state.lockInput(input, input, accessor));
    EXPECT_EQ(input.getNarHash(), firstHash);
}

} // namespace nix
