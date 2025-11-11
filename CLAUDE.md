# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## libfetchers Typed Inputs Project

**Branch**: `libfetchers-typed`

**Goal**: Use accurate types throughout libfetchers internals, relegating the dynamically typed `Input` (with `Attrs`) to the outer API boundary completely.

**Architectural Vision**:
- **Inner boundary**: All libfetchers internals use typed inputs exclusively (`GitLockedInput`, `PathFinalInput`, etc.)
- **Outer boundary**: `Input` class with `Attrs` exists only at API surface for backward compatibility
- **No `Attrs` manipulation inside libfetchers** except at conversion points

**Current Status**: Phase 4 COMPLETE ✅ - All fetchers use typed inputs internally

**Key Documentation**:
- `doc/libfetchers-typing-plan.md` - Complete architectural plan and migration strategy
- `doc/libfetchers-phase4-complete.md` - Phase 4 completion summary (latest)
- `doc/libfetchers-implementation-status.md` - Historical progress tracking

## Development Commands

### Build System
- **Build system**: Meson (with Ninja backend)
- **Shell environment**: Specified in `.envrc` (use `native-clangStdenv` for this repo)
- **Configure**: `nix develop .#native-clangStdenv --configure`
- **Build**: `(cd build && meson compile <target>)`
- **Test**: `(cd build && ./src/<lib>-tests/<test-binary>)`

### Quick Setup Workflow

**Initial setup (from source root):**
```bash
# Configure build with correct environment (reads .envrc automatically)
nix develop .#native-clangStdenv --configure

# Build specific targets (much faster than full build)
(cd build && meson compile nix-expr-tests)

# Run specific tests
(cd build && ./src/libexpr-tests/nix-expr-tests --gtest_filter="*test_name*")
```

**Incremental development:**
```bash
# After making changes, just rebuild the target you need
(cd build && meson compile nix-expr-tests)

# Run your specific test
(cd build && ./src/libexpr-tests/nix-expr-tests --gtest_filter="*your_test*")
```

### Legacy Development Tasks (for reference)

**Build Nix (full build - slower):**
```bash
# In development shell
configurePhase
buildPhase
```

**Run tests:**
```bash
# Unit tests
checkPhase

# Functional tests - MUST use meson test (run from source root)
meson test -C build --print-errorlogs socket-only-substituter
```

**Install locally:**
```bash
installPhase
# Binary will be available at ./outputs/out/bin/nix
```

**Format code:**
```bash
./maintainers/format.sh
```

**Pre-commit hooks:**
```bash
pre-commit-hooks-install
```

### Testing
- **Unit tests**: Located in `src/*/tests/` directories, run with `checkPhase`
- **Functional tests**: Located in `tests/functional/`, shell-based tests
  - **MUST use meson test** (run from source root): `meson test -C build --print-errorlogs <test-name>`
  - Use `expectStderr <exit-code> <command>` to test error conditions
  - `expectStderr` redirects stderr to stdout (2>&1) so it can be piped to `grepQuiet`
  - Example: `expectStderr 1 nix eval --expr 'abort "test"' | grepQuiet "test"`
  - Verifies the command exits with the expected code and checks error message content
  - Defined in `tests/functional/common/functions.sh:200`
- **Integration tests**: Located in `tests/nixos/`

## Architecture Overview

### Core Libraries (in dependency order)
1. **libutil** (`src/libutil/`): Core utilities, data structures, and platform abstractions
2. **libstore** (`src/libstore/`): Store abstraction, derivations, and store operations
3. **libfetchers** (`src/libfetchers/`): Input fetching (git, github, tarball, etc.)
4. **libexpr** (`src/libexpr/`): Nix language evaluator and built-in functions
5. **libflake** (`src/libflake/`): Flake support and lockfile management
6. **libmain** (`src/libmain/`): Common main function utilities
7. **libcmd** (`src/libcmd/`): Command-line interface framework

### Key Components

**Nix Language (`src/libexpr/`)**:
- Parser: `parser.y` (bison) and `lexer.l` (flex)
- Evaluator: `eval.cc` with lazy evaluation semantics
- Built-ins: `primops.cc` and `primops/` directory
- Value representation: `value.hh` and `value/context.cc`

**Store Abstraction (`src/libstore/`)**:
- Store API: `store-api.cc` - abstract interface for all stores
- Local store: `local-store.cc` - `/nix/store` implementation
- Remote stores: Various `*-store.cc` files for SSH, HTTP, S3, etc.
- Derivations: `derivations.cc` - build recipe representation
- Content addressing: `content-address.cc` for CA derivations

**Command Framework (`src/libcmd/`)**:
- Installables: `installables.cc` - unified interface for packages/derivations
- REPL: `repl.cc` - interactive Nix evaluator

