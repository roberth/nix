#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>

#include "nix/expr/eval.hh"
#include "nix/expr/source-root.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/value.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/source-path.hh"

namespace nix {

/* `__curPos.file` is lazily resolved through the same `coerceToString`
   path that interpolation of a `mkPath` value goes through. Routing
   is keyed on the `SourceRoot::kind` of the position's origin. The
   tests below admit an in-memory accessor under each user-visible
   kind (System and Copyable) via an explicit `SourceRoot` wrap, then
   parse a file rooted on it and inspect `__curPos.file`. Internal is
   covered indirectly through `mkPos`'s short-circuit-to-null (see
   `primops.cc::unsafeGetAttrPos`). */

class CurPosFileTest : public LibExprTest
{
protected:
    /* `copyPathToStore` consults the `sourcePathToHash` cache, which
       opens a sqlite db under `$XDG_CACHE_HOME` (or `$HOME/.cache` if
       unset). The nix sandbox sets `HOME=/homeless-shelter`, which is
       unwritable, so the test must point the cache somewhere it can
       write -- the meson harness sets `HOME` but the `nix build`
       harness doesn't. */
    void SetUp() override
    {
        auto dir = std::filesystem::temp_directory_path() / "nix-test-cache";
        std::filesystem::create_directories(dir);
        ::setenv("XDG_CACHE_HOME", dir.c_str(), 1);
    }

    /* Parse `__curPos` from a file inside `accessor` (admitted as
       `kind`) at path `inner` and return the resulting position
       attrset value. The whole point of this dance is that the
       position's origin must be the supplied accessor, not `rootFS`. */
    Value parseAndEvalCurPos(
        ref<MemorySourceAccessor> accessor, SourceRootKind kind, CanonPath inner, std::string_view contents)
    {
        accessor->addFile(inner, std::string(contents));

        auto root = SourceRoot::make(accessor.cast<SourceAccessor>(), kind);
        auto * expr = state.parseExprFromFile({root, inner});
        Value v;
        state.eval(expr, v);
        state.forceValue(v, noPos);
        return v;
    }
};

/* For a Copyable-rooted file, `__curPos.file` must resolve to
   `<storePath>/<subpath>` -- the same shape `toString` of a path
   Value produces. This is the case that matters for lazy flake
   paths: position files inside fetched trees become real store
   paths that `nix edit` and friends can resolve. */
TEST_F(CurPosFileTest, copyableAccessorResolvesToStorePathPlusSubpath)
{
    auto accessor = make_ref<MemorySourceAccessor>();
    accessor->fingerprint = "test:cur-pos-file";

    auto vPos = parseAndEvalCurPos(accessor, SourceRootKind::Copyable, CanonPath("/inner.nix"), "__curPos");

    ASSERT_EQ(vPos.type(), nAttrs);
    auto * file = vPos.attrs()->get(state.s.file);
    ASSERT_NE(file, nullptr);
    state.forceValue(*file->value, noPos);
    ASSERT_EQ(file->value->type(), nString);

    auto s = std::string(file->value->string_view());
    /* The resolved string must end with the subpath of the file. */
    EXPECT_TRUE(s.ends_with("/inner.nix")) << s;
    /* And it must contain a store-path prefix, not just be the raw
       canon path. */
    EXPECT_NE(s, "/inner.nix");
    EXPECT_NE(s.find("/nix/store/"), std::string::npos) << s;
}

/* For a System-rooted file (path literal, rootFS-backed posix paths,
   ...) `__curPos.file` must remain the raw absolute canon path.
   This matches today's behaviour for nixpkgs-style usage where
   `meta.position` points at filesystem-rooted paths. */
TEST_F(CurPosFileTest, systemAccessorReturnsRawAbs)
{
    auto accessor = make_ref<MemorySourceAccessor>();

    auto vPos = parseAndEvalCurPos(accessor, SourceRootKind::System, CanonPath("/some/file.nix"), "__curPos");

    ASSERT_EQ(vPos.type(), nAttrs);
    auto * file = vPos.attrs()->get(state.s.file);
    ASSERT_NE(file, nullptr);
    state.forceValue(*file->value, noPos);
    ASSERT_EQ(file->value->type(), nString);

    EXPECT_EQ(std::string(file->value->string_view()), "/some/file.nix");
}

/* `__curPos.file` is lazy: reading the attrset doesn't immediately
   compute the file string. Only forcing `.file` triggers resolution.
   This guards the `mkApp` thunk wiring -- if we accidentally
   reverted to eager `mkString`, the `.file` attribute would be
   `nString` straight out of `mkPos` rather than a thunk that needs
   forcing. */
TEST_F(CurPosFileTest, fileAttributeIsLazyThunk)
{
    auto accessor = make_ref<MemorySourceAccessor>();
    accessor->fingerprint = "test:cur-pos-file-lazy";

    auto vPos = parseAndEvalCurPos(accessor, SourceRootKind::Copyable, CanonPath("/lazy.nix"), "__curPos");

    ASSERT_EQ(vPos.type(), nAttrs);
    auto * file = vPos.attrs()->get(state.s.file);
    ASSERT_NE(file, nullptr);
    /* Before forcing, the value is not yet a string. */
    EXPECT_NE(file->value->type(), nString);
}

} // namespace nix
