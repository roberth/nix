#include "nix/util/resolve-symlinks.hh"
#include "nix/util/memory-source-accessor.hh"

#include <gtest/gtest.h>

namespace nix {

class ResolveSymlinksTest : public ::testing::Test
{
protected:
    ref<MemorySourceAccessor> accessor = make_ref<MemorySourceAccessor>();
    MemorySink sink{*accessor};

    void SetUp() override
    {
        /* MemorySourceAccessor's root isn't a directory until something
           creates it. Symlinks-at-root tests need this. */
        sink.createDirectory(CanonPath::root);
    }

    /* Resolve through `nix::resolveSymlinks` against `accessor`. */
    CanonPath resolve(const CanonPath & path, SymlinkResolution mode = SymlinkResolution::Full)
    {
        return nix::resolveSymlinks(*accessor, path, mode);
    }
};

/* No-symlink, no-dotdot input is returned verbatim. Pins the trivial
   identity of the walker on a plain path. */
TEST_F(ResolveSymlinksTest, PlainPathReturnedUnchanged)
{
    accessor->addFile(CanonPath("/a/b/c"), "x");
    EXPECT_EQ(resolve(CanonPath("/a/b/c")).abs(), "/a/b/c");
}

/* Root resolves to root. */
TEST_F(ResolveSymlinksTest, RootResolvesToRoot)
{
    EXPECT_EQ(resolve(CanonPath("/")).abs(), "/");
}

/* `.` and empty components are stripped by `CanonPath`'s constructor
   before the walker sees them, so resolving `/a/./b` or `/a//b`
   simply walks `/a/b`. Pinned here so any future change that lets
   raw `.` / empty components reach the walker is forced to revisit
   what the walker should do with them. */
TEST_F(ResolveSymlinksTest, DotAndEmptyComponentsStrippedAtCanonPathConstruction)
{
    accessor->addFile(CanonPath("/a/b"), "x");
    EXPECT_EQ(resolve(CanonPath("/a/./b")).abs(), "/a/b");
    EXPECT_EQ(resolve(CanonPath("/a//b")).abs(), "/a/b");
}

/* `..`-past-root in the raw input is stripped by `CanonPath`'s
   constructor (which clamps `..` at root lexically), so by the time
   the walker sees the path it's already `/foo`. Pinned so any future
   strict-mode caller that bypasses `CanonPath`'s clamp can spot the
   shift in responsibility. */
TEST_F(ResolveSymlinksTest, ParentPastRootStrippedAtCanonPathConstruction)
{
    accessor->addFile(CanonPath("/foo"), "x");
    EXPECT_EQ(resolve(CanonPath("/../foo")).abs(), "/foo");
}

/* Relative-target symlink in an ancestor position: `Full` follows it,
   the result lives where the target points. */
TEST_F(ResolveSymlinksTest, RelativeTargetSymlinkFollowedInFullMode)
{
    accessor->addFile(CanonPath("/x/file"), "x");
    sink.createDirectory(CanonPath("/a"));
    sink.createSymlink(CanonPath("/a/link"), "../x");

    EXPECT_EQ(resolve(CanonPath("/a/link/file"), SymlinkResolution::Full).abs(), "/x/file");
}

/* Absolute-target symlink rebases the walker on accessor root (not the
   real-filesystem root) and resolves the rest against that. */
TEST_F(ResolveSymlinksTest, AbsoluteTargetSymlinkRebasesOnAccessorRoot)
{
    accessor->addFile(CanonPath("/x/y/file"), "y");
    sink.createSymlink(CanonPath("/link"), "/x/y");

    EXPECT_EQ(resolve(CanonPath("/link/file"), SymlinkResolution::Full).abs(), "/x/y/file");
}

/* `Ancestors` mode leaves the final component as-is even if it's a
   symlink. `Full` follows it. */
TEST_F(ResolveSymlinksTest, AncestorsLeavesTrailingSymlinkUntouched)
{
    accessor->addFile(CanonPath("/target"), "y");
    sink.createSymlink(CanonPath("/link"), "/target");

    EXPECT_EQ(resolve(CanonPath("/link"), SymlinkResolution::Ancestors).abs(), "/link");
    EXPECT_EQ(resolve(CanonPath("/link"), SymlinkResolution::Full).abs(), "/target");
}

/* `Ancestors` still follows symlinks that are *not* the final
   component — `link` here is an ancestor, so it gets followed even in
   Ancestors mode. */
TEST_F(ResolveSymlinksTest, AncestorsFollowsNonTrailingSymlink)
{
    accessor->addFile(CanonPath("/x/file"), "y");
    sink.createSymlink(CanonPath("/link"), "/x");

    EXPECT_EQ(resolve(CanonPath("/link/file"), SymlinkResolution::Ancestors).abs(), "/x/file");
}

/* On a path with no symlinks, `Full` and `Ancestors` agree. Pins that
   the mode parameter doesn't accidentally do something extra. */
TEST_F(ResolveSymlinksTest, ModeEquivalenceOnPlainPath)
{
    accessor->addFile(CanonPath("/a/b/c"), "x");

    auto fullR = resolve(CanonPath("/a/b/c"), SymlinkResolution::Full);
    auto ancR = resolve(CanonPath("/a/b/c"), SymlinkResolution::Ancestors);
    EXPECT_EQ(fullR.abs(), ancR.abs());
    EXPECT_EQ(fullR.abs(), "/a/b/c");
}

/* Chain of symlinks within the recursion budget resolves. */
TEST_F(ResolveSymlinksTest, ChainOfSymlinksResolves)
{
    accessor->addFile(CanonPath("/final"), "x");
    sink.createSymlink(CanonPath("/a"), "/b");
    sink.createSymlink(CanonPath("/b"), "/c");
    sink.createSymlink(CanonPath("/c"), "/final");

    EXPECT_EQ(resolve(CanonPath("/a"), SymlinkResolution::Full).abs(), "/final");
}

/* A cycle of symlinks blows up with the documented error rather than
   spinning forever. */
TEST_F(ResolveSymlinksTest, CycleHitsRecursionLimit)
{
    sink.createSymlink(CanonPath("/a"), "/b");
    sink.createSymlink(CanonPath("/b"), "/a");

    EXPECT_THROW(resolve(CanonPath("/a"), SymlinkResolution::Full), Error);
}

/* A relative symlink target whose `..` would escape past the
   accessor root gets clamped silently — same as `..` in the input. */
TEST_F(ResolveSymlinksTest, RelativeTargetParentPastRootClampsSilently)
{
    accessor->addFile(CanonPath("/foo"), "x");
    sink.createSymlink(CanonPath("/link"), "../foo");

    /* Walk: push "link" → res=/link; readLink="../foo"; not absolute,
       pop res to /; splice ["..", "foo"] into todo. Next iter: ".."
       at root, clamp silently. Next iter: push "foo" → res=/foo. */
    EXPECT_EQ(resolve(CanonPath("/link"), SymlinkResolution::Full).abs(), "/foo");
}

/* Splice-after-splice: a path whose first component is an absolute
   symlink, and the resulting path runs into *another* absolute
   symlink. Exercises the loop where the position reached via one
   splice itself contains a symlink that triggers another splice. */
TEST_F(ResolveSymlinksTest, ChainOfAncestorSymlinksThroughSplice)
{
    /* Layout:
         /p/q is a directory containing a file `target`.
         /a -> /p (absolute symlink)
         /p/q/link -> /p/q/target (relative target reached via splice)
       Resolving "/a/q/link" walks:
         push a (lstat: symlink) -> target "/p", res=/, splice [p].
         push p (lstat: dir).
         push q (lstat: dir).
         push link (lstat: symlink) -> target "/p/q/target", res=/, splice [p, q, target].
         push p, push q, push target — final res = /p/q/target.
       Two splices in one walk, the second arising from a position
       (/p/q/link) reached via the first splice.  */
    accessor->addFile(CanonPath("/p/q/target"), "x");
    sink.createSymlink(CanonPath("/a"), "/p");
    sink.createSymlink(CanonPath("/p/q/link"), "/p/q/target");

    EXPECT_EQ(resolve(CanonPath("/a/q/link")).abs(), "/p/q/target");
}

/* Ancestors mode resolves all ancestor symlinks but leaves the
   trailing component as-is. Critically, this includes the case where
   a *trailing symlink itself points further* — Ancestors must stop
   before that further follow. Full continues. */
TEST_F(ResolveSymlinksTest, AncestorsStopsBeforeTrailingSymlinkChain)
{
    accessor->addFile(CanonPath("/y"), "y");
    sink.createDirectory(CanonPath("/x"));
    sink.createSymlink(CanonPath("/link"), "/x");
    sink.createSymlink(CanonPath("/x/leaf"), "/y");

    /* Ancestors: /link is ancestor (followed → /x), leaf is trailing
       (not followed). Result: /x/leaf. */
    EXPECT_EQ(resolve(CanonPath("/link/leaf"), SymlinkResolution::Ancestors).abs(), "/x/leaf");

    /* Full: /link is followed to /x, /x/leaf is followed to /y. */
    EXPECT_EQ(resolve(CanonPath("/link/leaf"), SymlinkResolution::Full).abs(), "/y");
}

/* The walker doesn't validate that intermediate components are
   directories. It only inspects them for symlink-ness. Pushing past
   a regular file yields a path that doesn't exist, but the walk
   itself doesn't error — `resolveSymlinks`'s contract is
   "resolve any symlinks", not "verify the path is reachable". */
TEST_F(ResolveSymlinksTest, WalkPastRegularFileDoesNotError)
{
    accessor->addFile(CanonPath("/file"), "x");

    /* Resolves to /file/extra even though /file is not a directory. */
    EXPECT_NO_THROW(resolve(CanonPath("/file/extra")));
    EXPECT_EQ(resolve(CanonPath("/file/extra")).abs(), "/file/extra");
}

/* A relative-target symlink whose target contains `.` components:
   the splice produces those components in the todo list, and the
   walker's skip-`.` logic must apply to them. This is the only way
   for `.` components to reach the walker (the input CanonPath strips
   them at construction). Empty tokens never reach the walker either
   way — the tokeniser the splice runs through absorbs them. */
TEST_F(ResolveSymlinksTest, SymlinkTargetWithDotComponentsSkipped)
{
    accessor->addFile(CanonPath("/x/file"), "y");
    /* Relative target "././x" tokenises to [".", ".", "x"]; the two
       leading `.`s are skipped, leaving just "x". */
    sink.createSymlink(CanonPath("/link"), "././x");

    /* Walk: push link → res=/link; readLink="././x"; not absolute;
       pop res to /; splice [".", ".", "x"] into todo. Skip the two
       `.`s, push "x" → res=/x. Then push "file" → res=/x/file. */
    EXPECT_EQ(resolve(CanonPath("/link/file"), SymlinkResolution::Full).abs(), "/x/file");
}

/* A broken symlink (target that doesn't exist as anything in the
   accessor) still resolves "successfully" — the walker reads the
   target and produces whatever the splice computes. Existence
   checking is not part of the contract. */
TEST_F(ResolveSymlinksTest, BrokenSymlinkResolvesToTargetPath)
{
    sink.createSymlink(CanonPath("/broken"), "/no/such/place");

    EXPECT_EQ(resolve(CanonPath("/broken"), SymlinkResolution::Full).abs(), "/no/such/place");
}

/* A trailing slash on input is absorbed by `tokenizeString` (which
   collapses runs of separators), so no empty component reaches the
   walker — the end-to-end result matches the un-slashed form via the
   tokeniser, not via any walker-level empty-skip. */
TEST_F(ResolveSymlinksTest, TrailingSlashIgnored)
{
    accessor->addFile(CanonPath("/foo"), "x");

    EXPECT_EQ(nix::resolveSymlinks(*accessor, std::string_view{"/foo/"}).abs(), "/foo");
    /* CanonPath strips the trailing slash itself, so this is redundant
       at the CanonPath level — but the test pins that both paths
       agree on the same input shape. */
    EXPECT_EQ(nix::resolveSymlinks(*accessor, CanonPath("/foo/")).abs(), "/foo");
}

/* Boundary inputs: empty string, single slash, just `.`. Each should
   resolve to root without throwing. */
TEST_F(ResolveSymlinksTest, EmptyAndRootLikeInputs)
{
    EXPECT_EQ(nix::resolveSymlinks(*accessor, std::string_view{""}).abs(), "/");
    EXPECT_EQ(nix::resolveSymlinks(*accessor, std::string_view{"/"}).abs(), "/");
    EXPECT_EQ(nix::resolveSymlinks(*accessor, std::string_view{"."}).abs(), "/");
    EXPECT_EQ(nix::resolveSymlinks(*accessor, std::string_view{"/."}).abs(), "/");
}

/* The raison d'être of the `std::string_view` overload: it preserves
   `..` components so the walker can apply symlink-aware semantics,
   producing a different (and correct) answer than the `CanonPath`
   overload when a component along the way is a symlink.

   Setup: `/a` is an absolute symlink to `/x/y`, and there's a regular
   file at `/x/file`. The POSIX walk of `/a/../file` follows `/a` to
   `/x/y`, takes `..` from there to `/x`, then `file` → `/x/file`.

   The `std::string_view` overload tokenises raw, sees the `..` after
   the symlink follow, and produces `/x/file`. The `CanonPath`
   overload pre-strips `/a/..` lexically inside the constructor, so
   the walker never sees the symlink and the result is `/file` — a
   path that doesn't exist as anything in this accessor. */
TEST_F(ResolveSymlinksTest, RawStringPreservesDotDotForSymlinkAwareSemantics)
{
    accessor->addFile(CanonPath("/x/file"), "y");
    sink.createSymlink(CanonPath("/a"), "/x/y");

    auto walked = nix::resolveSymlinks(*accessor, std::string_view{"/a/../file"}, SymlinkResolution::Full);
    EXPECT_EQ(walked.abs(), "/x/file");

    /* Pin the lexical-clamp behaviour of the `CanonPath` overload on
       the same input so a future change that fixes either side
       surfaces here. */
    auto lexical = nix::resolveSymlinks(*accessor, CanonPath("/a/../file"), SymlinkResolution::Full);
    EXPECT_EQ(lexical.abs(), "/file");

    EXPECT_NE(walked.abs(), lexical.abs());
}

/* Sanity: on input that's already canonical (no `..`, no `.`, no
   empty components), the two overloads must agree. */
TEST_F(ResolveSymlinksTest, OverloadsAgreeOnCanonicalInput)
{
    accessor->addFile(CanonPath("/a/b/c"), "x");

    auto fromString = nix::resolveSymlinks(*accessor, std::string_view{"/a/b/c"});
    auto fromCanon = nix::resolveSymlinks(*accessor, CanonPath("/a/b/c"));
    EXPECT_EQ(fromString.abs(), fromCanon.abs());
    EXPECT_EQ(fromString.abs(), "/a/b/c");
}

/* ------------------------------------------------------------------
 * Callbacks
 *
 * The string_view overload accepts a `Callbacks` template parameter
 * whose hooks are called at semantically interesting transitions.
 * Tests below use `RecordingCallbacks` (accumulate events into a
 * vector) and `ThrowingCallbacks` (throw on the Nth event).
 * ------------------------------------------------------------------ */

struct RecordedEvent
{
    enum Kind {
        Parent,
        AbsoluteSymlink,
    };