**Main Binary (`src/nix/`)**:
- Modern CLI with subcommands
- Each subcommand typically has its own `.cc` file
- Markdown documentation embedded in `.md` files

### Build System Details
- Uses Meson with subprojects in `src/`
- Each library has its own `meson.build` file
- Cross-compilation support via Nixpkgs
- Testing integrated into build system

### C API
- C bindings for major libraries in `src/lib*-c/` directories
- Designed for language bindings and external integration

## File Structure Patterns
- Headers in `include/nix/[library]/` subdirectories
- Platform-specific code in `unix/`, `windows/`, `linux/` subdirectories
- Test support libraries in `lib*-test-support/`
- Actual tests in `lib*-tests/`

## Development Notes
- All C++ code should follow existing patterns and style
- Use existing error handling mechanisms from `libutil/error.hh`
- Prefer lazy evaluation patterns when working with expressions
- Store operations should go through the Store API abstraction
- Use structured bindings and modern C++ features where appropriate

### Session Setup Checklist
At the start of each development session, check:
1. **Working directory**: Should be `/home/user/src/nix-master` (source root), not `build/`
2. **Required files**: `meson.build`, `flake.nix`, `src/` directory should be visible
3. **Development environment**: Run `nix develop` or `source build/claude.env` as needed

If launched from wrong directory, inform user and suggest relaunching from source root.

## Additional Documentation
- **Interrupt Handling**: See `docs/interrupt-handling.md` for detailed explanation of Nix's signal handling and graceful cancellation system

## Logging Architecture

### Current Logging System
Nix has a flexible logging architecture with multiple output formats:

**Available Log Formats** (`src/libmain/loggers.cc`):
- `raw` - Simple text output, low verbosity
- `raw-with-logs` - Simple text with build logs
- `internal-json` - Structured JSON (very verbose, not AI-friendly)
- `bar` - Progress bar with real-time updates (default, TTY only)
- `bar-with-logs` - Progress bar + build logs

**Core Logger Classes** (`src/libutil/logging.cc`):
- `SimpleLogger` - Basic text output, used by raw formats
- `ProgressBar` (`src/libmain/progress-bar.cc`) - Interactive progress display
- `TeeLogger` (`src/libutil/tee-logger.cc`) - Multiplexes to multiple loggers
- `JSONLogger` - Structured output (generates excessive noise)

### Progress Bar Implementation Details
**Update Mechanism** (`src/libmain/progress-bar.cc:103-112`):
- Background thread wakes every 50ms maximum (20 Hz rate limit)
- 10ms debouncing for short-lived activities to prevent flicker
- Thread-safe state management using `Sync<State>` pattern
- Tracks all activities with start times, types, and descriptions

**Activity Tracking**:
- `ActInfo` struct contains activity metadata and timing
- Activities organized by type (build, substitute, download, etc.)
- Real-time progress updates via `result()` method

### AI-Friendly Logging Enhancement

**Problem**: Raw format can be silent for long periods during builds, making it unclear if progress is being made.

**Solution**: `ActivityTracker` class that provides periodic activity summaries alongside normal raw logging.

**Implementation** (`src/libutil/logging.cc`):
```cpp
class ActivityTracker : public Logger {
    // Tracks ALL activities regardless of verbosity level
    // Runs background thread that reports every 10 seconds
    // Generates smart summaries: "→ 2 builds active"
};
```

**Integration**: 
- Raw formats (`LogFormat::raw`, `LogFormat::rawWithLogs`) now use `TeeLogger`
- Combines `SimpleLogger` + `ActivityTracker` for dual output streams
- ActivityTracker produces low-volume, informative progress summaries

**Output Example**:
```
building hello-2.12.1...          # SimpleLogger
→ 2 builds active                 # ActivityTracker (10s later)
→ 7 downloads active              # ActivityTracker (activity changed)
copying path '/nix/store/...'     # SimpleLogger
```

### Development Environment Setup

**First time setup (from source root)**:
```bash
# Configure the build with correct environment (reads .envrc automatically)
nix develop .#native-clangStdenv --configure
```

**Alternative method - From build directory**:
```bash
# Generate development environment
nix print-dev-env >claude.env

# Load environment and configure (from source root)
source build/claude.env && configurePhase
```

**Important**: Claude Code directory restrictions may prevent `cd` to parent directories. If launched from `build/`, you'll need to exit and relaunch from the source root, or work around the restriction by using relative paths in commands.

### Key Insights for AI Usage
1. **Use `--log-format raw`** to avoid progress bar noise in token consumption
2. **ActivityTracker provides heartbeat** during long silent periods
3. **JSON format is broken** - generates 300+ lines for simple operations
4. **Raw format is actually quite good** for both humans and AI when enhanced with periodic summaries