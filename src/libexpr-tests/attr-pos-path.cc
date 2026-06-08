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

/* Position attrsets — the result of `builtins.unsafeGetAttrPos`,
   of `__curPos`, and of anywhere else `mkPos` is the value
   constructor — gain a `.path` attribute alongside the existing
   `.file`, `.line`, `.column`. The value is a path Value
   (nPath) carrying the position's origin RootedPath directly.

   Why a path Value rather than just relying on `.file`:
   - `.file` on a Copyable origin renders via `toString`, which
     materialises the whole tree into the store. NixOS-style
     callers that just want to compare or manipulate paths pay a
     hash for no reason.
   - With `.path` as a real path Value, callers can stay lazy:
     compare via `==` / `isPathEquivalent`, append subpaths,
     etc., without ever tripping the materialisation.
   - Reuses the same emission machinery as `fetchTree`'s lazy
     `outPath`: `Value::mkPath(RootedPath, mem)` — no
     coerceToString, no copyPathToStore.

   Internal-kinded origins still cause `mkPos` to return `null`
   for the whole position; there's no path either. Stdin /
   String origins likewise yield `null`. */

class AttrPosPathTest : public LibExprTest
{
protected:
    /* copyPathToStore uses an on-disk sqlite cache under
       $XDG_CACHE_HOME; nix's sandbox HOME is unwritable. Same
       hack as cur-pos-file.cc. .path doesn't go through that
       path, but the harness loads parse-time files which may. */
    void SetUp() override
    {
        auto dir = std::filesystem::temp_directory_path() / "nix-test-cache";
        std::filesystem::create_directories(dir);
        ::setenv("XDG_CACHE_HOME", dir.c_str(), 1);
    }

    /* Parse `contents` as the file `inner` under an accessor
       admitted with `kind`, evaluate, and return the result. */
    Value
    parseAndEval(ref<MemorySourceAccessor> accessor, SourceRootKind kind, CanonPath inner, std::string_view contents)
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

/* ----- `__curPos` exposes `.path` ------------------------------- */

TEST_F(AttrPosPathTest, curPosCopyableExposesPathValue)
{
    /* For a Copyable-rooted file, `__curPos.path` is a path
       Value with the file's own RootedPath. The point is that
       it stays a path — never reduced through `coerceToString` —
       so callers can use it under lazy semantics. */
    auto accessor = make_ref<MemorySourceAccessor>();

    auto vPos = parseAndEval(accessor, SourceRootKind::Copyable, CanonPath("/m.nix"), "__curPos");

    ASSERT_EQ(vPos.type(), nAttrs);
    auto * pathAttr = vPos.attrs()->get(state.s.path);
    ASSERT_NE(pathAttr, nullptr) << "position attrset is missing the .path slot";
    state.forceValue(*pathAttr->value, noPos);
    ASSERT_EQ(pathAttr->value->type(), nPath) << "the .path slot must be a path Value, not a string";

    auto rp = pathAttr->value->rootedPath();
    EXPECT_EQ(rp.root->kind, SourceRootKind::Copyable);
    EXPECT_EQ(rp.path.abs(), "/m.nix");
}

TEST_F(AttrPosPathTest, curPosSystemExposesPathValue)
{
    /* Same for a System-rooted file. The path Value carries the
       System SourceRoot; toString on it would be just the
       abspath (no IO). */
    auto accessor = make_ref<MemorySourceAccessor>();

    auto vPos = parseAndEval(accessor, SourceRootKind::System, CanonPath("/sys/file.nix"), "__curPos");

    ASSERT_EQ(vPos.type(), nAttrs);
    auto * pathAttr = vPos.attrs()->get(state.s.path);
    ASSERT_NE(pathAttr, nullptr);
    state.forceValue(*pathAttr->value, noPos);
    ASSERT_EQ(pathAttr->value->type(), nPath);

    auto rp = pathAttr->value->rootedPath();
    EXPECT_EQ(rp.root->kind, SourceRootKind::System);
    EXPECT_EQ(rp.path.abs(), "/sys/file.nix");
}

/* ----- `unsafeGetAttrPos` also exposes `.path` ------------------ */

TEST_F(AttrPosPathTest, unsafeGetAttrPosExposesPathValue)
{
    /* unsafeGetAttrPos and __curPos share `mkPos`, so the
       `.path` slot applies identically here. Pin it explicitly
       because this is the entry point the NixOS module system
       uses to discover module file paths. */
    auto accessor = make_ref<MemorySourceAccessor>();

    auto vPos = parseAndEval(
        accessor, SourceRootKind::Copyable, CanonPath("/mod.nix"), "builtins.unsafeGetAttrPos \"x\" { x = 1; }");

    ASSERT_EQ(vPos.type(), nAttrs);
    auto * pathAttr = vPos.attrs()->get(state.s.path);
    ASSERT_NE(pathAttr, nullptr);
    state.forceValue(*pathAttr->value, noPos);
    ASSERT_EQ(pathAttr->value->type(), nPath);

    auto rp = pathAttr->value->rootedPath();
    EXPECT_EQ(rp.root->kind, SourceRootKind::Copyable);
    EXPECT_EQ(rp.path.abs(), "/mod.nix");
}

/* ----- `.path` and `.file` agree under coercion ----------------- */

TEST_F(AttrPosPathTest, pathAttrCoercesToFileAttrString)
{
    /* `toString` of the `.path` value must equal the `.file`
       string. Pins the equivalence so the two surfaces never
       drift — anything that touches `.file` should be free to
       move to `.path` and back via `toString`. */
    auto accessor = make_ref<MemorySourceAccessor>();

    auto vPos = parseAndEval(accessor, SourceRootKind::System, CanonPath("/agree.nix"), "__curPos");

    ASSERT_EQ(vPos.type(), nAttrs);
    auto * fileAttr = vPos.attrs()->get(state.s.file);
    auto * pathAttr = vPos.attrs()->get(state.s.path);
    ASSERT_NE(fileAttr, nullptr);
    ASSERT_NE(pathAttr, nullptr);
    state.forceValue(*fileAttr->value, noPos);
    state.forceValue(*pathAttr->value, noPos);
    ASSERT_EQ(fileAttr->value->type(), nString);
    ASSERT_EQ(pathAttr->value->type(), nPath);

    NixStringContext context;
    auto pathStr =
        state.coerceToString(noPos, *pathAttr->value, context, "", /*coerceMore=*/false, /*copyToStore=*/false);
    EXPECT_EQ(std::string(*pathStr), std::string(fileAttr->value->string_view()));
}

} // namespace nix
