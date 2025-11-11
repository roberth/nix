# Phase 4: Remaining Fetchers Analysis

## Summary

**Completed:**
- ✅ PathInputScheme - Fully refactored using typed inputs (committed: 7e56c039a)
- ✅ IndirectInputScheme - No refactoring needed (just throws error, no attrs manipulation)

**Remaining:** 4 complex fetchers that need typed refactoring

---

## Remaining Fetchers

### 1. GitInputScheme (src/libfetchers/git.cc)

**Complexity:** HIGH - Most complex fetcher

**Current Structure:**
- Line 884: `getAccessor()` delegates to:
  - `getAccessorFromCommit()` (line 626) - Handles remote/cached repos
  - `getAccessorFromWorkdir()` (line 800) - Handles local working directories
- Both methods mutate input.attrs with insert_or_assign
- Handles shallow clones, submodules, LFS, exportIgnore
- Complex caching and ref resolution logic

**Attrs Mutations:**
```cpp
// getAccessorFromCommit
input.attrs.insert_or_assign("ref", ref);  // line 635
input.attrs.insert_or_assign("rev", ...);   // lines 642, 717
input.attrs.insert_or_assign("lastModified", ...); // later
input.attrs.insert_or_assign("revCount", ...);
```

**Refactoring Strategy:**
1. Create `lockTypedFromCommit(store, GitUnlockedInput) -> GitLockedInput`
2. Create `lockTypedFromWorkdir(store, GitUnlockedInput) -> GitLockedInput`
3. Main `lockTyped()` dispatches to one of these
4. `getAccessor()` becomes wrapper with boundary conversions
5. Need to handle the complex state: ref resolution, caching, shallow vs full

**Estimated Effort:** 3-4 hours (most complex due to submodules, caching, workdir vs commit paths)

---

### 2. TarballInputScheme (src/libfetchers/tarball.cc)

**Complexity:** MEDIUM-HIGH

**Current Structure:**
- Line 380: `getAccessor()` - Downloads tarball and extracts
- Handles HTTP redirects via "immutableUrl"
- Two schemes: TarballInputScheme and FileInputScheme (line 343)

**Attrs Mutations:**
```cpp
// TarballInputScheme::getAccessor
input.attrs.insert_or_assign("lastModified", ...);  // line 397
input.attrs.insert_or_assign("narHash", ...);        // line 399

// FileInputScheme::getAccessor
input.attrs.insert_or_assign("narHash", ...);        // line 354
```

**Special Handling:**
- Line 387-394: If immutableUrl redirect occurs, replaces entire input
- This is tricky: `input = immutableInput;` (line 393)

**Refactoring Strategy:**
1. Create `lockTyped(store, TarballUnlockedInput) -> TarballLockedInput`
2. Handle immutableUrl case by creating new typed input from URL
3. `getAccessor()` wrapper with conversions
4. FileInputScheme gets similar treatment

**Estimated Effort:** 2-3 hours (redirect handling adds complexity)

---

### 3. GitHubInputScheme (src/libfetchers/github.cc)

**Complexity:** MEDIUM

**Current Structure:**
- Downloads tarballs from GitHub/GitLab API
- Simpler than Git (no local repos, no submodules)
- Uses `downloadTarball()` helper

**Expected Attrs Mutations:**
- Similar to tarball: narHash, lastModified, rev
- Likely treeHash for GitHub's tree ID

**Refactoring Strategy:**
1. Create `lockTyped(store, GitHubUnlockedInput) -> GitHubLockedInput`
2. Straightforward conversion of download + metadata extraction
3. `getAccessor()` wrapper

**Estimated Effort:** 1-2 hours (simpler than Git/Tarball)

---

### 4. MercurialInputScheme (src/libfetchers/mercurial.cc)

**Complexity:** MEDIUM

**Current Structure:**
- Similar to Git but for Mercurial VCS
- Likely has caching, rev resolution
- Probably simpler than Git (Mercurial is simpler than Git)

**Expected Pattern:**
- Similar to Git but without submodules/LFS complexity
- Cache management
- Rev/ref resolution

**Refactoring Strategy:**
1. Create `lockTyped(store, MercurialUnlockedInput) -> MercurialLockedInput`
2. Similar to Git but likely simpler
3. `getAccessor()` wrapper

**Estimated Effort:** 2-3 hours (similar to Git but less complex)

---

## Total Estimated Remaining Effort

**Development Time:** 8-12 hours
- GitInputScheme: 3-4 hours
- TarballInputScheme: 2-3 hours
- GitHubInputScheme: 1-2 hours
- MercurialInputScheme: 2-3 hours

**Testing Time:** 2-3 hours
- Each fetcher needs tests
- Integration testing with flakes
- Verify backward compatibility

**Total:** 10-15 hours of focused development work

---

## Pattern Established

PathInputScheme proves the pattern works:

```cpp
// Typed method - primary implementation (no Attrs!)
std::pair<ref<SourceAccessor>, PathLockedInput>
lockTyped(ref<Store> store, const PathUnlockedInput & input) const {
    // Pure typed logic
    auto absPath = getAbsPath(input.path);
    // ... work ...

    // Construct new locked input (immutable)
    LockingMetadata locking;
    locking.lastModified = input.lastModified.value_or(mtime);
    PathLockedInput locked(*input.settings, input.path, locking);
    return {accessor, std::move(locked)};
}

// Wrapper for backward compatibility
std::pair<ref<SourceAccessor>, Input>
getAccessor(ref<Store> store, const Input & input) const override {
    // Boundary: Attrs → typed
    auto unlocked = pathInputFromAttrs(*input.settings, input.attrs);

    // Pure typed logic
    auto [accessor, locked] = lockTyped(store, unlocked);

    // Boundary: typed → Attrs
    Input result(input);  // CRITICAL: copy to preserve scheme!
    result.attrs = pathInputToAttrs(locked);
    return {accessor, std::move(result)};
}
```

**Key Principles:**
1. ✅ Zero `insert_or_assign` in typed method
2. ✅ Immutable pattern (construct new, don't mutate)
3. ✅ State transitions explicit in signatures
4. ✅ All Attrs manipulation in wrapper only
5. ✅ **Copy input to preserve scheme field**

---

## Recommendation

The remaining fetchers are significantly more complex than Path:
- **Git**: Multiple code paths, submodules, caching, workdir vs commit
- **Tarball**: HTTP redirects, immutableUrl replacement
- **GitHub**: API interaction, treeHash handling
- **Mercurial**: VCS operations, caching

Each requires careful refactoring to avoid breaking existing functionality.

**Suggested Approach:**
1. Start with GitHub (simplest of remaining)
2. Then Tarball (medium complexity)
3. Then Mercurial (Git-like but simpler)
4. Finally Git (most complex, save for last)

**Benefits So Far:**
- ✅ Pattern established and proven with Path
- ✅ All conversion infrastructure in place
- ✅ Type system fully designed and implemented
- ✅ Tests passing, backward compatible

The foundation is solid. The remaining work is systematic application of the proven pattern to more complex fetchers.