    Kind kind;
    /* For Parent: path before pop. For AbsoluteSymlink: link site. */
    CanonPath path;
    /* For AbsoluteSymlink only. Empty for Parent. */
    std::string target;

    friend std::ostream & operator<<(std::ostream & os, const RecordedEvent & e)
    {
        os << (e.kind == Parent ? "Parent(" : "AbsoluteSymlink(") << e.path.abs();
        if (e.kind == AbsoluteSymlink)
            os << ", " << e.target;
        return os << ")";
    }
};

struct RecordingCallbacks : NoOpResolveSymlinksCallbacks
{
    std::vector<RecordedEvent> & events;

    explicit RecordingCallbacks(std::vector<RecordedEvent> & e)
        : events(e)
    {
    }

    void onParent(const CanonPath & current) const
    {
        events.push_back({RecordedEvent::Parent, current, {}});
    }

    void onAbsoluteSymlink(const CanonPath & link, std::string_view target) const
    {
        events.push_back({RecordedEvent::AbsoluteSymlink, link, std::string{target}});
    }
};

class ResolveSymlinksCallbacksTest : public ResolveSymlinksTest
{
protected:
    std::vector<RecordedEvent> events;
    RecordingCallbacks recording{events};

    CanonPath resolveRecording(std::string_view rawPath, SymlinkResolution mode = SymlinkResolution::Full)
    {
        return nix::resolveSymlinks(*accessor, rawPath, mode, recording);
    }
};

/* A plain path with no symlinks and no `..` fires no events. */
TEST_F(ResolveSymlinksCallbacksTest, NoEventsOnPlainPath)
{
    accessor->addFile(CanonPath("/a/b/c"), "x");

    EXPECT_EQ(resolveRecording("/a/b/c").abs(), "/a/b/c");
    EXPECT_TRUE(events.empty());
}

/* `.` and empty components are skipped silently — no `onParent` or
   `onAbsoluteSymlink` events for them. */
TEST_F(ResolveSymlinksCallbacksTest, NoEventsForDotOrEmpty)
{
    accessor->addFile(CanonPath("/a/b"), "x");

    EXPECT_EQ(resolveRecording("/a/./b").abs(), "/a/b");
    EXPECT_EQ(resolveRecording("/a//b").abs(), "/a/b");
    EXPECT_TRUE(events.empty());
}

/* A single `..` in the input fires exactly one Parent event with
   `current` set to the path before the pop. */
TEST_F(ResolveSymlinksCallbacksTest, ParentEventFiresOnDotDotInInput)
{
    accessor->addFile(CanonPath("/a/file"), "x");
    sink.createDirectory(CanonPath("/a/b"));

    EXPECT_EQ(resolveRecording("/a/b/..").abs(), "/a");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, RecordedEvent::Parent);
    EXPECT_EQ(events[0].path.abs(), "/a/b");
}

