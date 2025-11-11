# Phase 4: InputScheme Interface Design

## Goal

Refactor InputScheme implementations to use typed inputs internally while maintaining backward compatibility with the existing `Input`/`Attrs` API.

## Design Pattern

### Current Architecture (Attrs-based)

```cpp
struct PathInputScheme : InputScheme {
    std::pair<ref<SourceAccessor>, Input> getAccessor(ref<Store> store, const Input & input) const override {
        Input result(input);  // Copy
        // Extract from attrs
        auto path = getStrAttr(result.attrs, "path");

        // Do work...

        // Mutate attrs
        result.attrs.insert_or_assign("lastModified", mtime);

        return {accessor, std::move(result)};
    }
};
```

**Problems:**
- ❌ Uses dynamic `Attrs` throughout
- ❌ Mutates input by adding fields
- ❌ State transitions implicit (attrs change but type doesn't)
- ❌ No compile-time guarantees about what fields exist

### New Architecture (Typed Internal)

```cpp
struct PathInputScheme : InputScheme {
    // NEW: Typed internal method - primary implementation
    std::pair<ref<SourceAccessor>, PathLockedInput>
    lockTyped(ref<Store> store, const PathUnlockedInput & input) const {
        // Extract from typed input (compile-time safe)
        auto path = input.path;

        // Do work...

        // Return new locked input (immutable pattern)
        PathLockedInput locked(input.settings, path);
        locked.locking.lastModified = mtime;

        return {accessor, std::move(locked)};
    }

    // OLD: Attrs-based method - thin wrapper for backward compatibility
    std::pair<ref<SourceAccessor>, Input> getAccessor(ref<Store> store, const Input & input) const override {
        // Convert at boundary: Input → typed
        auto typed = pathInputFromAttrs(*input.settings, input.attrs);

        // Delegate to typed method
        auto [accessor, locked] = lockTyped(store, typed);

        // Convert at boundary: typed → Input
        Input result(input.settings);
        result.attrs = pathInputToAttrs(locked);

        return {accessor, std::move(result)};
    }
};
```

**Benefits:**
- ✅ Uses typed inputs internally
- ✅ Immutable pattern (returns new object, doesn't mutate)
- ✅ State transitions explicit in types
- ✅ Compile-time field checking
- ✅ Backward compatible (old API still works)
- ✅ `Attrs` manipulation only at boundary

## Implementation Strategy

### Step 1: Add Typed Method Alongside Old Method

Keep both methods during migration:
- `lockTyped()` - New typed method (internal use)
- `getAccessor()` - Old method (delegates to typed, maintains compatibility)

### Step 2: Internal Logic Uses Only Typed Inputs

```cpp
std::pair<ref<SourceAccessor>, PathLockedInput>
PathInputScheme::lockTyped(ref<Store> store, const PathUnlockedInput & input) const
{
    // Pure typed input manipulation - no Attrs!
    auto absPath = getAbsPath(input.path);

    // ... fetching logic ...

    // Construct new locked input
    PathLockedInput locked(input.settings, input.path);
    locked.locking.lastModified = mtime;

    return {accessor, std::move(locked)};
}
```

**Key principles:**
- ❌ No `getStrAttr()` - use `input.path` directly
- ❌ No `insert_or_assign()` - construct new objects
- ❌ No `Attrs` manipulation inside this method
- ✅ Pure typed input logic

### Step 3: Wrapper Method Handles Conversion

```cpp
std::pair<ref<SourceAccessor>, Input>
PathInputScheme::getAccessor(ref<Store> store, const Input & input) const override
{
    // Boundary conversion: dynamic → typed
    auto unlocked = pathInputFromAttrs(*input.settings, input.attrs);

    // Pure typed logic (no Attrs!)
    auto [accessor, locked] = lockTyped(store, unlocked);

    // Boundary conversion: typed → dynamic
    Input result(input.settings);
    result.attrs = pathInputToAttrs(locked);

    return {accessor, std::move(result)};
}
```

## Handling State Transitions

### Unlocked → Locked

```cpp
// Typed method signature clearly shows state transition
std::pair<ref<SourceAccessor>, PathLockedInput>
lockTyped(ref<Store>, const PathUnlockedInput &) const;
```

### Locked → Final

For fetchers that need a separate finalization step:

```cpp
PathFinalInput
finalizeTyped(ref<Store>, const PathLockedInput &, const Hash & narHash) const;
```

### Direct Unlocked → Final

For simple fetchers that go straight to final:

```cpp
std::pair<ref<SourceAccessor>, PathFinalInput>
lockAndFinalizeTyped(ref<Store>, const PathUnlockedInput &) const;
```

## Migration Path

### Phase 4a: Add Typed Methods (Current)
- Add `lockTyped()` methods to fetchers
- Keep old `getAccessor()` as wrapper
- Old method delegates to new typed method
- **All Attrs manipulation only in wrapper**

### Phase 4b: Migrate Callers (Future)
- Update high-level code (libflake) to use typed methods directly
- Eventually remove wrapper methods
- Move boundary conversion outward to libflake/libcmd

## Success Criteria

After refactoring PathInputScheme:

1. ✅ `lockTyped()` method uses only typed inputs internally
2. ✅ Zero `insert_or_assign` on attrs inside `lockTyped()`
3. ✅ All Attrs manipulation confined to `getAccessor()` wrapper
4. ✅ State transition explicit in method signature
5. ✅ All tests still pass
6. ✅ Backward compatible - existing code works unchanged

## Example: Path Fetcher

### Before (Current)
```cpp
std::pair<ref<SourceAccessor>, Input> getAccessor(...) const override {
    Input input(_input);  // Copy for mutation
    auto path = getStrAttr(input.attrs, "path");  // Dynamic lookup

    // ... work ...

    input.attrs.insert_or_assign("lastModified", mtime);  // Mutation!
    return {accessor, std::move(input)};
}
```

### After (Typed)
```cpp
// Primary implementation - typed and pure
std::pair<ref<SourceAccessor>, PathLockedInput>
lockTyped(ref<Store> store, const PathUnlockedInput & input) const {
    auto path = input.path;  // Compile-time safe

    // ... work ...

    // Construct new object - immutable!
    PathLockedInput locked(input.settings, path);
    locked.locking.lastModified = mtime;
    return {accessor, std::move(locked)};
}

// Wrapper for backward compatibility
std::pair<ref<SourceAccessor>, Input>
getAccessor(ref<Store> store, const Input & input) const override {
    auto unlocked = pathInputFromAttrs(*input.settings, input.attrs);
    auto [accessor, locked] = lockTyped(store, unlocked);

    Input result(input.settings);
    result.attrs = pathInputToAttrs(locked);
    return {accessor, std::move(result)};
}
```

## Next Steps

1. Implement `lockTyped()` for PathInputScheme
2. Update `getAccessor()` to delegate to `lockTyped()`
3. Test that all path-related tests pass
4. Commit PathInputScheme refactor
5. Repeat for remaining fetchers (Git, GitHub, Tarball, Mercurial, Indirect)
