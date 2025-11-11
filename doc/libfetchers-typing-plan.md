# libfetchers Type Safety Improvement Plan

## Executive Summary

The libfetchers library currently suffers from poor type safety due to:
1. **Dynamic typing** - All input/output data uses `Attrs` (a `std::map<string, variant>`)
2. **Heavy mutation** - Input objects are mutated during locking/fetching
3. **Unclear state transitions** - No clear distinction between unlocked, locked, and final inputs

This document proposes an incremental refactoring plan to introduce static types throughout libfetchers, relegating the dynamically typed `Input` (with `Attrs`) to the outer API boundary completely. The goal is compile-time verified types for all internal operations, with backward compatibility maintained at the edges.

## Current Architecture Problems

### Problem 1: Dynamic Typing with Attrs

**Location:** `src/libfetchers/include/nix/fetchers/attrs.hh:15-22`

```cpp
typedef std::variant<std::string, uint64_t, Explicit<bool>> Attr;
typedef std::map<std::string, Attr> Attrs;
```

**Issues:**
- No compile-time type safety for attribute names or types
- String keys mean typos aren't caught until runtime
- Need runtime type checks everywhere (`maybeGetStrAttr`, `getIntAttr`, etc.)
- Easy to forget required attributes or use wrong types
- Hard to understand what attributes a fetcher actually needs/provides

**Evidence:** Every file needs helper functions like:
- `maybeGetStrAttr(attrs, "ref")` - could typo "ref" as "rev"
- `getIntAttr(attrs, "revCount")` - could accidentally use `getStrAttr`
- No way to know at compile time if an attribute exists

### Problem 2: Input Mutation During Locking

**Location:** `src/libfetchers/include/nix/fetchers/fetchers.hh:35-47`

```cpp
struct Input {
    std::shared_ptr<InputScheme> scheme;
    Attrs attrs;  // Mutable!
    // ...
};
```

**Issues:**
- Input conceptually represents an immutable specification, but `attrs` is freely mutated
- Methods like `getAccessor()` return a **different** Input with additional attributes
- Pattern: take unlocked Input → fetch → return locked Input with new fields
- Makes it hard to reason about whether an Input is locked or not
- Violates principle of least surprise (inputs "change" during operations)

**Evidence of mutation hotspots:**

1. **git.cc:717** - Adding rev after resolution:
```cpp
input.attrs.insert_or_assign("rev", repo->resolveRef(ref).gitRev());
```

2. **git.cc:736-739** - Adding metadata after fetch:
```cpp
input.attrs.insert_or_assign("lastModified", getLastModified(...));
if (!getShallowAttr(input))
    input.attrs.insert_or_assign("revCount", getRevCount(...));
```

3. **fetchers.cc:214-216** - Adding final markers:
```cpp
result.attrs.insert_or_assign("narHash", narHash.to_string(...));
result.attrs.insert_or_assign("__final", Explicit<bool>(true));
```

### Problem 3: Unclear State Transitions

**Issue:** There are actually three states an Input can be in:
1. **Unlocked** - User-provided specification (may have partial info like `ref` but no `rev`)
2. **Locked** - After fetching/resolving (has `rev`, `lastModified`, etc.)
3. **Final** - After fetching to store (additionally has `narHash` and `__final` flag)

But these states are:
- Not reflected in the type system
- Only distinguished by runtime attribute presence checks
- Mixed together in the same `Input` type
- Validated dynamically by `checkLocks()` at:fetchers.cc:232

**Evidence:**
```cpp
bool Input::isLocked() const { return scheme && scheme->isLocked(*this); }
bool Input::isFinal() const { return maybeGetBoolAttr(attrs, "__final").value_or(false); }
```

These should be type-level distinctions, not runtime checks.

### Problem 4: Per-Scheme Attribute Variation

Different input schemes have completely different attribute sets:

**Git inputs** (git.cc):
- Base: `type`, `url`, `ref?`, `rev?`, `shallow`, `submodules`, `lfs`, `allRefs`, `exportIgnore`
- After locking: `+rev`, `+lastModified`, `+revCount?`
- After final: `+narHash`, `+__final`

**GitHub inputs** (github.cc):
- Base: `type`, `owner`, `repo`, `ref?`, `rev?`
- After locking: `+rev`, `+lastModified`, `+treeHash`
- After final: `+narHash`, `+__final`