/* A `..` whose pop would escape past root fires the event with
   `current = /`. The resolver still clamps silently (the callback
   doesn't change behaviour unless it throws). */
TEST_F(ResolveSymlinksCallbacksTest, ParentEventFiresAtRootBeforeClamp)
{
    accessor->addFile(CanonPath("/foo"), "x");

    EXPECT_EQ(resolveRecording("/../foo").abs(), "/foo");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, RecordedEvent::Parent);
    EXPECT_EQ(events[0].path.abs(), "/");
}

/* Each `..` fires its own event, with `current` set to the path
   before its pop. */
TEST_F(ResolveSymlinksCallbacksTest, ParentEventsFirePerDotDot)
{
    accessor->addFile(CanonPath("/foo"), "x");
    sink.createDirectory(CanonPath("/a/b/c"));

    EXPECT_EQ(resolveRecording("/a/b/c/../../foo").abs(), "/a/foo");
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].kind, RecordedEvent::Parent);
    EXPECT_EQ(events[0].path.abs(), "/a/b/c");
    EXPECT_EQ(events[1].kind, RecordedEvent::Parent);
    EXPECT_EQ(events[1].path.abs(), "/a/b");
}

/* A `..` that arrives via a symlink target's splice also fires the
   event. With `/link -> ".."`, walking `/link` pops to root, splices
   `[".."]`, processes `..` against root → event with `current = /`. */
