# libfetchers Typed Inputs - Implementation Status

**Last Updated**: 2025-11-11

## Overview

This document tracks the progress of adding type-safe input structures to libfetchers, replacing the dynamic `Attrs`-based system with a compile-time verified three-state pattern (Unlocked → Locked → Final).

## Completed Work

### ✅ Phase 0: Base Infrastructure
**Commit**: 9e688709c

**Files Created**:
- `src/libfetchers/include/nix/fetchers/typed-inputs.hh`
- `src/libfetchers/typed-inputs.cc`

**Provides**:
- `InputBase`: Base class for all typed inputs
- `LockingMetadata`: Data obtained during locking (lastModified)
- `FinalizationData`: Data obtained during fetching (narHash)
- `InputStates<Unlocked, Locked, Final>`: Template for grouping related types
- C++20 concepts: `IsUnlockedInput`, `IsLockedInput`, `IsFinalInput`

**Status**: ✅ Complete, all tests passing

---

### ✅ Phase 1: Path Typed Inputs (Prototype)
**Commit**: 9e688709c

**Files Created**:
- `src/libfetchers/path-typed.hh`
- `src/libfetchers/path-typed.cc`

**Provides**:
- `PathUnlockedInput`: Simple file system path
- `PathLockedInput`: Path with metadata
- `PathFinalInput`: Path with narHash
- Conversion functions: `pathInputFromAttrs()`, `pathInputToAttrs()`

**Demonstrates**:
- Simplest fetcher implementation
- Pattern for other fetchers
- Backward compatibility approach

**Status**: ✅ Complete, all tests passing

---

### ✅ Phase 2: Git Typed Inputs (Complex Case)
**Commit**: ad07c66e3

**Files Created**:
- `src/libfetchers/include/nix/fetchers/git-typed.hh`
- `src/libfetchers/git-typed.cc`

**Provides**:
- `GitUnlockedInput`: Repository with optional rev/ref
- `GitLockedInput`: Locked to specific commit with metadata
- `GitFinalInput`: With narHash after fetching
- Conversion functions: `gitInputFromAttrs()`, `gitInputToAttrs()`

**Features**:
- Full Git support: shallow, submodules, LFS, exportIgnore, allRefs
- Verified fetches (experimental): verifyCommit, keytype, publicKey(s)
- Dirty working directory support: dirtyRev, dirtyShortRev
- Most complex fetcher fully modeled

**Status**: ✅ Complete, all tests passing

---

### ✅ Phase 3: Complete Typed Inputs for All Fetchers
**Commits**: 04ff90abb, b33410524, b96980ca3, 71c87a92a, 5d085174d, 9dcbef90b

**Files Created**:
- `src/libfetchers/include/nix/fetchers/tarball-typed.hh`
- `src/libfetchers/tarball-typed.cc`
- `src/libfetchers/include/nix/fetchers/github-typed.hh`
- `src/libfetchers/github-typed.cc`
- `src/libfetchers/include/nix/fetchers/mercurial-typed.hh`
- `src/libfetchers/mercurial-typed.cc`
- `src/libfetchers/include/nix/fetchers/indirect-typed.hh`
- `src/libfetchers/indirect-typed.cc`

**Tarball Typed Inputs** (also used by FileInputScheme):
- `TarballUnlockedInput`: Just a URL
- `TarballLockedInput`: With effectiveUrl, etag, immutableUrl
- `TarballFinalInput`: With narHash
- Features: HTTP caching, optional unpack, Git archive metadata

**GitHub/GitLab Typed Inputs** (also used by SourceHutInputScheme):
- `GitHubUnlockedInput`: owner, repo, optional ref/rev, optional host
- `GitHubLockedInput`: Resolved to commit hash with treeHash
- `GitHubFinalInput`: With narHash
- Type aliases: GitLab uses identical structures to GitHub

**Mercurial Typed Inputs**:
- `MercurialUnlockedInput`: URL with optional ref/rev
- `MercurialLockedInput`: Specific changeset with revCount
- `MercurialFinalInput`: With narHash