**Tarball inputs** (tarball.cc):
- Base: `type`, `url`, `unpack?`
- After locking: `+treeHash`, `+lastModified?`
- After final: `+narHash`, `+__final`

**Path inputs** (path.cc):
- Base: `type`, `path`
- After locking: `+lastModified`
- After final: `+narHash`, `+__final`

**Problem:** Currently all stored as untyped `Attrs`, making it impossible to know what's available.

## Architecture Vision

### Inner vs Outer Boundaries

**Inner Boundary (libfetchers internals)**:
- All operations use typed inputs (`GitLockedInput`, `PathFinalInput`, etc.)
- Compile-time verification of state transitions
- Type-safe function signatures
- No `Attrs` manipulation

**Outer Boundary (libfetchers API)**:
- `Input` class with `Attrs` exists only for backward compatibility
- Immediately converts to typed inputs on entry
- Converts back to `Attrs` only when returning to caller
- Eventually this boundary moves outward to libflake/libcmd

**Goal**: Typed inputs everywhere internally, with `Input`/`Attrs` only at the interface for legacy compatibility.

## Proposed Type Hierarchy

### Core Type Structure

```cpp
// Base class for all inputs (common fields only)
struct InputBase {
    const Settings * settings;
    std::string type;  // "git", "github", "tarball", etc.

    // Common convenience methods
    std::string getName() const;
};

// Common fields added during locking
struct LockingMetadata {
    time_t lastModified;
};

// Common fields added when finalizing
struct FinalizationData {
    Hash narHash;
    // __final = true is implicit by being in a FinalInput
};

// Template for three-state inputs per scheme
template<typename Unlocked, typename Locked, typename Final>
struct InputStates {
    using UnlockedType = Unlocked;
    using LockedType = Locked;
    using FinalType = Final;
};
```

### Example: Git Input Types

```cpp
// Git-specific base attributes
struct GitInputBase : InputBase {
    std::string url;
    std::optional<std::string> ref;  // branch/tag name
    bool shallow = false;
    bool submodules = false;
    bool lfs = false;
    bool allRefs = false;
    bool exportIgnore = false;
    std::vector<PublicKey> publicKeys;
};

// Unlocked: user provides ref but maybe not rev
struct GitUnlockedInput : GitInputBase {
    std::optional<Hash> rev;  // Optional when unlocked

    // Can't call these methods on unlocked inputs
    // (enforced by not providing them)
};

// Locked: has resolved rev and metadata
struct GitLockedInput : GitInputBase {
    Hash rev;                      // Required after locking
    LockingMetadata locking;
    std::optional<uint64_t> revCount;  // Present unless shallow

    // Now can get fingerprint, etc.
    std::string getFingerprint() const;
};

// Final: additionally has narHash, ready for store
struct GitFinalInput : GitLockedInput {
    FinalizationData finalization;

    // Can compute store paths, etc.
    StorePath computeStorePath(Store & store) const;
};

using GitInputStates = InputStates<GitUnlockedInput, GitLockedInput, GitFinalInput>;
```

### Example: GitHub Input Types

```cpp
struct GitHubInputBase : InputBase {
    std::string owner;
    std::string repo;
    std::optional<std::string> ref;
};

struct GitHubUnlockedInput : GitHubInputBase {
    std::optional<Hash> rev;
};

struct GitHubLockedInput : GitHubInputBase {
    Hash rev;
    LockingMetadata locking;
    Hash treeHash;

    std::string getFingerprint() const;
};

struct GitHubFinalInput : GitHubLockedInput {
    FinalizationData finalization;
};

using GitHubInputStates = InputStates<GitHubUnlockedInput, GitHubLockedInput, GitHubFinalInput>;
```

### Sum Type for Polymorphic Operations

For code that needs to handle any input type polymorphically:

```cpp
// Variant types for each state
using UnlockedInput = std::variant<
    GitUnlockedInput,
    GitHubUnlockedInput,
    TarballUnlockedInput,
    PathUnlockedInput,
    IndirectUnlockedInput
>;

using LockedInput = std::variant<
    GitLockedInput,
    GitHubLockedInput,
    TarballLockedInput,
    PathLockedInput
    // Note: IndirectInput never locked
>;

using FinalInput = std::variant<
    GitFinalInput,
    GitHubFinalInput,
    TarballFinalInput,
    PathFinalInput
>;

// For backward compatibility during migration
using AnyInput = std::variant<UnlockedInput, LockedInput, FinalInput>;
```