TEST_F(ResolveSymlinksCallbacksTest, ParentEventFiresOnDotDotFromSymlinkTarget)
{
    sink.createSymlink(CanonPath("/link"), "..");

    EXPECT_EQ(resolveRecording("/link").abs(), "/");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, RecordedEvent::Parent);
    EXPECT_EQ(events[0].path.abs(), "/");
}

/* Following an absolute-target symlink fires AbsoluteSymlink with
   `link` = symlink site and `target` = target string. */
TEST_F(ResolveSymlinksCallbacksTest, AbsoluteSymlinkEventFiresBeforeRebase)
{
    accessor->addFile(CanonPath("/x/y"), "y");
    sink.createSymlink(CanonPath("/link"), "/x/y");

    EXPECT_EQ(resolveRecording("/link").abs(), "/x/y");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, RecordedEvent::AbsoluteSymlink);
    EXPECT_EQ(events[0].path.abs(), "/link");
    EXPECT_EQ(events[0].target, "/x/y");
}

/* A relative-target symlink does *not* fire AbsoluteSymlink (it uses
   the pop+splice path, not the reset-to-root path). Use a target with
   no `..`s, so we can assert the events vector is genuinely empty —
   any `..` in a relative target would still legitimately fire Parent. */
TEST_F(ResolveSymlinksCallbacksTest, RelativeSymlinkDoesNotFireAbsoluteEvent)
{
    accessor->addFile(CanonPath("/x"), "y");
    sink.createSymlink(CanonPath("/link"), "x");

    EXPECT_EQ(resolveRecording("/link").abs(), "/x");
    EXPECT_TRUE(events.empty());
}