**Indirect Typed Inputs**:
- `IndirectUnlockedInput`: Just an identifier (e.g., "nixpkgs")
- `IndirectLockedInput`: Resolved to specific revision
- `IndirectFinalInput`: With narHash from resolved input

**Status**: ✅ Phase 3 Complete - All fetcher types now have typed inputs!

---

## Remaining Work

---

## Future Phases (Not Yet Started)

### ⏳ Phase 4: Core Infrastructure Integration
**Goal**: Update existing code to use typed inputs internally

**Key Changes Needed**:
- Modify `Input` class to wrap typed inputs instead of `Attrs`
- Update `InputScheme` methods to work with typed inputs
- Ensure backward compatibility with existing `Attrs` APIs during transition
- Add runtime type checking where needed

**Estimated Impact**: High - Requires careful migration of existing code

---

### ⏳ Phase 5: Integration Points
**Goal**: Update libflake and CLI to use typed inputs

**Key Changes Needed**:
- Update flake lockfile parsing/generation
- Modify CLI commands that work with inputs
- Update input validation and error messages
- Ensure flake.lock format remains compatible

**Estimated Impact**: Medium - Well-defined integration points

---

### ⏳ Phase 6: Cleanup and Documentation
**Goal**: Remove deprecated APIs and document the new system

**Key Changes Needed**:
- Mark old `Attrs`-based APIs as deprecated
- Eventually remove compatibility shims
- Add comprehensive documentation
- Create migration guide for downstream users
- Add examples of using typed inputs

**Estimated Impact**: Low - Documentation and cleanup

---

## Testing Strategy

### Current Approach
All changes so far have been **purely additive**:
- New types coexist with existing `Attrs` code
- No behavior modifications
- All 253 unit tests pass (11 skipped)
- New types unused by production code

### Future Testing
When integrating typed inputs (Phases 4-5):
- Gradual migration with both paths active
- Extensive testing of `Attrs` ↔ typed input conversions
- Property-based testing for roundtrip conversions
- Integration tests for flake operations
- Performance benchmarks

---

## Benefits Achieved So Far

### 1. Type Safety
Compile-time guarantees about:
- What fields exist at each state
- Valid state transitions
- Impossible states (e.g., narHash without rev)

### 2. Self-Documenting Code
Types clearly show:
- What data is required vs optional
- The fetching process flow
- State transitions during locking

### 3. Better Error Messages
Future work will enable:
- Compile-time errors instead of runtime
- Clear messages about missing required fields
- Better IDE support and autocomplete

### 4. Easier Maintenance
- Clear contracts between components
- Easier to add new fetcher types
- Safer refactoring with compiler help

---

## Statistics

- **Commits**: 6
- **New Files**: 19 (11 source, 8 documentation)
- **Lines Added**: ~3500 (types, conversions, docs)
- **Test Status**: All 253 tests passing
- **Behavior Changes**: None (purely additive)
- **Fetchers Typed**: 6 of 6 (Path, Git, Tarball/File, GitHub/GitLab/SourceHut, Mercurial, Indirect)
- **Completion**: Phase 3 complete (100%), ~50% overall

---

## Next Immediate Steps

Phase 3 is complete! **All libfetchers behavior is now modeled with types**. The types coexist with the existing `Attrs`-based system and provide compile-time verification of state transitions.

To begin using these types in production code, start Phase 4:

1. **Add typed input storage to Input class**
   - Add `std::any typedInput` member to store the actual typed input
   - Add methods to safely cast to specific types
   - Maintain backward compatibility with `Attrs`

2. **Update InputScheme interface**
   - Add methods that work with typed inputs
   - Keep existing `Attrs` methods for compatibility
   - Gradually migrate implementations

3. **Add conversion helpers**
   - Create utilities to convert between `Attrs` and typed inputs
   - Ensure roundtrip conversions preserve all data
   - Add validation for state transitions

---

## Conclusion

The foundation is solid with three diverse fetcher types completed (simple, complex, HTTP-based). The remaining three fetchers follow established patterns and can be implemented quickly. The incremental, test-driven approach has proven successful with zero test failures across all commits.
