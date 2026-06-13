#include <gtest/gtest.h>

#include "nix/expr/eval.hh"
#include "nix/expr/source-root.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/value.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/source-root.hh"

namespace nix {

/* `toString p` on a path Value has always produced a contextless
   string — NixOS modules and other consumers rely on this. Under
   lazy-paths, a path Value rooted at a Copyable accessor (e.g. a
   fetched tree's `outPath`) cannot produce its string form without
   materialising the tree to a storepath, and the natural
   `copyPathToStore(context, ...)` call pollutes the context with the
   resulting `Opaque` element. These tests pin the contextless
   contract for `toString`-style coercion (`copyToStore=false`,
   `canonicalizePath=false`). */
class ToStringPathContextTest : public LibExprTest
{
protected:
    /* Construct a Copyable-rooted accessor + a path Value at its
       root, mirroring how a fetched tree's `outPath` surfaces under
       `emitTreeAttrs(lazy=true)`. */
    std::pair<Value, ref<MemorySourceAccessor>> makeCopyablePath()
    {
        auto acc = make_ref<MemorySourceAccessor>();
        acc->addFile(CanonPath("/file"), "contents\n");
        auto root = SourceRoot::make(acc.cast<SourceAccessor>(), SourceRootKind::Copyable);
        Value v;
        v.mkPath(RootedPath{root, CanonPath::root}, state.mem);
        return {std::move(v), acc};
    }
};

/* `toString` on a Copyable nPath: the string materialises the
   storepath but the *context* must remain empty. This is the
   regression guard. */
TEST_F(ToStringPathContextTest, ToStringIsContextless)
{
    auto [v, acc] = makeCopyablePath();
    NixStringContext ctx;
    /* Match `prim_toString`: coerceMore=true, copyToStore=false; the
       default `canonicalizePath=true` is what hits the Copyable
       materialisation branch where the bug lives. */
    state.coerceToString(noPos, v, ctx, "test", /*coerceMore=*/true, /*copyToStore=*/false);
    EXPECT_TRUE(ctx.empty()) << "toString on a Copyable nPath leaked context";
}

/* The interpolation arm (`copyToStore=true`) still adds context — we
   are not breaking `"${p}"` semantics, only the `toString` arm. */
TEST_F(ToStringPathContextTest, InterpolationAddsContext)
{
    auto [v, acc] = makeCopyablePath();
    NixStringContext ctx;
    state.coerceToString(
        noPos,
        v,
        ctx,
        "test",
        /*coerceMore=*/true,
        /*copyToStore=*/true,
        /*canonicalizePath=*/false);
    EXPECT_FALSE(ctx.empty()) << "interpolation on a Copyable nPath dropped context";
}

} // namespace nix