## Updated InputScheme Interface

The current interface returns mutated inputs:

```cpp
// OLD - returns modified Input
virtual std::pair<ref<SourceAccessor>, Input>
    getAccessor(ref<Store> store, const Input & input) const = 0;
```

Proposed typed interface:

```cpp
// NEW - explicit state transitions via return type
template<typename SchemeTraits>
struct TypedInputScheme {
    using Unlocked = typename SchemeTraits::UnlockedType;
    using Locked = typename SchemeTraits::LockedType;
    using Final = typename SchemeTraits::FinalType;

    // Parse from URL/attrs
    virtual Unlocked inputFromURL(const Settings & settings, const ParsedURL & url) const = 0;
    virtual Unlocked inputFromAttrs(const Settings & settings, const Attrs & attrs) const = 0;

    // Lock: unlocked -> locked (resolve refs, fetch metadata)
    virtual std::pair<ref<SourceAccessor>, Locked>
        lock(ref<Store> store, const Unlocked & input) const = 0;

    // Finalize: locked -> final (compute narHash)
    virtual Final finalize(ref<Store> store, const Locked & input, const Hash & narHash) const = 0;

    // Can also lock directly from unlocked -> final
    virtual std::pair<ref<SourceAccessor>, Final>
        lockAndFinalize(ref<Store> store, const Unlocked & input) const = 0;
};
```

Example for Git:

```cpp
struct GitInputScheme : TypedInputScheme<GitInputStates> {
    GitUnlockedInput inputFromURL(...) const override;
    GitUnlockedInput inputFromAttrs(...) const override;

    std::pair<ref<SourceAccessor>, GitLockedInput>
        lock(ref<Store> store, const GitUnlockedInput & input) const override;

    GitFinalInput finalize(ref<Store>, const GitLockedInput &, const Hash &) const override;
};
```

## Migration Strategy: Incremental Refactoring

The key insight: **we cannot do this all at once**. The code is intricate and tightly coupled. We need an incremental approach where tests keep passing at each step.

### Phase 0: Preparation (Non-breaking)

**Goal:** Add new types alongside old ones, no behavior changes yet.

**Steps:**

1. **Define base type hierarchy** in new files:
   - `src/libfetchers/include/nix/fetchers/typed-inputs.hh`
   - Define `InputBase`, `LockingMetadata`, `FinalizationData`
   - Define traits and concepts for typed inputs

2. **Add conversion utilities** between `Attrs` and typed inputs:
   - `template<typename T> T inputFromAttrs(const Attrs &)`
   - `template<typename T> Attrs inputToAttrs(const T &)`
   - These ensure we can round-trip during migration

3. **Write conversion tests** to verify:
   - Typed inputs can be created from Attrs
   - Converting back produces identical Attrs
   - Run in existing test suite: `meson test -C build`

**Success criteria:**
- All existing tests still pass
- New code doesn't affect any existing code paths
- Conversion utilities work correctly

### Phase 1: Single Fetcher Prototype (PathInputScheme)

**Goal:** Prove the approach works with the simplest fetcher.

**Why Path?**
- Simplest fetcher (~200 lines in path.cc)
- No network operations (fast tests)
- Minimal attributes (type, path, lastModified, narHash)
- No complex state transitions

**Steps:**

1. **Define path input types** in `src/libfetchers/typed/path.hh`:
   ```cpp
   struct PathUnlockedInput : InputBase {
       std::filesystem::path path;
   };

   struct PathLockedInput : PathUnlockedInput {
       LockingMetadata locking;
   };

   struct PathFinalInput : PathLockedInput {
       FinalizationData finalization;
   };
   ```

2. **Create PathInputScheme adapter** in `src/libfetchers/typed/path.cc`:
   - Wraps existing PathInputScheme
   - Provides typed interface internally
   - Converts to/from Attrs at boundaries
   - **Does not change existing public API**

3. **Add path-specific tests**:
   - Create `src/libfetchers/typed/tests/path-tests.cc`
   - Test unlocked → locked transition
   - Test locked → final transition
   - Test that typed and untyped paths produce same results

