#include <gtest/gtest.h>

#include "nix/expr/eval.hh"
#include "nix/expr/source-root.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/value.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/source-root.hh"

namespace nix {

/* `coerceToRootedPath` finds the language-level `SourceRoot` kind
   inductively by peeling the input Value's structure. These tests
   pin the contract for each peel step (bare path, `__toString`,
   `outPath`, string fallback) and the kind-preservation property
   across nested forms.

   The kind matters: `realisePath`, `import`, and `readDir` route on
   it to pick the symlink-resolution policy (Copyable uses
   `StrictCopyableBoundary`; System uses the lenient walker). Losing
   the kind silently relaxes the escape check on attrset-shaped
   inputs whose `outPath` is a path Value (the lazy-paths regime).
   These tests are the regression guard for that property. */
class CoerceToRootedPathTest : public LibExprTest
{
protected:
    /* Evaluate `contents` inside an in-memory Copyable accessor at
       `/test.nix`, force the result, and return both the Value and
       the accessor (so tests can assert `rp.root->accessor` is the
       expected one, not rootFS).

       Constructing the fixture by *parsing through a Copyable
       admission seam* matches how production reaches these shapes
       (a fetched flake's `flake.nix` evaluates with Copyable rooted
       path literals). Stack-allocated `mkPath` values, by contrast,
       interact poorly with the GC-tracked Nix-side closures that
       `__toString` recursion builds. */
    std::pair<Value, ref<MemorySourceAccessor>> evalInCopyable(std::string_view contents)
    {
        auto acc = make_ref<MemorySourceAccessor>();
        acc->addFile(CanonPath("/test.nix"), std::string(contents));
        auto root = make_ref<SourceRoot>(acc.cast<SourceAccessor>(), SourceRootKind::Copyable);
        auto * expr = state.parseExprFromFile(RootedPath{root, CanonPath("/test.nix")});
        Value v;
        state.eval(expr, v);
        state.forceValue(v, noPos);
        return {std::move(v), acc};
    }
};

/* A bare nPath: kind is the path Value's own `SourceRoot`. No
   peeling needed; the function returns its `rootedPath()` directly.
   This is the trivial case that establishes the "kind preserved"
   property the other tests build on. */
TEST_F(CoerceToRootedPathTest, BarePathPreservesCopyable)
{
    auto [v, acc] = evalInCopyable("./.");
    NixStringContext ctx;
    auto rp = state.coerceToRootedPath(noPos, v, ctx, "test");
    EXPECT_EQ(rp.root->kind, SourceRootKind::Copyable);
    EXPECT_EQ(&*rp.root->accessor, &*acc);
    EXPECT_EQ(rp.path.abs(), "/");
}

/* A bare nPath admitted under System (the rootFS case) carries
   System through. Pins the symmetric arm so a future regression
   that defaulted everything to one kind would surface here. */
TEST_F(CoerceToRootedPathTest, BarePathPreservesSystem)
{
    Value v;
    v.mkPath(RootedPath{state.rootFSRoot, CanonPath("/some/file")}, state.mem);
    NixStringContext ctx;
    auto rp = state.coerceToRootedPath(noPos, v, ctx, "test");
    EXPECT_EQ(rp.root->kind, SourceRootKind::System);
    EXPECT_EQ(rp.path.abs(), "/some/file");
}

/* `{ outPath = <Copyable nPath>; }` — the lazy-paths regime where
   `outPath` is itself a path Value. The structural peel recurses
   into the inner nPath; the inner's kind is what surfaces, not the
   System fallback the outer `nAttrs` would otherwise pick. This is
   the load-bearing case: without it, `import {outPath=lazyPath;}`
   would silently route through the System lenient symlink walker
   and skip the Copyable escape check. */
TEST_F(CoerceToRootedPathTest, OutPathIsBarePath_PreservesKind)
{
    auto [v, acc] = evalInCopyable("{ outPath = ./dir/file; }");
    NixStringContext ctx;
    auto rp = state.coerceToRootedPath(noPos, v, ctx, "test");
    EXPECT_EQ(rp.root->kind, SourceRootKind::Copyable);
    EXPECT_EQ(&*rp.root->accessor, &*acc);
    EXPECT_EQ(rp.path.abs(), "/dir/file");
}

/* `{ outPath = "/some/path"; }` — the documented contract case and
   the eager-fetchTree case. The string identifies a filesystem
   location; admit under rootFSRoot (System). This shape is what
   keeps `builtins.storePath { outPath = "/nix/store/X"; }` working
   for derivation-result attrsets. */
TEST_F(CoerceToRootedPathTest, OutPathIsString_GivesSystem)
{
    auto [v, _] = evalInCopyable("{ outPath = \"/some/absolute\"; }");
    NixStringContext ctx;
    auto rp = state.coerceToRootedPath(noPos, v, ctx, "test");
    EXPECT_EQ(rp.root->kind, SourceRootKind::System);
    EXPECT_EQ(rp.path.abs(), "/some/absolute");
}

/* `{ __toString = self: <Copyable nPath>; }` — Nix's documented
   contract is that `__toString` returns a string, but the function
   has historically recursed on the result, and the lazy-paths
   regime relies on the recursion preserving kind. Pin it. */
TEST_F(CoerceToRootedPathTest, ToStringReturnsPath_PreservesKind)
{
    auto [v, acc] = evalInCopyable("{ __toString = self: ./.; }");
    NixStringContext ctx;
    auto rp = state.coerceToRootedPath(noPos, v, ctx, "test");
    EXPECT_EQ(rp.root->kind, SourceRootKind::Copyable);
    EXPECT_EQ(&*rp.root->accessor, &*acc);
    EXPECT_EQ(rp.path.abs(), "/");
}

/* When both `__toString` and `outPath` are present, `__toString`
   wins (string-interpolation.md:215). Pin this by giving them
   distinct paths and verifying the result matches `__toString`'s.
   Wrapping `outPath` in a `throw` also pins that `outPath` isn't
   even forced when `__toString` wins. */
TEST_F(CoerceToRootedPathTest, ToStringTakesPrecedenceOverOutPath)
{
    auto [v, acc] = evalInCopyable("{ __toString = self: ./from-tostring; outPath = throw \"unreached\"; }");
    NixStringContext ctx;
    auto rp = state.coerceToRootedPath(noPos, v, ctx, "test");
    EXPECT_EQ(&*rp.root->accessor, &*acc) << "__toString's accessor should win, not outPath's";
    EXPECT_EQ(rp.path.abs(), "/from-tostring");
}

/* `{ outPath = { outPath = <Copyable nPath>; }; }` — two levels of
   attrset wrapping. The inductive peel handles arbitrary nesting
   as long as each level is `outPath` or `__toString`. Pins that
   the recursion isn't bounded to one peel. */
TEST_F(CoerceToRootedPathTest, NestedOutPath_PreservesInnerKind)
{
    auto [v, acc] = evalInCopyable("{ outPath = { outPath = ./nested-leaf; }; }");
    NixStringContext ctx;
    auto rp = state.coerceToRootedPath(noPos, v, ctx, "test");
    EXPECT_EQ(rp.root->kind, SourceRootKind::Copyable);
    EXPECT_EQ(&*rp.root->accessor, &*acc);
    EXPECT_EQ(rp.path.abs(), "/nested-leaf");
}

/* A bare nString gives System — the string identifies a filesystem
   location, which is always rootFS-rooted. This pins the fallback
   arm directly. */
TEST_F(CoerceToRootedPathTest, StringValue_GivesSystem)
{
    Value v;
    v.mkString("/etc/passwd", {}, state.mem);
    NixStringContext ctx;
    auto rp = state.coerceToRootedPath(noPos, v, ctx, "test");
    EXPECT_EQ(rp.root->kind, SourceRootKind::System);
    EXPECT_EQ(rp.path.abs(), "/etc/passwd");
}

} // namespace nix