/* Events fire in walk order: parent before each `..`, absolute-symlink
   before its rebase. A mixed input produces them interleaved. */
TEST_F(ResolveSymlinksCallbacksTest, EventOrderMatchesWalk)
{
    /* `/a -> /x` (absolute). Walking `/a/../y`:
         push a → res=/a; symlink → event(AbsoluteSymlink, /a, /x),
                       res=/, splice [x]; todo=[x, .., y].
         push x → res=/x.
         process .. → event(Parent, /x), pop to /.
         push y → res=/y.
       Expected: AbsoluteSymlink, Parent. */
    sink.createSymlink(CanonPath("/a"), "/x");

    EXPECT_EQ(resolveRecording("/a/../y").abs(), "/y");
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].kind, RecordedEvent::AbsoluteSymlink);
    EXPECT_EQ(events[0].path.abs(), "/a");
    EXPECT_EQ(events[0].target, "/x");
    EXPECT_EQ(events[1].kind, RecordedEvent::Parent);
    EXPECT_EQ(events[1].path.abs(), "/x");
}

/* A callback may throw to refuse — the exception propagates out of
   `resolveSymlinks` unchanged. */
struct ThrowingOnParent : NoOpResolveSymlinksCallbacks
{
    struct Refused : std::runtime_error
    {
        Refused()
            : std::runtime_error("refused parent traversal")
        {
        }
    };