4. **Update functional tests** to verify no regressions:
   ```bash
   meson test -C build fetchTree-file
   ```

**Success criteria:**
- Path fetcher works with typed interface internally
- All path-related tests pass
- No changes to tests in `tests/functional/`
- Proves the pattern works

### Phase 2: Git Fetcher (Complex Case)

**Goal:** Validate approach scales to complex fetcher with submodules.

**Why Git?**
- Most complex fetcher (~800 lines in git.cc)
- Multiple state transitions (resolve ref → fetch commit → handle submodules)
- Heavy attribute mutation currently
- Most test coverage

**Steps:**

1. **Define git input types** in `src/libfetchers/typed/git.hh`:
   ```cpp
   struct GitUnlockedInput : InputBase { /* ... */ };
   struct GitLockedInput : GitUnlockedInput { /* ... */ };
   struct GitFinalInput : GitLockedInput { /* ... */ };
   ```

2. **Refactor getAccessorFromCommit()** in git.cc:
   - Currently mutates input heavily (lines 717, 736-739)
   - Convert to: take `GitUnlockedInput`, return `GitLockedInput`
   - Keep Attrs-based interface for compatibility

3. **Handle submodules** carefully:
   - Currently creates new Input objects dynamically (lines 764-782)
   - These need to be `GitUnlockedInput` objects
   - Test submodule handling thoroughly

4. **Update git tests**:
   ```bash
   meson test -C build fetchGit
   meson test -C build fetchGitRefs
   meson test -C build fetchGitShallow
   meson test -C build fetchGitSubmodules
   meson test -C build fetchGitVerification
   ```

**Success criteria:**
- Git fetcher uses typed inputs internally
- All git functional tests pass
- Submodules work correctly
- Performance is equivalent

### Phase 3: Remaining Fetchers

**Goal:** Convert remaining fetchers one by one.

**Order (by complexity):**

1. **Tarball** (`tarball.cc`, ~400 lines)
   - Moderate complexity
   - Network operations
   - Tests: `meson test -C build fetchurl`

2. **GitHub/GitLab** (`github.cc`, ~300 lines)
   - Builds on git concepts
   - API integration
   - Tests: `meson test -C build` (various flake tests use github)

3. **Mercurial** (`mercurial.cc`, ~400 lines)
   - Similar to git but simpler
   - Tests: `meson test -C build fetchMercurial`

4. **Indirect** (`indirect.cc`, ~150 lines)
   - Registry lookups
   - Never locked (always resolves to direct input)
   - Tests: flake registry tests

**Per-fetcher steps:**
1. Define typed input structs
2. Refactor fetcher to use types internally
3. Keep Attrs interface at boundaries
4. Run fetcher-specific tests
5. Run full test suite

**Success criteria per fetcher:**
- Fetcher converted to typed interface
- All fetcher-specific tests pass
- Full test suite still passes

### Phase 4: Core Infrastructure - Inner Boundary Migration

**Goal:** Update core libfetchers infrastructure to use typed inputs internally, with `Input`/`Attrs` only at the API boundary.

**Architectural Principle:**
- **Internal operations**: Work exclusively with typed inputs
- **External API**: `Input` class serves as adapter, converting at the boundary
- **No `Attrs` manipulation inside libfetchers** except at conversion points

**Components to update:**

1. **InputScheme base class** (`fetchers.hh:195-268`):
   - Add typed virtual methods as the primary interface
   - Methods accept and return typed inputs directly
   - Old `Attrs`-based methods become thin wrappers that convert
   - Example:
     ```cpp
     // New: Primary interface (typed)
     virtual std::pair<ref<SourceAccessor>, GitLockedInput>
         lock(ref<Store>, const GitUnlockedInput &) const = 0;

     // Old: Compatibility wrapper (converts at boundary)
     std::pair<ref<SourceAccessor>, Input>
         getAccessor(ref<Store>, const Input &) const final;
     ```

2. **Fetcher implementations** (git.cc, github.cc, etc.):
   - Refactor internal logic to work with typed inputs
   - Remove all `insert_or_assign` on attrs
   - State transitions become function returns, not mutations
   - Keep conversion helpers for the Input-based wrappers

3. **Input class** (`fetchers.hh:35-184`):
   - Remains as thin adapter at the boundary
   - Stores both `Attrs` (for serialization) and `typedInput` (for operations)
   - Public API unchanged for backward compatibility
   - Internally delegates to typed input operations

