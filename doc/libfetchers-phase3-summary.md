# Phase 3 Summary: Remaining Fetchers

This document summarizes the typed input patterns for the remaining fetchers.
The implementations follow the same three-state pattern established in Phases 0-2.

## Completed in this Phase

### Tarball Fetcher
**Files**: `tarball-typed.hh`, `tarball-typed.cc`

**States**:
- `TarballUnlockedInput`: Just a URL, optionally with name
- `TarballLockedInput`: Has effectiveUrl (after redirects), etag, immutableUrl
- `TarballFinalInput`: Adds narHash

**Key attributes**:
- `url`: The tarball/file URL
- `name`: Optional name override
- `unpack`: Whether to unpack (vs flat file)
- `etag`, `immutableUrl`: For HTTP caching
- `rev`, `revCount`: For git archive downloads

## Patterns for Remaining Fetchers

### GitHub/GitLab Fetchers
**Pattern**: Git-like but fetches tarballs from hosting APIs

**Would have**:
- `GitHubUnlockedInput`: owner, repo, ref (branch/tag)
- `GitHubLockedInput`: Resolved to specific commit hash
- `GitHubFinalInput`: With narHash

**Key attributes**:
- `owner`, `repo`: Repository identification
- `ref`: Branch or tag name
- `rev`: Commit hash (when locked)
- `host`: Optional (for GitHub Enterprise, self-hosted GitLab)

### Mercurial Fetcher
**Pattern**: Very similar to Git

**Would have**:
- `MercurialUnlockedInput`: URL, optional ref and rev
- `MercurialLockedInput`: Specific changeset, lastModified, revCount
- `MercurialFinalInput`: With narHash

**Key attributes** (similar to Git):
- `url`: Repository URL
- `ref`: Branch name
- `rev`: Changeset hash (when locked)
- `revCount`, `lastModified`: Metadata

### Indirect Fetcher
**Pattern**: Resolves to another input via registry

**Would have**:
- `IndirectUnlockedInput`: Just an identifier (e.g., "nixpkgs")
- `IndirectLockedInput`: Resolved to actual input
- `IndirectFinalInput`: With narHash from resolved input

**Key attributes**:
- `id`: The indirect identifier
- `ref`: Optional ref specification
- `rev`: Optional revision specification

## Implementation Strategy

For each fetcher:

1. **Create header file** (`<fetcher>-typed.hh`):
   - Define the three state types
   - Inherit from common base
   - Add fetcher-specific fields
   - Declare conversion functions

2. **Create implementation** (`<fetcher>-typed.cc`):
   - Implement `<fetcher>InputFromAttrs()` - parse Attrs to typed input
   - Implement `<fetcher>InputToAttrs()` - convert back for compatibility

3. **Update build files**:
   - Add to `src/libfetchers/meson.build` sources list
   - Add header to `src/libfetchers/include/nix/fetchers/meson.build`

4. **Test** with existing test suite (no behavior changes yet)

5. **Commit** with descriptive message

## Why This Matters

Each typed input provides:

1. **Type Safety**: Compile-time guarantees about what fields exist at each state
2. **Documentation**: Types serve as living documentation of the fetching process
3. **Invariants**: Impossible states become unrepresentable (e.g., can't have narHash without rev)
4. **Future Migration**: Foundation for migrating existing code away from dynamic Attrs

## Next Steps

After Phase 3 completes:

- **Phase 4**: Update core Input class and InputScheme to use typed inputs internally
- **Phase 5**: Update libflake lockfiles and CLI commands to use typed inputs
- **Phase 6**: Remove deprecated Attrs-based APIs, add comprehensive documentation

## Current Progress

- [x] Phase 0: Base type infrastructure
- [x] Phase 1: Path typed inputs (simple prototype)
- [x] Phase 2: Git typed inputs (complex case with submodules, verified fetches)
- [~] Phase 3: Tarball typed inputs (HTTP-based fetching)
- [ ] Phase 3: GitHub/GitLab typed inputs
- [ ] Phase 3: Mercurial typed inputs
- [ ] Phase 3: Indirect typed inputs
- [ ] Phase 4-6: Core infrastructure migration

## Testing Strategy

All changes so far are **additive only** - new types coexist with existing Attrs-based code:
- No existing behavior is modified
- All 253 unit tests continue to pass
- Integration tests remain unchanged
- New types are unused by production code until Phase 4

This allows incremental, safe migration with rollback capability at any point.
