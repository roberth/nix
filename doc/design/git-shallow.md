# Git Shallow Fetching

This document describes the design space around the `shallow` parameter in fetchGit/fetchTree, the history of changes, key opinions from contributors, and considerations for future work.

## Overview

The `shallow` parameter controls whether Nix performs shallow Git fetches (using `--depth 1`) rather than full clones. This can dramatically reduce network transfer and disk usage—in some cases by 99%+ for repositories with large histories.

For example, fetching a single revision of the PHP static analyzer `phpstan` requires fetching 4.17 GiB with a full clone, but only 13.40 MiB with shallow fetching—a reduction of ~99.7%.

## The Three Behaviors

The `shallow` parameter can affect three distinct behaviors, which have historically been conflated:

1. **Tolerate shallow repositories**: Accept that a local repository may have incomplete history (the original meaning from 2020)

2. **Perform shallow fetches**: Use `--depth 1` when fetching from a remote, transferring only the tree objects for a single commit

3. **Adjust return attributes**: Skip computing `revCount`, which requires walking the full commit history

These behaviors are related but not identical. A repository can be shallow without having been fetched shallowly (e.g., a user's local shallow clone), and computing `revCount` requires full history regardless of how the repository was obtained.

## The `revCount` Problem

The `revCount` attribute is the primary obstacle to shallow-by-default fetching:

- **What it is**: The number of commits reachable from the current revision
- **Use case**: Generating monotonically increasing version numbers (e.g., `1.0.0-r1234`)
- **Problem**: Computing it requires the full commit ancestry graph
- **Alternatives**: `shortRev` (first 7 characters of the commit hash) can serve similar purposes

### Treeless Clones Considered

Fetching the commit history without trees ("treeless" clones via `--filter=tree:0`) was benchmarked by @DavHau ([results in issue #9402](https://github.com/NixOS/nix/issues/9402#issuecomment-1833228894)):

| Metric | Shallow Only | Shallow + History (Treeless) |
|--------|-------------|------------------------------|
| Network transfer | Baseline | 3.7x-4x higher |
| Objects fetched (nixpkgs) | ~60k (tree) | ~500k (commits) + ~60k (tree) |

Additional findings:
- Some providers (e.g., Bitbucket) don't support `--filter`, falling back to full clones
- Fetching commit history showed inconsistent performance, possibly due to server-side prioritization
- The overhead scales with repository history size

## Server Compatibility

Shallow fetching by revision requires the server to have `uploadpack.allowReachableSHA1InWant` enabled. This is disabled by default in Git.

**Known server support:**
| Server | Shallow by SHA | Notes |
|--------|---------------|-------|
| GitHub | Yes | Enabled |
| GitLab (gitlab.com) | No | Disabled by default |
| Bitbucket | No | Also lacks `--filter` support |
| Sourcehut (git.sr.ht) | No | |
| Codeberg | No | |
| git.savannah.gnu.org | No | |

When disabled, git returns: "Server does not allow request for unadvertised object"

This incompatibility was the primary reason a revert was proposed. See [issue #11012](https://github.com/NixOS/nix/issues/11012) for details.

## Current State (Nix 2.23–2.33+)

**The shallow-by-default behavior is still in effect**, despite the server compatibility issues discovered in July 2024.

The code in `src/libexpr/primops/fetchTree.cc` still contains:
```cpp
// fetchTree should fetch git repos with shallow = true by default
if (type == "git" && !params.isFetchGit && !attrs.contains("shallow")) {
    attrs.emplace("shallow", Explicit<bool>{true});
}
```

This means:
- **All releases from 2.23.0 onward** have `shallow = true` by default for `fetchTree` git inputs
- `builtins.fetchGit` still defaults to `shallow = false`
- **No fallback exists** when the server doesn't support `uploadpack.allowReachableSHA1InWant`
- Fetching non-tip revisions on incompatible servers **will fail**

### What Happened

1. [PR #10028](https://github.com/NixOS/nix/pull/10028) merged shallow-by-default (June 2024)
2. [Issue #11012](https://github.com/NixOS/nix/issues/11012) reported server compatibility issues (July 2024)
3. [PR #11172](https://github.com/NixOS/nix/pull/11172) proposed a revert (July 2024)
4. The revert was **closed without merging**—the decision was to implement a fallback instead
5. **The fallback was never implemented**

### Workaround

Users must explicitly pass `shallow = false` (or `?shallow=0` in flake URLs) when fetching non-tip revisions from servers that don't support shallow fetching by SHA.

### Pending Fix

[PR #14633](https://github.com/NixOS/nix/pull/14633) proposes removing the `fetchTree` shallow hack and making shallow fetching automatic when appropriate (e.g., when `revCount` is already known from a lockfile). This PR is still open.

## Historical Timeline

### February 2018: First Request

[Issue #1837](https://github.com/NixOS/nix/issues/1837) (@shlevy): "fetchGit should do shallow fetches when possible"

> Especially as many git servers, including github, have allowReachableSHA1InWant, fetchGit should just try fetching the specific rev or the HEAD of the specific ref first, and only then fall back to a fuller fetch.

Key exchange:
- **@edolstra**: "Are shallow fetches incremental?"
- **@shlevy**: "Yep"

### July 2020: Initial `shallow` Parameter

Commit `7d554f295` (@edolstra): "Support building flakes from a shallow Git repo"

**Purpose**: Allow Nix to work with repositories that were *already* shallow, not to perform shallow fetches.

**Implementation**: Detected `.git/shallow` file existence and set `shallow=1` automatically in the URL query.

**@edolstra's comment** on [#1837](https://github.com/NixOS/nix/issues/1837#issuecomment-652404447):
> Shallow clones are now supported. However we can't make it the default because it would break the `revCount` attribute.

### January 2021: Feature Request and Confusion

[Issue #4455](https://github.com/NixOS/nix/issues/4455): "Allow performing shallow clones with builtins.fetchGit"

**@edolstra's response**: Points to `builtins.fetchTree { type = "git"; shallow = true; ... }`

**Problem**: This only allowed fetching from *already-shallow remote repos*, not performing shallow fetches from non-shallow repos. Users expected the former behavior.

### August 2021: Bug Report Confirms the Problem

[Issue #5119](https://github.com/NixOS/nix/issues/5119) (@L-as): "Nix doesn't fetch repositories shallowly when told to"

Demonstration that `shallow=1` in the URL had no effect on fetch behavior.

**Key observation** by @ImBearChild (October 2023, [comment](https://github.com/NixOS/nix/issues/5119#issuecomment-1766559866)):
> The `shallow` parameter is never used when calling "git fetch". So it has no impact on how git fetches remote content. It just checks if the local git repository is a "shallow repository" or not.

### June 2022: First Documentation

[PR #6727](https://github.com/NixOS/nix/pull/6727) (@symphorien): "fetchGit: document `shallow` argument"

Added documentation but didn't clarify the confusion about what `shallow` actually did.

### November 2023: Major Refactoring

#### libgit2 Integration and Shallow Fetching Implementation

[PR #9240](https://github.com/NixOS/nix/pull/9240) (@edolstra): "Introduce libgit2" — major refactor of git fetching.

Key commits in November 2023:
- `7ab91e723` (@edolstra): "Implement shallow fetching" — actual `--depth 1` support added
- `e4066c044` (@edolstra): "Fetch specific Git revisions" — enables fetching by SHA directly
- `071f14a0b` (@edolstra): "Don't do shallow fetches over ssh" — first limitation discovered

[PR #9410](https://github.com/NixOS/nix/pull/9410) (@edolstra): "Some shallow git fetching improvements" — merged November 21.

#### DavHau's Comprehensive Analysis

[PR #9376](https://github.com/NixOS/nix/pull/9376) (@DavHau): "fetchGit/fetchTree: add support for shallow cloning"

Key insight from the PR description:
> The current fetchGit/fetchTree builtin already offers a `shallow` flag, but this only allows fetching remote shallow repos. It does not support shallow fetching from non-shallow remote repos. This has been confusing to a number of users, because they expected the `shallow` flag to behave similar to `git clone --depth 1`.

Proposed a new `shallowRev = true` parameter to clarify the distinction.

[PR #9390](https://github.com/NixOS/nix/pull/9390) (@DavHau): "fetchTree: clarify docs for shallow flag" — merged November 20, backported to 2.15-2.19.

#### Central Tracking Issue Created

[Issue #9402](https://github.com/NixOS/nix/issues/9402) (@Ericson2314): "Shallowness of git fetching"

Two approaches proposed:

1. **Treeless route**: Always fetch commit history (spine) but only one tree
   - Supports `revCount`
   - Adds 3.7x+ overhead
   - Requires server filter support

2. **Conditional history**: Make `revCount` optional
   - Default for `fetchTree`: no `revCount`
   - Default for `fetchGit`: keep `revCount` for backwards compatibility

**@thufschmitt's response** ([comment](https://github.com/NixOS/nix/issues/9402#issuecomment-1833288668)):
> Pursuing 2.ii seems to be the right call indeed given your benchmarks.

### January 2024: Authentication Crisis and Re-enablement

#### Git Auth Breaks

[PR #9806](https://github.com/NixOS/nix/pull/9806) (@roberth): "Fix git auth"

The libgit2 integration broke credential helper support. This PR reverted to using the git CLI for fetching, which inadvertently removed shallow fetching support.

**@DavHau's comment** ([link](https://github.com/NixOS/nix/pull/9806#issuecomment-1899853415)):
> It shouldn't be an issue to add it back... Another problem to fix: never share the same cache between shallow and non-shallow clones of the same repo, otherwise we will trip into certain issues when computing revCounts etc.

#### Shallow Fetching Re-enabled

[PR #9811](https://github.com/NixOS/nix/pull/9811) (@DavHau): "fetchTree/fetchGit: re-enable shallow fetching" — merged January 19.

**Team discussion** (from [comment](https://github.com/NixOS/nix/pull/9811#issuecomment-1900604313)):
- @edolstra: Worried this entrenches us in git-cli specific behavior
- @thufschmitt: Unlikely we can use libgit everywhere given missing features
- @Ericson2314: Could we add missing features to libgit2? (Conclusion: too much effort)
- Agreement to merge

#### Documentation Update

[PR #9704](https://github.com/NixOS/nix/pull/9704) (@fricklerhandwerk): "update docs on `fetchGit` shallow clone behavior" — merged January 26.

### February–June 2024: Shallow-by-Default

[PR #10028](https://github.com/NixOS/nix/pull/10028) (@DavHau): "fetchTree: shallow git fetching by default"

**@edolstra's feedback** (February 23, [comment](https://github.com/NixOS/nix/pull/10028#issuecomment-1961393188)):
> Team discussion: Idea approved. This needs a release notes entry. Are there any potential performance issues with switching to shallow fetching? E.g. is there a possibility that incremental fetching doesn't work as well if the client doesn't have the whole history?

**@DavHau's response** ([comment](https://github.com/NixOS/nix/pull/10028#issuecomment-1968305423)):
> Yes, generally it is possible to have scenarios where the lack of incrementality of shallow fetching leads to more overall network traffic... Though it is worth mentioning that non-incremental fetching currently already seems to be the preferred choice overall. There is probably a reason why github flake inputs are fetched via the tarball API and not using git full cloning.

**Merged June 3, 2024** — included in Nix 2.23.

### July 2024: Server Compatibility Crisis

#### Problem Discovered

[Issue #11012](https://github.com/NixOS/nix/issues/11012) (@marienz): "Shallow fetches of specific revs do not work with all servers"

> The new shallow-by-default fetches introduced in Nix 2.23 do not seem to work with all servers... I suspect but have not fully confirmed that it does not work with servers that do not have `uploadpack.allowReachableSHA1InWant` enabled.

**@marienz's observation** ([comment](https://github.com/NixOS/nix/issues/11012#issuecomment-2230571639)):
> I'd argue `shallow = true` may be too confusing relative to the network/storage savings it provides to be a good default... I had to sprinkle several more `shallow = false` over my fetches as more things failed.

**@terlar's comment** ([link](https://github.com/NixOS/nix/issues/11012#issuecomment-2225459863)):
> I have run into this issue since 2.19. Therefore on CI that works with these repos I have had to pin to 2.18.

#### Revert Proposed But Not Merged

[PR #11172](https://github.com/NixOS/nix/pull/11172) (@roberth): "Revert 'fetchTree: shallow git fetching by default'"

Commit `ccacbd0f8` (@roberth) was prepared but **never merged**:
> It was noted in #11012 that this approach does not work for all Git servers. This could be revisited by evaluating the performance cost of attempting to fetch shallowly first, and non-shallowly as a fallback. Failing when our shallow fetching strategy is not supported by the remote is unfortunately not acceptable.

**Meeting decision** ([comment](https://github.com/NixOS/nix/pull/11172#issuecomment-2255982165)):
> Close this PR and fix the bug by retrying as if `shallow = false;`.

Options discussed:
- Revert shallow-by-default
- Retry the specific error non-shallowly ← **chosen approach**
- Make `revCount` lazy (not good enough for lockfiles, non-trivial to implement)

**Outcome**: PR #11172 was **closed without merging** on 2024-07-29. The decision was to implement a fallback instead of reverting. However, **the fallback was never implemented**.

### 2025: Ongoing Refinements

#### Cache Fixes

[PR #12642](https://github.com/NixOS/nix/pull/12642) (@Mic92): "libfetchers/git: fix caching head when using shallow clones" — merged March 2025.

#### Error Handling Improvements

[PR #13265](https://github.com/NixOS/nix/pull/13265) (@Mic92): "Improve errors when we are trying to access a git repository with partial history (+ fix fetchGit on these repos)" — merged May 2025.

Improved error message when `revCount` computation fails on shallow repos:
> Failed to retrieve the parent of Git commit '...'. This may be due to an incomplete repository history. To resolve this, either enable the shallow parameter in your flake URL (?shallow=1) or set the shallow parameter to true in builtins.fetchGit, or fetch the complete history for this branch.

#### revCount Deprecation Discussion

[PR #13260](https://github.com/NixOS/nix/pull/13260) (@Mic92): "flakes: No longer compute revCount for local git repository by default" — open.

Key opinions:

**@edolstra** ([comment](https://github.com/NixOS/nix/pull/13260#issuecomment-2909047125)):
> BTW, maybe as an alternative we can make revCount computation lazy. I.e. it would only be computed if the flake actually uses the `revCount` attribute of the source tree.

**@Mic92** ([response](https://github.com/NixOS/nix/pull/13260#issuecomment-2909159878)):
> I considered the alternative but decided against it because it would still then try to compute the revCount when we override flake inputs in other flakes in which case revCount will always be recorded and checked in the flake.lock.

**@DavHau** ([comment](https://github.com/NixOS/nix/pull/13260#issuecomment-2909445800)):
> `revCount` should be removed from flakes and `fetchTree`. As expensive as computing `revCount` is, it doesn't justify the benefit. Its only use case seems to be generating a version string dynamically, but the `shortRev` can be used instead.

**@grahamc** ([comment](https://github.com/NixOS/nix/pull/13260#issuecomment-2909497237)):
> FlakeHub prohibits a hard revcount dependency because it loses that data once it is published. In other words, it is unlikely a primarily-flakehub backed corpus would fail if the feature went away.

#### Automatic Shallow Fetching

[Issue #14588](https://github.com/NixOS/nix/issues/14588) (@edolstra): "fetchTree sets shallow = true"

Reports that the `fetchTree` hack of setting `shallow = true` is problematic because it makes `fetchTree` behave inconsistently with other fetches.

[PR #14633](https://github.com/NixOS/nix/pull/14633) (@edolstra): "Avoid unnecessary Git refetches" — open.

New approach:
- Commit `50b013f61`: "Remove fetchTree 'shallow' hack" — the git fetcher now automatically does shallow fetch if `revCount` is already set (e.g., from lockfile)
- Commit `5b7badd00`: "Use non-shallow cache repo if it contains the requested commit" — fixes unnecessary refetches

## Summary of Opinions

| Contributor | Position |
|------------|----------|
| **@edolstra** | Shallow clones supported but can't be default due to `revCount`. Prefers making optimization automatic when `revCount` is already known. |
| **@DavHau** | Strong advocate for shallow-by-default. `revCount` should be removed or made opt-in. Provided benchmarks showing 99%+ savings. |
| **@roberth** | Reverted shallow-default due to server compatibility. Prefers retry fallback strategy. |
| **@Mic92** | Improve error messages for partial history. Make `revCount` lazy with deprecation warning. |
| **@grahamc** | FlakeHub already prohibits `revCount` dependency, suggesting ecosystem is moving away from it. |
| **@thufschmitt** | Agreed with conditional approach (2.ii) based on benchmarks. |

## Design Considerations

### Automatic Shallow Fetching

The most promising current approach ([PR #14633](https://github.com/NixOS/nix/pull/14633)) is to make shallow fetching automatic based on context:

1. **When `revCount` is already known** (e.g., from a lockfile), shallow fetch is safe
2. **When the commit is at a ref tip**, shallow fetch by ref works on all servers
3. **When fetching by arbitrary revision**, shallow fetch may fail on some servers

This avoids forcing users to understand the trade-offs while providing good performance in common cases.

### Cache Coherence

Shallow and non-shallow clones must use separate cache paths to avoid:
- `revCount` computation failures on shallow repos
- Unnecessary re-fetches when switching between shallow/non-shallow

Current implementation appends `-shallow` suffix to cache directory names.

### Fallback Strategy

When shallow fetching fails (server doesn't support it), options are:

1. **Automatic retry**: Retry with full fetch; adds latency but always works
2. **Cache server capability**: Remember which servers support shallow fetches
3. **Fail with guidance**: Error message suggests `shallow = false`

The team decision in July 2024 was to pursue option 1 (automatic retry), but **this was never implemented**. As of Nix 2.33.0, there is no fallback—fetches simply fail on incompatible servers.

### revCount Deprecation Path

If `revCount` were deprecated:

1. Make it lazy (computed only when accessed)
2. Emit warning on access
3. Eventually remove from output attributes

Challenges:
- Lockfiles are strict data, not lazy
- Evaluation cache fingerprints depend on all attributes ([noted by @edolstra](https://github.com/NixOS/nix/pull/13260#issuecomment-3481343465))
- Some projects actively use `revCount` for versioning

## Current Implementation Details

### Attribute Definition

In `src/libfetchers/git.cc`:

```cpp
{
    "shallow",
    {
        .type = "Bool",
        .required = false,
        .doc = R"(
          Make a shallow clone when fetching the Git tree.
          When this is enabled, the options `ref` and `allRefs` have no effect anymore.

          Default: `true`
        )",
    },
},
```

**Note**: The documentation in `git.cc` says default is `true` but the fetcher code defaults to `false` (`maybeGetBoolAttr(...).value_or(false)`). However, `fetchTree.cc` contains a hack that sets `shallow = true` for git inputs, so the effective default for `fetchTree` is `true`. This inconsistency between the fetcher's native default and the `fetchTree` override is the subject of [issue #14588](https://github.com/NixOS/nix/issues/14588).

### Fetch Behavior

In `src/libfetchers/git-utils.cc`, when `shallow = true`:

```cpp
void fetch(const std::string & url, const std::string & refspec, bool shallow) override
{
    Strings gitArgs{"-C", dir.string(), "--git-dir", ".", "fetch", "--progress", "--force"};
    if (shallow)
        append(gitArgs, {"--depth", "1"});
    append(gitArgs, {std::string("--"), url, refspec});
    // ...
}
```

### Cache Path Differentiation

```cpp
std::filesystem::path getCachePath(std::string_view key, bool shallow)
{
    return getCacheDir() / "gitv3"
           / (hashString(HashAlgorithm::SHA256, key).to_string(HashFormat::Nix32, false)
              + (shallow ? "-shallow" : ""));
}
```

### Shallow Detection and Validation

```cpp
auto isShallow = repo->isShallow();

if (isShallow && !getShallowAttr(input))
    throw Error(
        "'%s' is a shallow Git repository, but shallow repositories are only allowed "
        "when `shallow = true;` is specified",
        repoInfo.locationToArg());
```

### revCount Exclusion

When shallow, `revCount` is not computed:

```cpp
if (!getShallowAttr(input))
    input.attrs.insert_or_assign("revCount", getRevCount(settings, repoInfo, repoDir, rev));
```

## References

- [Issue #1837](https://github.com/NixOS/nix/issues/1837): Original 2018 request for shallow fetches
- [Issue #4455](https://github.com/NixOS/nix/issues/4455): 2021 request that revealed confusion
- [Issue #5119](https://github.com/NixOS/nix/issues/5119): Bug report confirming shallow didn't work
- [Issue #9402](https://github.com/NixOS/nix/issues/9402): Central tracking issue with benchmarks and design discussion
- [Issue #11012](https://github.com/NixOS/nix/issues/11012): Server compatibility problems
- [Issue #14588](https://github.com/NixOS/nix/issues/14588): fetchTree shallow hack issue
- [PR #9376](https://github.com/NixOS/nix/pull/9376): DavHau's comprehensive POC (superseded)
- [PR #9811](https://github.com/NixOS/nix/pull/9811): Re-enable shallow fetching after auth fix
- [PR #10028](https://github.com/NixOS/nix/pull/10028): Shallow-by-default (merged then reverted)
- [PR #11172](https://github.com/NixOS/nix/pull/11172): Revert discussion
- [PR #13260](https://github.com/NixOS/nix/pull/13260): revCount deprecation discussion
- [PR #14633](https://github.com/NixOS/nix/pull/14633): Current work on automatic shallow fetching