4. **checkLocks()** (`fetchers.cc:232-298`):
   - Primary implementation works on typed inputs
   - Uses `std::visit` for type-safe validation
   - Old `Attrs`-based version converts and delegates

5. **Cache** (`cache.hh`):
   - Continues using `Attrs` for serialization (disk format)
   - Converts to typed inputs for validation
   - Internal operations use typed inputs

**Steps:**

1. Add typed virtual methods to InputScheme base class

2. Implement typed methods in each fetcher (git.cc, path.cc, etc.)
   - Work directly with typed inputs
   - No `Attrs` manipulation internally

3. Update Input class to delegate to typed methods
   - Convert on entry: `Attrs` → typed input
   - Perform operation with typed inputs
   - Convert on exit: typed input → `Attrs` (if needed)

4. Gradually remove `Attrs` manipulation from fetcher internals

**Success criteria:**
- Fetcher internals use only typed inputs
- `Attrs` manipulation only at Input class boundary
- All tests pass
- No change to public API behavior

### Phase 5: Integration Points

**Goal:** Update libflake and other consumers of libfetchers.

**Components:**

1. **libflake** (`src/libflake/`):
   - `FlakeRef` wraps `Input`
   - `LockedNode` stores locked inputs
   - Update to use typed inputs

2. **nix CLI commands** (`src/nix/`):
   - Commands like `nix flake update`, `nix flake lock`
   - Should benefit from better type safety

3. **Lockfile serialization** (`src/libflake/lockfile.cc`):
   - Currently serializes/deserializes Attrs
   - Can continue using Attrs format
   - Convert to typed inputs after parsing

**Steps:**

1. Update `FlakeRef` to hold typed inputs
2. Update lockfile parsing to create typed inputs
3. Update CLI commands to work with typed inputs
4. Run flake tests: `meson test -C build` (flake tests)

**Success criteria:**
- libflake uses typed inputs
- Lockfile format unchanged (backward compatible)
- All flake tests pass
- CLI commands work correctly

### Phase 6: Cleanup and Documentation

**Goal:** Remove old code, document new patterns.

**Tasks:**

1. **Remove deprecated code**:
   - Old Attrs-based methods in `InputScheme`
   - Conversion shims that are no longer needed
   - `attrs` field from `Input` (replaced by typed variant)

2. **Update documentation**:
   - Document type hierarchy in comments
   - Add examples to `src/libfetchers/README.md`
   - Update CONTRIBUTING.md with typed input guidelines

3. **Add static assertions**:
   ```cpp
   static_assert(std::is_trivially_copyable_v<GitUnlockedInput>);
   static_assert(requires(GitLockedInput i) { i.rev; i.locking; });
   ```

4. **Performance validation**:
   - Benchmark input parsing and locking
   - Should be equivalent or better than before
   - Profile to ensure no regressions

**Success criteria:**
- No old Attrs-based code remains
- Documentation is complete and clear
- All tests pass
- Performance is equivalent or better

## Testing Strategy

### Existing Tests to Preserve

**Unit tests** (currently none, but should add):
- Add `src/libfetchers-tests/` directory
- Test each input type's construction and validation
- Test state transitions
- Test error cases

**Functional tests** (existing, must keep passing):
- `tests/functional/fetchGit*.sh` - Git fetching variations
- `tests/functional/fetchMercurial.sh` - Mercurial support
- `tests/functional/fetchurl.sh` - Tarball fetching
- `tests/functional/fetchTree-file.sh` - Path inputs
- `tests/functional/flakes/` - Flake integration tests
- Run with: `meson test -C build --print-errorlogs <test-name>`

### New Tests to Add

**Typed input tests** (`src/libfetchers/typed/tests/`):
1. **Conversion tests**:
   - Attrs → typed input → Attrs round-trips correctly
   - Test for each input type

2. **State transition tests**:
   - Unlocked → locked transition preserves user attributes
   - Locked → final transition adds narHash correctly
   - Invalid transitions are prevented at compile time

3. **Type safety tests**:
   - Demonstrate compile-time prevention of:
     - Accessing `rev` on unlocked inputs
     - Computing store paths on non-final inputs
     - Forgetting required attributes