    void onParent(const CanonPath &) const
    {
        throw Refused{};
    }
};

TEST_F(ResolveSymlinksTest, ThrowFromOnParentPropagates)
{
    accessor->addFile(CanonPath("/foo"), "x");
    sink.createDirectory(CanonPath("/a/b"));

    EXPECT_THROW(
        nix::resolveSymlinks(*accessor, std::string_view{"/a/b/../foo"}, SymlinkResolution::Full, ThrowingOnParent{}),
        ThrowingOnParent::Refused);
}

struct ThrowingOnAbsoluteSymlink : NoOpResolveSymlinksCallbacks
{
    struct Refused : std::runtime_error
    {
        Refused()
            : std::runtime_error("refused absolute-symlink follow")
        {
        }
    };

    void onAbsoluteSymlink(const CanonPath &, std::string_view) const
    {
        throw Refused{};
    }
};

TEST_F(ResolveSymlinksTest, ThrowFromOnAbsoluteSymlinkPropagates)
{
    accessor->addFile(CanonPath("/x"), "y");
    sink.createSymlink(CanonPath("/link"), "/x");

    EXPECT_THROW(
        nix::resolveSymlinks(
            *accessor, std::string_view{"/link"}, SymlinkResolution::Full, ThrowingOnAbsoluteSymlink{}),
        ThrowingOnAbsoluteSymlink::Refused);
}

/* Throwing from a hook halts the walk: subsequent events do not fire.
   Set up two `..`s, throw on the second; verify only the first was
   recorded. */
struct ThrowOnNthParent : NoOpResolveSymlinksCallbacks
{
    struct Refused : std::runtime_error
    {
        Refused()
            : std::runtime_error("refused on Nth")
        {
        }
    };

    std::vector<RecordedEvent> & events;
    mutable size_t count = 0;
    size_t throwOn;

    ThrowOnNthParent(std::vector<RecordedEvent> & e, size_t n)
        : events(e)
        , throwOn(n)
    {
    }

    void onParent(const CanonPath & current) const
    {
        events.push_back({RecordedEvent::Parent, current, {}});
        if (++count == throwOn)
            throw Refused{};
    }
};

