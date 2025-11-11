# libfetchers Phase 4 Progress - Inner Boundary Migration

**Date**: 2025-11-11
**Status**: Foundation Complete

## Summary

Phase 4 establishes the infrastructure for migrating libfetchers to use typed inputs internally, relegating the dynamically typed `Input` (with `Attrs`) to the outer API boundary completely.

**Architectural Goal**: Typed inputs everywhere internally, with `Input`/`Attrs` only at the interface for legacy compatibility.

## Completed Work

### 1. Typed Input Storage in Input Class
**File**: `src/libfetchers/include/nix/fetchers/fetchers.hh`

Added `std::optional<std::any> typedInput` member to the `Input` struct. This allows Input objects to optionally carry their typed representation alongside the existing attrs, enabling gradual migration.

### 2. Polymorphic Variant Infrastructure
**File**: `src/libfetchers/include/nix/fetchers/typed-input-variant.hh`

Created comprehensive variant types for storing any typed input:
- `AnyUnlockedInput` - Variant of all unlocked input types
- `AnyLockedInput` - Variant of all locked input types
- `AnyFinalInput` - Variant of all final input types
- `AnyTypedInput` - Variant that can hold any typed input at any state

Helper functions provided:
- `getTypedInputType()` - Extract the type string from a variant
- `isTypedInputLocked()` - Check if a variant holds a locked input
- `isTypedInputFinal()` - Check if a variant holds a final input

### 3. Complete Conversion Function Suite
**Files**: All typed input implementation files (*.cc)

Implemented full bidirectional conversion for **all** typed input states:

#### Git Inputs (git-typed.cc/hh)
- `Attrs gitInputToAttrs(const GitUnlockedInput &)` ✅
- `Attrs gitInputToAttrs(const GitLockedInput &)` ✅
- `Attrs gitInputToAttrs(const GitFinalInput &)` ✅

#### Tarball Inputs (tarball-typed.cc/hh)
- `Attrs tarballInputToAttrs(const TarballUnlockedInput &)` ✅
- `Attrs tarballInputToAttrs(const TarballLockedInput &)` ✅
- `Attrs tarballInputToAttrs(const TarballFinalInput &)` ✅

#### GitHub/GitLab Inputs (github-typed.cc/hh)
- `Attrs githubInputToAttrs(const GitHubUnlockedInput &)` ✅
- `Attrs githubInputToAttrs(const GitHubLockedInput &)` ✅
- `Attrs githubInputToAttrs(const GitHubFinalInput &)` ✅
- `Attrs gitlabInputToAttrs(const GitLabUnlockedInput &)` ✅
- `Attrs gitlabInputToAttrs(const GitLabLockedInput &)` ✅
- `Attrs gitlabInputToAttrs(const GitLabFinalInput &)` ✅

#### Mercurial Inputs (mercurial-typed.cc/hh)
- `Attrs mercurialInputToAttrs(const MercurialUnlockedInput &)` ✅
- `Attrs mercurialInputToAttrs(const MercurialLockedInput &)` ✅
- `Attrs mercurialInputToAttrs(const MercurialFinalInput &)` ✅

#### Indirect Inputs (indirect-typed.cc/hh)
- `Attrs indirectInputToAttrs(const IndirectUnlockedInput &)` ✅
- `Attrs indirectInputToAttrs(const IndirectLockedInput &)` ✅
- `Attrs indirectInputToAttrs(const IndirectFinalInput &)` ✅

#### Path Inputs (already complete)
- All three states already had conversion functions ✅

### 4. Boundary Conversion Utilities
**File**: `src/libfetchers/typed-input-accessor.cc`

Implemented high-level conversion and accessor functions:

- `hasTypedInput(const Input &)` - Check if Input has typed representation
- `getTypedInput(const Input &)` - Extract typed input from Input
- `setTypedInput(Input &, const AnyTypedInput &)` - Store typed input in Input
- `attrsToTypedInput(const Settings &, const Attrs &)` - Convert Attrs → typed input
- `typedInputToAttrs(const AnyTypedInput &)` - Convert typed input → Attrs
- `tryPopulateTypedInput(Input &)` - Populate typedInput field from attrs

These functions provide the foundation for boundary conversion:
```cpp
// At API boundary - conversion happens once
Input apiInput = Input::fromAttrs(settings, attrs);
auto typed = attrsToTypedInput(*apiInput.settings, apiInput.attrs);
// ... all internal operations use typed ...
auto resultAttrs = typedInputToAttrs(typedResult);
```

## Key Design Decisions

### 1. Complete State Coverage
All typed inputs now support conversion from all three states (Unlocked, Locked, Final). This ensures we can convert at any point in the input lifecycle.

### 2. Locked Inputs Don't Preserve `ref`
Once an input is locked, the `ref` field (branch/tag name) is no longer needed or stored - we have the exact `rev` (commit hash). Conversion functions correctly handle this:
- Unlocked: has optional `ref` and optional `rev`
- Locked: has only required `rev` (no `ref`)
- Final: same as Locked plus `narHash`

### 3. Type Erasure via std::any
Using `std::optional<std::any>` for typed input storage allows the Input class to remain unchanged in size and maintain backward compatibility while storing arbitrary typed inputs.

### 4. Visitor Pattern for Conversion
The `typedInputToAttrs()` function uses `std::visit` with a lambda to dispatch to the appropriate conversion function based on the variant's active type.

## Build Status

✅ **All code compiles successfully**
- No errors
- Only minor warnings about virtual destructors (expected with inheritance)
- All existing tests still pass (11 tests skipped as before)

## Remaining Work for Phase 4

### Next: Refactor InputScheme Interface

Add typed virtual methods as the primary interface:

```cpp
// Example for GitInputScheme
class GitInputScheme : public InputScheme {
    // New: Primary typed interface
    virtual std::pair<ref<SourceAccessor>, GitLockedInput>
        lock(ref<Store>, const GitUnlockedInput &) const;

    virtual GitFinalInput
        finalize(ref<Store>, const GitLockedInput &, const Hash & narHash) const;

    // Old: Compatibility wrapper (calls typed methods internally)
    std::pair<ref<SourceAccessor>, Input>
        getAccessor(ref<Store>, const Input &) const final override;
};
```

### After That: Refactor Fetcher Implementations

Update the internal logic of each fetcher (git.cc, github.cc, etc.) to:
1. Work exclusively with typed inputs internally
2. Remove all `insert_or_assign` on attrs from internal logic
3. Make state transitions explicit function returns, not mutations
4. Keep `Attrs` manipulation only in the wrapper methods at the boundary

## Impact

### Lines of Code Added
- New files: 2 (typed-input-variant.hh, typed-input-accessor.cc)
- Modified files: 13 (all typed input headers and implementations)
- Total new functions: ~36 conversion functions
- Lines added: ~600 (mostly straightforward conversion logic)

### Architecture
- **Clear boundary** between typed (internal) and dynamic (external) code
- **Complete coverage** of all input types and states
- **Foundation ready** for Phase 4b (InputScheme refactoring)

### Testing
- Zero test failures
- Zero behavior changes (purely additive)
- Conversion functions match existing Attrs semantics exactly

## Next Session

To continue Phase 4, the next steps are:
1. Design the typed InputScheme interface (draft virtual methods)
2. Start with PathInputScheme (simplest) to prove the pattern
3. Update Path fetcher internals to use typed inputs exclusively
4. Verify tests still pass with Path using typed inputs
5. Repeat for remaining fetchers

---

**Phase 4 Foundation: Complete ✅**
**Ready for**: InputScheme refactoring and fetcher internal migration