4. **Compatibility tests**:
   - Old Attrs-based code produces same results as typed code
   - Serialization format unchanged

**Example test structure**:
```cpp
TEST(GitInput, UnlockedToLocked) {
    GitUnlockedInput unlocked{
        .url = "https://github.com/NixOS/nix",
        .ref = "master"
    };

    auto [accessor, locked] = scheme.lock(store, unlocked);

    // Check required fields present
    EXPECT_TRUE(locked.rev);
    EXPECT_GT(locked.locking.lastModified, 0);

    // Check user attributes preserved
    EXPECT_EQ(locked.url, unlocked.url);
    EXPECT_EQ(locked.ref, unlocked.ref);
}
```

## Backward Compatibility Considerations

### Lockfile Format

**Must not change:** Lockfiles are stored in repositories and must remain parseable.

**Strategy:**
- Keep lockfile format as JSON with flat attributes
- Convert to typed inputs after parsing
- Convert back to Attrs before serializing
- Add tests to ensure format unchanged

### C API

**Location:** `src/libfetchers-c/`

**Concern:** C bindings can't use C++ templates/variants

**Strategy:**
- Keep C API using Attrs
- Internally convert to typed inputs
- No breaking changes to C interface

### NIX_PATH and Legacy Paths

**Concern:** Old-style `NIX_PATH` uses string-based paths

**Strategy:**
- These already go through `Input::fromAttrs()`
- Conversion layer handles them transparently
- No user-visible changes

## Risk Mitigation

### Risk 1: Tests Fail During Migration

**Mitigation:**
- Work in feature branch
- Run full test suite after each phase
- If tests fail, fix before proceeding to next phase
- Keep phases small and manageable

### Risk 2: Performance Regression

**Mitigation:**
- Benchmark before and after each phase
- Typed inputs should be faster (no runtime type checks)
- If regression found, profile and optimize
- Consider using `std::visit` optimizations

### Risk 3: Breaking External Code

**Mitigation:**
- Maintain Attrs-based API during migration
- Deprecate but don't remove until Phase 6
- Provide migration guide for downstream users
- Announce changes on discourse and GitHub

### Risk 4: Complexity Explosion

**Mitigation:**
- Keep types simple (plain structs, no complex inheritance)
- Use templates judiciously
- Document patterns clearly
- Add examples for common cases
- Refactor if types become unwieldy

## Benefits of Typed Approach

### Compile-Time Safety

**Before (with Attrs everywhere):**
```cpp
// Can typo attribute names
auto ref = maybeGetStrAttr(attrs, "rfe");  // Typo! Should be "ref"

// Can use wrong type
auto revCount = getStrAttr(attrs, "revCount");  // Should be getIntAttr!

// No way to know if attribute exists
auto rev = maybeGetStrAttr(attrs, "rev");  // Is rev always present? Sometimes? Never?
```

**After (typed inputs internally, Attrs at boundary only):**
```cpp
// Inside libfetchers - compile-time safety
auto ref = input.ref;  // Won't compile if typo'd
auto revCount = input.revCount;  // Type is std::optional<uint64_t>
auto rev = locked.rev;  // Type is Hash (required on locked inputs)

// At API boundary - conversion happens once
Input apiInput = Input::fromAttrs(settings, attrs);  // Validates and converts
auto typed = getTypedInput(apiInput);  // Extract typed input
// ... all operations use typed ...
auto result = typedInputToAttrs(typedResult);  // Convert back only at exit
```

### Clear State Transitions

**Before:**
```cpp
// Hard to tell what state input is in
std::pair<ref<SourceAccessor>, Input>
GitInputScheme::getAccessor(ref<Store> store, const Input & input) const
{
    // input is modified and returned - what changed? unclear!
    // ...
    return {accessor, input};
}
```

**After:**
```cpp
// State transition is explicit in type signature
std::pair<ref<SourceAccessor>, GitLockedInput>
GitInputScheme::lock(ref<Store> store, const GitUnlockedInput & input) const
{
    // Clearly transforms unlocked → locked
    // Can't accidentally return wrong state
}
```

### Better Documentation

The types serve as documentation:

```cpp
struct GitLockedInput {
    // Base attributes
    std::string url;
    std::optional<std::string> ref;  // branch/tag name
    bool shallow = false;

    // Locking fields (present after resolution)
    Hash rev;                        // Commit hash
    LockingMetadata locking;         // timestamp
    std::optional<uint64_t> revCount;  // commits since root (if not shallow)

    std::string getFingerprint() const;  // Only available after locking
};
```

Now developers can:
- See exactly what fields are available
- Understand when fields are populated
- Know which operations are valid

### Easier Testing

```cpp
// Before: need to construct Attrs carefully
Attrs attrs;
attrs["type"] = "git";
attrs["url"] = "https://github.com/NixOS/nix";
attrs["ref"] = "master";
// Did we forget any required fields? Who knows!

// After: compiler ensures required fields present
GitUnlockedInput input{
    .url = "https://github.com/NixOS/nix",
    .ref = "master"
    // Forgot a required field? Won't compile!
};
```

### Fewer Runtime Errors

Many errors move from runtime to compile time:
- Missing attributes
- Wrong attribute types
- Invalid state transitions
- Accessing fields that don't exist yet

## Success Metrics

After completion, we should have:

1. **Type Safety**
   - **Zero uses of `insert_or_assign` on input attrs inside libfetchers**
   - All input state tracked in type system
   - Compile-time prevention of state errors
   - `Attrs` manipulation only at API boundary for conversion

2. **Clear Boundaries**
   - **Inner boundary**: All libfetchers internals use typed inputs exclusively
   - **Outer boundary**: `Input` class with `Attrs` only at API surface
   - Conversion happens exactly once on entry/exit
   - No mixed Attrs/typed code paths internally

3. **Clarity**
   - Clear unlocked/locked/final distinction in types
   - Explicit state transitions in function signatures
   - Self-documenting code via types
   - Function signatures reveal exactly what happens

4. **Testing**
   - All existing tests still pass
   - New typed input tests added
   - No regressions in functionality or performance
   - Tests can use typed inputs directly

5. **Maintainability**
   - Easier to add new fetchers (clear typed template to follow)
   - Easier to understand input lifecycle (follow the types)
   - Fewer runtime surprises (compiler catches errors)
   - Clear separation of concerns (types vs serialization)

## Open Questions

### 1. Should we use inheritance or composition?

**Option A: Inheritance**
```cpp
struct GitLockedInput : GitUnlockedInput {
    Hash rev;  // Now required
    LockingMetadata locking;
};
```

Pros: Natural "is-a" relationship, code reuse
Cons: Can't prevent access to fields that shouldn't change

**Option B: Composition**
```cpp
struct GitLockedInput {
    GitUnlockedInput unlocked;  // Embedded
    Hash rev;
    LockingMetadata locking;
};
```

Pros: More explicit, can control field access
Cons: Need to access via `locked.unlocked.url` (more verbose)

**Recommendation:** Start with inheritance (simpler), consider composition if issues arise.

### 2. How to handle optional attributes?

Some attributes are optional even after locking (e.g., `revCount` only present for non-shallow git clones).

**Options:**
- `std::optional<T>` - explicit optionality
- Separate types for shallow/non-shallow - too many types
- Runtime check - defeats purpose of types

**Recommendation:** Use `std::optional<T>` for truly optional fields.

### 3. What about input schemes we don't know about yet?

The registry system allows dynamic registration of input schemes. What if someone registers a custom scheme?

**Options:**
- Require all schemes to use typed inputs - breaks extensibility
- Allow both typed and Attrs-based schemes - complexity
- Provide typed wrapper for Attrs-based schemes - good balance

**Recommendation:** Provide a `GenericInput` type that wraps Attrs for unknown schemes.

### 4. How to serialize typed inputs?

Need to convert to JSON for lockfiles, debug output, etc.

**Options:**
- Provide `toAttrs()` method on each type - simple, reuses existing code
- Use reflection/template metaprogramming - complex but automatic
- Manual serialization functions - tedious but explicit

**Recommendation:** Start with `toAttrs()` methods, consider reflection if too much boilerplate.

## References

### Relevant Files

**Core libfetchers:**
- `src/libfetchers/include/nix/fetchers/attrs.hh` - Dynamic typing
- `src/libfetchers/include/nix/fetchers/fetchers.hh` - Input class and InputScheme
- `src/libfetchers/fetchers.cc` - Input creation and validation