TEST_F(ResolveSymlinksTest, ThrowFromHookHaltsWalk)
{
    accessor->addFile(CanonPath("/foo"), "x");
    sink.createDirectory(CanonPath("/a/b/c"));
    /* Place an absolute-target symlink at `/a/link` so that if the
       walk continued past the throw, an `onAbsoluteSymlink` event
       would fire. We can then assert that no AbsoluteSymlink event
       was recorded, which actually distinguishes halt-on-throw from
       just-completed. */
    sink.createSymlink(CanonPath("/a/link"), "/foo");

    std::vector<RecordedEvent> events;
    ThrowOnNthParent throwOnSecond{events, 2};

    EXPECT_THROW(
        nix::resolveSymlinks(*accessor, std::string_view{"/a/b/c/../../link"}, SymlinkResolution::Full, throwOnSecond),
        ThrowOnNthParent::Refused);

    /* Two Parent events recorded; the AbsoluteSymlink that would
       have fired on `/a/link` doesn't appear because the walk
       halted at the throw. */
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].kind, RecordedEvent::Parent);
    EXPECT_EQ(events[0].path.abs(), "/a/b/c");
    EXPECT_EQ(events[1].kind, RecordedEvent::Parent);
    EXPECT_EQ(events[1].path.abs(), "/a/b");
    for (const auto & e : events)
        EXPECT_NE(e.kind, RecordedEvent::AbsoluteSymlink);
}

/* ------------------------------------------------------------------
 * StrictAccessorBoundary
 *
 * The strict-escape callback. Throws Error when a `..` would pop past
 * the accessor's root, regardless of whether the `..` came from the
 * input or from a spliced symlink target. Otherwise transparent — non-
 * escaping inputs resolve the same as the no-callback path.
 * ------------------------------------------------------------------ */

class StrictAccessorBoundaryTest : public ResolveSymlinksTest
{
protected:
    CanonPath resolveStrict(std::string_view rawPath, SymlinkResolution mode = SymlinkResolution::Full)
    {
        return nix::resolveSymlinks(*accessor, rawPath, mode, StrictAccessorBoundary{*accessor});
    }
};

/* Non-escaping input resolves the same as without the boundary —
   `..` that stays inside the tree pops normally and doesn't throw. */
TEST_F(StrictAccessorBoundaryTest, NonEscapingResolvesUnchanged)
{
    accessor->addFile(CanonPath("/a/b"), "x");

    EXPECT_EQ(resolveStrict("/a/b").abs(), "/a/b");
    EXPECT_EQ(resolveStrict("/a/../a/b").abs(), "/a/b");
}

/* `..`-at-root in the input throws AccessorBoundaryEscape (the
   dedicated subclass) rather than silently clamping. */
TEST_F(StrictAccessorBoundaryTest, ParentPastRootFromInputThrows)
{
    accessor->addFile(CanonPath("/foo"), "x");

    EXPECT_THROW(resolveStrict("/../foo"), AccessorBoundaryEscape);
}

/* `..` in a relative symlink target that would escape past root
   throws — the policy fires for `..`s introduced via splice, not
   only those literally in the input. */
TEST_F(StrictAccessorBoundaryTest, ParentPastRootFromSymlinkTargetThrows)
{
    accessor->addFile(CanonPath("/foo"), "x");
    sink.createSymlink(CanonPath("/link"), "../foo");

    EXPECT_THROW(resolveStrict("/link"), AccessorBoundaryEscape);
}

/* Absolute symlink target rebases to accessor root — not an escape,
   no throw. */
TEST_F(StrictAccessorBoundaryTest, AbsoluteSymlinkRebaseDoesNotThrow)
{
    accessor->addFile(CanonPath("/x"), "y");
    sink.createSymlink(CanonPath("/link"), "/x");

    EXPECT_NO_THROW(resolveStrict("/link"));
    EXPECT_EQ(resolveStrict("/link").abs(), "/x");
}

/* The error message names the source tree at the moment of the would-be
   escape. Compare against the same HintFmt the policy throws — the
   reader of this test sees the exact format string and arguments. */
TEST_F(StrictAccessorBoundaryTest, ErrorMessageNamesPathAtEscape)
{
    accessor->addFile(CanonPath("/foo"), "x");
    accessor->setPathDisplay("«fixture»");

    try {
        resolveStrict("/../foo");
        FAIL() << "expected throw";
    } catch (const AccessorBoundaryEscape & e) {
        EXPECT_EQ(e.info().msg.str(), HintFmt("'..' would escape the source tree at %s", "«fixture»/").str());
    }
}

} // namespace nix
