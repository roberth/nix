#include <gtest/gtest.h>

#include "nix/expr/source-root.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/source-accessor.hh"

namespace nix {

class SourceRootResolveSymlinksTest : public LibExprTest
{
protected:
    /* Fixture: a tiny in-memory accessor with one file at the root,
       one in a subdir, and topology useful for distinguishing
       walked / rejected / silently-clamped outcomes. */
    ref<MemorySourceAccessor> makeFixture()
    {
        auto a = make_ref<MemorySourceAccessor>();
        a->addFile(CanonPath("/sub/file"), "x");
        a->addFile(CanonPath("/sibling/leaf"), "y");
        return a;
    }
};

/* Copyable: input-side `..` past root rejected. The wrapper's
   dispatch to `StrictAccessorBoundary` is the whole point. */
TEST_F(SourceRootResolveSymlinksTest, CopyableRejectsInputEscape)
{
    auto root = SourceRoot::make(makeFixture().cast<SourceAccessor>(), SourceRootKind::Copyable);
    EXPECT_THROW(
        nix::resolveSymlinks(*root, std::string_view{"/../../escape"}, SymlinkResolution::Ancestors),
        AccessorBoundaryEscape);
}

/* Copyable: `..` that stays within the tree is fine. Distinguishes
   "any `..` rejected" from "only escape rejected". */
TEST_F(SourceRootResolveSymlinksTest, CopyableAllowsInTreeParent)
{
    auto root = SourceRoot::make(makeFixture().cast<SourceAccessor>(), SourceRootKind::Copyable);
    auto resolved = nix::resolveSymlinks(*root, std::string_view{"/sub/../sibling/leaf"}, SymlinkResolution::Ancestors);
    EXPECT_EQ(resolved.abs(), "/sibling/leaf");
}

/* Copyable: symlink-target escape. The input path itself has no
   `..`, but resolution follows an in-tree symlink whose target
   pops past root. The walker splices the target's components and
   re-fires `onParent`, catching the escape that wasn't visible in
   the input string. */
TEST_F(SourceRootResolveSymlinksTest, CopyableRejectsSymlinkTargetEscape)
{
    auto a = make_ref<MemorySourceAccessor>();
    a->addFile(CanonPath("/sub/file"), "x");
    {
        MemorySink sink{*a};
        /* `/sub/escape` resolves to `../../target` — pops past root. */
        sink.createSymlink(CanonPath("/sub/escape"), "../../target");
    }
    auto root = SourceRoot::make(a.cast<SourceAccessor>(), SourceRootKind::Copyable);
    EXPECT_THROW(
        nix::resolveSymlinks(*root, std::string_view{"/sub/escape"}, SymlinkResolution::Full), AccessorBoundaryEscape);
}

/* System: input-side `..` past root silently clamps per
   `CanonPath`'s historical behaviour. Pins the lenient arm and
   acts as a regression guard against a future "make System strict
   too" change. */
TEST_F(SourceRootResolveSymlinksTest, SystemSilentlyClampsInputEscape)
{
    auto root = SourceRoot::make(makeFixture().cast<SourceAccessor>(), SourceRootKind::System);
    auto resolved = nix::resolveSymlinks(*root, std::string_view{"/../sibling/leaf"}, SymlinkResolution::Ancestors);
    EXPECT_EQ(resolved.abs(), "/sibling/leaf");
}

/* Copyable: absolute-target symlink rejected. Distinct from
   `..` escape: the symlink doesn't try to pop past root, but its
   `/sibling` target would shift meaning between accessor-view
   (= `<root>/sibling`) and post-materialisation (= real
   `/sibling` on the host filesystem). Refusing at admission
   keeps Copyable trees position-independent. */
TEST_F(SourceRootResolveSymlinksTest, CopyableRejectsAbsoluteSymlink)
{
    auto a = make_ref<MemorySourceAccessor>();
    a->addFile(CanonPath("/sibling/leaf"), "y");
    {
        MemorySink sink{*a};
        sink.createSymlink(CanonPath("/abs-link"), "/sibling");
    }
    auto root = SourceRoot::make(a.cast<SourceAccessor>(), SourceRootKind::Copyable);
    EXPECT_THROW(
        nix::resolveSymlinks(*root, std::string_view{"/abs-link/leaf"}, SymlinkResolution::Full),
        AccessorBoundaryEscape);
}

/* System: absolute-target symlink follows lenient (the rebased
   semantics match the libutil resolver's default). Pins the
   asymmetry with Copyable. */
TEST_F(SourceRootResolveSymlinksTest, SystemFollowsAbsoluteSymlink)
{
    auto a = make_ref<MemorySourceAccessor>();
    a->addFile(CanonPath("/sibling/leaf"), "y");
    {
        MemorySink sink{*a};
        sink.createSymlink(CanonPath("/abs-link"), "/sibling");
    }
    auto root = SourceRoot::make(a.cast<SourceAccessor>(), SourceRootKind::System);
    auto resolved = nix::resolveSymlinks(*root, std::string_view{"/abs-link/leaf"}, SymlinkResolution::Full);
    EXPECT_EQ(resolved.abs(), "/sibling/leaf");
}

/* System: symlink-target escape silently follows. Asymmetric with
   the Copyable case above; both pinned so a future change has to
   update both tests. */
TEST_F(SourceRootResolveSymlinksTest, SystemFollowsSymlinkTargetEscapeSilently)
{
    auto a = make_ref<MemorySourceAccessor>();
    a->addFile(CanonPath("/sub/anchor"), "x"); /* anchor so /sub exists as a directory */
    {
        MemorySink sink{*a};
        sink.createSymlink(CanonPath("/sub/escape"), "../../target");
    }
    auto root = SourceRoot::make(a.cast<SourceAccessor>(), SourceRootKind::System);
    auto resolved = nix::resolveSymlinks(*root, std::string_view{"/sub/escape"}, SymlinkResolution::Full);
    EXPECT_EQ(resolved.abs(), "/target");
}

/* Internal: the wrapper has no decided policy and the arm
   terminates the process via `unreachable()` rather than throwing.
   Callers that legitimately read Internal-rooted paths (e.g.
   `parseExprFromFile` loading corepkgs) bypass the wrapper. Use a
   death test: `unreachable()` calls `panic()` which terminates via
   `std::terminate()`, so EXPECT_THROW can't catch it. The regex
   matches the prefix `panic()` writes via the `Unexpected
   condition` formatter — pinning that the right abort path fires
   (vs. some unrelated terminate). */
TEST_F(SourceRootResolveSymlinksTest, InternalTerminatesProcess)
{
    auto root = SourceRoot::make(makeFixture().cast<SourceAccessor>(), SourceRootKind::Internal);
    EXPECT_DEATH(nix::resolveSymlinks(*root, std::string_view{"/anything"}), "Unexpected condition");
}

/* The `CanonPath` overload delegates to the `string_view` form. */
TEST_F(SourceRootResolveSymlinksTest, CanonPathOverloadDelegates)
{
    auto root = SourceRoot::make(makeFixture().cast<SourceAccessor>(), SourceRootKind::Copyable);
    auto resolved = nix::resolveSymlinks(*root, CanonPath("/sub/file"), SymlinkResolution::Ancestors);
    EXPECT_EQ(resolved.abs(), "/sub/file");
}

/* The `RootedPath` overload delegates to the SourceRoot form. */
TEST_F(SourceRootResolveSymlinksTest, RootedPathOverloadDelegates)
{
    auto root = SourceRoot::make(makeFixture().cast<SourceAccessor>(), SourceRootKind::Copyable);
    RootedPath rp{root, CanonPath("/sub/file")};
    auto resolved = nix::resolveSymlinks(rp, SymlinkResolution::Ancestors);
    EXPECT_EQ(resolved.abs(), "/sub/file");
}

/* `EvalState::getOrCreateRoot(acc, k)` keys on `(accessor, kind)`,
   not on accessor alone: the same accessor admitted under different
   kinds must produce two distinct `SourceRoot`s. Pins the central
   invariant the (accessor, kind) keying provides — a regression to
   accessor-only keying would silently return whichever kind the
   first admission used. */
TEST_F(SourceRootResolveSymlinksTest, GetOrCreateRootKeysOnAccessorAndKind)
{
    auto accessor = makeFixture().cast<SourceAccessor>();

    auto copyable = state.getOrCreateRoot(accessor, SourceRootKind::Copyable);
    auto system = state.getOrCreateRoot(accessor, SourceRootKind::System);

    EXPECT_NE(&*copyable, &*system) << "different kinds must allocate distinct SourceRoots";
    EXPECT_EQ(copyable->kind, SourceRootKind::Copyable);
    EXPECT_EQ(system->kind, SourceRootKind::System);

    /* And same-kind re-lookup returns the *same* SourceRoot. */
    auto copyable2 = state.getOrCreateRoot(accessor, SourceRootKind::Copyable);
    EXPECT_EQ(&*copyable, &*copyable2) << "same (accessor, kind) must memoise";
}

/* First admission with an `unpinnedId` stamps it on the cached
   SourceRoot — the producer's identity claim is captured. */
TEST_F(SourceRootResolveSymlinksTest, GetOrCreateRootStampsUnpinnedIdOnFirstAdmission)
{
    auto accessor = makeFixture().cast<SourceAccessor>();
    auto root = state.getOrCreateRoot(accessor, SourceRootKind::Copyable, "github:NixOS/nixpkgs");
    ASSERT_TRUE(root->unpinnedId.has_value());
    EXPECT_EQ(*root->unpinnedId, "github:NixOS/nixpkgs");
}

/* The first admission's id is sticky. Subsequent admissions returning
   the cached SourceRoot keep the original id even if the caller passes
   a different one. Two producers stamping the same (accessor, kind)
   with different URLs is not expected in practice; if it happens, the
   first one wins deterministically. */
TEST_F(SourceRootResolveSymlinksTest, GetOrCreateRootPreservesFirstIdOnReadmission)
{
    auto accessor = makeFixture().cast<SourceAccessor>();
    auto first = state.getOrCreateRoot(accessor, SourceRootKind::Copyable, "github:NixOS/nixpkgs");
    auto second = state.getOrCreateRoot(accessor, SourceRootKind::Copyable, "git+https://example.com/other");
    EXPECT_EQ(&*first, &*second);
    ASSERT_TRUE(second->unpinnedId.has_value());
    EXPECT_EQ(*second->unpinnedId, "github:NixOS/nixpkgs");
}

/* `rootFSRoot` is stamped with the `path:` scheme so System-kinded
   paths get an identity in the eval cache. Identical across runs
   for any given EvalState, so cache lookups against rootFS-rooted
   path values are stable. */
TEST_F(SourceRootResolveSymlinksTest, RootFSRootStampedWithPathScheme)
{
    ASSERT_TRUE(state.rootFSRoot->unpinnedId.has_value());
    EXPECT_EQ(*state.rootFSRoot->unpinnedId, "path:");
}

} // namespace nix