**Fetcher implementations:**
- `src/libfetchers/git.cc` - Git fetcher (most complex)
- `src/libfetchers/github.cc` - GitHub/GitLab fetcher
- `src/libfetchers/tarball.cc` - Tarball fetcher
- `src/libfetchers/path.cc` - Local path fetcher (simplest)
- `src/libfetchers/indirect.cc` - Registry-based indirect inputs
- `src/libfetchers/mercurial.cc` - Mercurial fetcher

**Supporting infrastructure:**
- `src/libfetchers/cache.hh` - Fetch result caching
- `src/libfetchers/registry.hh` - Flake registry

**Integration points:**
- `src/libflake/flakeref.hh` - FlakeRef wraps Input
- `src/libflake/lockfile.hh` - Lockfile stores locked inputs

**Tests:**
- `tests/functional/fetchGit*.sh` - Git fetching tests
- `tests/functional/fetchMercurial.sh` - Mercurial tests
- `tests/functional/fetchurl.sh` - Tarball tests
- `tests/functional/fetchTree-file.sh` - Path tests
- `tests/functional/flakes/` - Flake integration tests

### Related Documentation

- `src/libfetchers/` directory - Implementation
- This document - Refactoring plan

## Appendix: Example Code Comparison

### Before: Dynamic Typing with Mutation

```cpp
// Creating an input (no type safety)
Attrs attrs;
attrs["type"] = "git";
attrs["url"] = "https://github.com/NixOS/nix";
attrs["ref"] = "master";  // Could typo as "rfe"
// Forgot to set shallow? Won't know until runtime

auto input = Input::fromAttrs(settings, std::move(attrs));

// Fetching (mutation pattern)
auto [accessor, lockedInput] = input.getAccessor(store);
// What changed in lockedInput? Have to read implementation to know

// Checking if locked (runtime check)
if (lockedInput.isLocked()) {
    auto rev = lockedInput.getRev();  // std::optional, may not exist!
    if (rev) {
        // Use rev
    }
}

// Fetching to store (more mutation)
auto [storePath, finalInput] = lockedInput.fetchToStore(store);
// What's the difference between lockedInput and finalInput? Unclear

// Checking attributes
if (auto revCount = lockedInput.getRevCount()) {
    // Is revCount always present after locking? Sometimes? No way to know
}
```

### After: Static Typing with Immutability

```cpp
// Creating an input (type safe)
GitUnlockedInput input{
    .url = "https://github.com/NixOS/nix",
    .ref = "master",
    .shallow = false  // Explicit, with default
    // Compiler ensures no typos, required fields present
};

// Locking (explicit state transition)
auto [accessor, locked] = gitScheme.lock(store, input);
// Type is GitLockedInput - clearly different from GitUnlockedInput

// Checking if locked (type system check)
// locked.rev is Hash (not optional) - guaranteed to exist
auto rev = locked.rev;  // No optional, no runtime check needed

// Finalizing (explicit state transition)
auto final = gitScheme.finalize(store, locked, narHash);
// Type is GitFinalInput - clearly different from GitLockedInput

// Checking attributes
if (locked.revCount) {  // std::optional<uint64_t>
    // revCount is optional because it's only present for non-shallow clones
    // This is explicit in the type
}
```

### Comparison: Adding a New Attribute

**Before (dynamic):**
1. Add `insert_or_assign` call somewhere in implementation
2. Add getter method to Input class
3. Remember to update lockfile parsing
4. Remember to update serialization
5. Update tests
6. Hope you didn't typo the attribute name anywhere
7. Hope you used consistent types everywhere

**After (static):**
1. Add field to appropriate typed input struct
2. Compiler tells you every place you need to update
3. Tests won't compile until you handle new field
4. Serialization handled by `toAttrs()` implementation
5. Can't typo - it's a struct field

## Conclusion

This refactoring will significantly improve libfetchers' maintainability and type safety. The incremental approach ensures we can:
- Keep tests passing at each step
- Validate the approach early with simple cases
- Adjust the plan based on learnings
- Maintain backward compatibility throughout

The end result will be code that is:
- Easier to understand (types document behavior)
- Safer to modify (compiler catches errors)
- More maintainable (clear patterns to follow)
- Better tested (state transitions explicit)

The investment of ~12 weeks will pay dividends in reduced debugging time, fewer runtime errors, and easier future enhancements to the fetcher system.
