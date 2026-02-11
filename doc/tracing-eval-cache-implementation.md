# Tracing Eval Cache Implementation

This document describes the implementation architecture for the tracing evaluation cache prototype.

For the high-level design rationale, see `docs/eval-cache-redesign.md`.
For the `Evaluator` and `Object` interfaces, see `doc/evaluator-architecture.md`.

## Overview

The tracing eval cache records all I/O operations during evaluation to enable fine-grained cache invalidation. The implementation follows a **decorator pattern**: each tracing component wraps an inner component, intercepting operations to record them before delegating.

There are two main flows:

1. **Recording** (`TracingEvaluator` → `TracingObject`): Records a trace during evaluation
2. **Replay** (`TracingReplayEvaluator` → `TracingReplayObject`): Replays cached results from a previous trace

```
┌─────────────────────────────────────────────────────────────────┐
│                        User (nix build)                         │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                   TracingReplayEvaluator                        │
│  - Looks up queries in QueryIndex from previous trace           │
│  - Validates file hashes via FileHashCache                      │
│  - Validates env vars match recorded values                     │
│  - Falls back to TracingEvaluator on cache miss                 │
└─────────────────────────────────────────────────────────────────┘
                                │ (on miss)
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                      TracingEvaluator                           │
│  - Logs user queries (evalFile, evalExpr) to TraceFile          │
│  - Lazily preloads files from previous trace on first eval      │
│  - Wraps returned Objects in TracingObject                      │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                         Interpreter                             │
│  - Real evaluation via EvalState                                │
│  - Uses TracingEnvironment for I/O                              │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                     TracingEnvironment                          │
│  - Logs environment queries (getEnv) to TraceFile               │
│  - Provides TracingSourceAccessor for file access               │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                   TracingSourceAccessor                         │
│  - Logs file reads with content hashes                          │
│  - Provides readSpeculatively() for deferred tracing            │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                      Real Filesystem                            │
└─────────────────────────────────────────────────────────────────┘
```

## Components

### TraceFile (`tracing-database.hh`)

Writes JSON trace entries to a file. Manages value handle allocation.

```cpp
class TraceFile {
    uint64_t allocValue();           // Allocate new value handle
    void log(const json & entry);    // Write entry to trace

    template<typename T>
    uint64_t logQuery(const T & q);  // Log query, return handle

    template<typename T>
    void logResult(uint64_t v, const T & r);  // Log result for handle
};
```

### TracingDatabase (`tracing-database.hh`)

Manages trace file storage in `~/.cache/nix/eval-tracing-v0/traces/`.

- `newTraceFile()` - Create a new timestamped trace file
- `updateLatestSymlink()` - Update `latest.json` symlink after trace completion
- `latestTraceFile()` - Get path to most recent trace
- `getTracedFilePaths()` - Extract file paths from a trace (for preloading)

### TracingReplayEvaluator (`tracing-replay-evaluator.hh`)

Wraps an `Evaluator` to replay cached results from a previous trace.

**Lookup**: Uses `QueryIndex` for O(1) query lookup in the trace.

**Validation**: Before returning a cached result:
1. Validates file content hashes via `FileHashCache`
2. Validates environment variables match recorded values
3. Only returns cached results if all validations pass

**Fallback**: On cache miss or validation failure, delegates to the inner evaluator (typically `TracingEvaluator`) and marks the cache as invalidated.

### TracingReplayObject (`tracing-replay-object.hh`)

Wraps cached trace data to implement the `Object` interface.

Each `TracingReplayObject` holds a value handle and a lazy fallback function. Operations like `maybeGetAttr()`, `getString()`, etc. look up results in the trace index. On cache miss, the fallback is activated to get the real object.

**Store validation**: For `getStringWithContext()`, validates that context store paths still exist before returning cached results.

### TracingEvaluator (`tracing-evaluator.hh`)

Wraps an `Evaluator` to trace user queries (recording side).

**Tracing**: Logs `QueryImport` / `QueryExpr` before evaluation, logs `ResultType` after, returns `TracingObject` wrappers.

**Lazy Preloading**: Defers file preloading to the first `evalFile`/`evalExpr` call via `ensurePreloaded()`:

1. Get file paths from `TracingDatabase::getTracedFilePaths()`
2. Read file contents in parallel using `ThreadPool`
3. Parse files sequentially (EvalState parsing is not thread-safe)
4. Insert into `fileEvalCache` wrapped in `ExprSpeculativeParseTrigger`

This avoids unnecessary I/O when `TracingReplayEvaluator` can satisfy all queries from cache.

### TracingObject (`tracing-object.hh`)

Wraps an `Object` to trace value operations.

Each `TracingObject` holds a value handle (`valueNum`). Operations like `maybeGetAttr()`, `getString()`, etc. log queries with the handle before delegating, and log results after.

```cpp
std::shared_ptr<Object> maybeGetAttr(const std::string & name) override {
    auto valueId = traceFile.logQuery(trace::QueryGetAttr{name, valueNum});
    auto result = inner->maybeGetAttr(name);
    if (result) {
        traceFile.logResult(valueId, trace::ResultType{...});
        return new TracingObject(result, traceFile, valueId);
    }
    return nullptr;
}
```

### TracingEnvironment (`tracing-environment.hh`)

Wraps an `Environment` to trace I/O operations.

- `getEnv()` - Logs `GetEnvRequest` / `GetEnvResponse`
- `fsRoot()` - Returns a `TracingSourceAccessor`

### TracingSourceAccessor (`tracing-source-accessor.hh`)

Wraps a `SourceAccessor` to trace file reads.

- `readFile()` - Reads file, computes SHA256 hash, logs `FileReadRequest` / `FileReadResponse`
- `readSpeculatively()` - Reads without tracing; returns content plus a deferred `emitTrace()` callback

**Speculative reads** enable parallel file preloading: files are read and parsed ahead of time, but the trace is only emitted when the file is actually demanded during evaluation.

## Trace Format

Traces are JSON files with one entry per line. Entry types:

### Environment Messages (request/response pairs)

```json
{"request": {"absPath": "/path/to/file.nix"}, "response": {"contentHash": "sha256:abc..."}}
{"request": {"name": "NIX_PATH"}, "response": {"value": "/nix/var/..."}}
```

### User Messages (queries and results with handles)

```json
{"query": {"import": "/path/to/default.nix"}, "v": 0}
{"result": {"type": "set"}, "v": 0}
{"query": {"getAttr": "hello", "from": 0}, "v": 1}
{"result": {"type": "set"}, "v": 1}
{"query": {"getAttr": "drvPath", "from": 1}, "v": 2}
{"result": {"value": "/nix/store/..."}, "v": 2}
```

## Trace Types (`trace-types.hh`)

Type-safe trace entries using template specialization:

```cpp
// Request/response pairs for environment operations
DECLARE_TRACE_PAIR(FileReadRequest, FileReadResponse)
DECLARE_TRACE_PAIR(GetEnvRequest, GetEnvResponse)

// Query/result pairs for user operations
DECLARE_QUERY_RESULT(QueryImport, ResultType)
DECLARE_QUERY_RESULT(QueryExpr, ResultType)
DECLARE_QUERY_RESULT(QueryGetAttr, ResultMaybeType)  // nullopt for missing attrs
DECLARE_QUERY_RESULT(QueryGetString, ResultString)
DECLARE_QUERY_RESULT(QueryGetStringWithContext, ResultStringWithContext)
// ... etc
```

### QueryIndex (`trace-types.hh`)

Provides O(1) lookup for queries in a parsed trace. Built by scanning the trace once and indexing queries by their content.

```cpp
class QueryIndex {
    std::map<QueryVariant, IndexEntry> index;
public:
    explicit QueryIndex(const std::vector<TraceEntry> & trace);

    template<typename Q>
    std::optional<IndexEntry> lookup(const Q & query) const;
};
```

Only queries with matching results are indexed (incomplete traces are rejected).

## FileHashCache (`file-hash-cache.hh`)

SQLite-backed cache mapping file paths to their SHA-256 content hashes. Uses mtime to detect when cached entries need revalidation.

```cpp
class FileHashCache {
    Hash getHash(const std::filesystem::path & path);      // Get or compute
    std::optional<Hash> lookup(const std::filesystem::path & path);  // Lookup only
    void invalidate(const std::filesystem::path & path);   // Clear entry
};
```

Stored in `~/.cache/nix/file-hash-cache.sqlite` with schema:
- `path` (text, primary key)
- `mtime` (integer, from `std::filesystem::last_write_time`)
- `hash` (text, SRI format)

## Speculative Preloading

The preloading optimization reads and parses files from the previous trace before they're needed:

```
Previous trace ──► getTracedFilePaths() ──► [path1, path2, ...]
                                                    │
                        ┌───────────────────────────┘
                        ▼
              ┌─────────────────────┐
              │  ThreadPool (I/O)   │  Read files in parallel
              └─────────────────────┘
                        │
                        ▼
              ┌─────────────────────┐
              │  Sequential parse   │  EvalState::parseExprFromString
              └─────────────────────┘
                        │
                        ▼
              ┌─────────────────────┐
              │  fileEvalCache      │  ExprSpeculativeParseTrigger wrappers
              └─────────────────────┘
```

When a preloaded file is demanded during evaluation:

1. `EvalState::evalFile()` finds the cached thunk
2. Thunk evaluation triggers `ExprSpeculativeParseTrigger::eval()`
3. `emitTrace()` is called (deferred trace emission)
4. Inner expression is evaluated normally

## Parallel Parsing (Not Implemented)

An earlier commit attempted parallel parsing, but this was reverted. Parallel parsing requires significant optimization to avoid `SymbolTable` contention - symbols are interned during parsing, and the current implementation uses locks that create a bottleneck. This optimization is not in scope for the current prototype.

## Enabling the Prototype

```bash
nix build --dry-run \
  --experimental-features 'nix-command flakes tracing-eval-cache' \
  --option tracing-eval-cache true \
  --impure \
  -f default.nix hello
```

**Current status**:
- Replay is implemented via `TracingReplayEvaluator` and `TracingReplayObject`
- File hash validation via `FileHashCache`
- Environment variable validation

**Not implemented**:
- Flakes not yet supported (embedded store paths make cache hits challenging)

**Opportunities**:
- Multi-threaded parsing
- "Bulk" attribute path traversal. Knowing the full path improves ability to
  concurrently load. E.g. `haskellPackages.pandoc` gives rise to three sets of
  files that are currently isolated by having to be called individually,
  sequentially:
  - The package set root loads a significant set of files
  - `haskellPackages` adds another significant set of files
  - `pandoc` may/may not add files.
  By merging all these inputs into a single pool, we improve concurrency =>
  more parallelism.

## Files

| File | Description |
|------|-------------|
| `src/libexpr/include/nix/expr/trace-types.hh` | Trace entry type definitions, QueryIndex |
| `src/libexpr/include/nix/expr/tracing-database.hh` | TraceFile and TracingDatabase |
| `src/libexpr/include/nix/expr/tracing-evaluator.hh` | TracingEvaluator (recording) |
| `src/libexpr/include/nix/expr/tracing-replay-evaluator.hh` | TracingReplayEvaluator (replay) |
| `src/libexpr/include/nix/expr/tracing-object.hh` | TracingObject (recording) |
| `src/libexpr/include/nix/expr/tracing-replay-object.hh` | TracingReplayObject (replay) |
| `src/libexpr/include/nix/expr/tracing-environment.hh` | TracingEnvironment |
| `src/libexpr/include/nix/expr/tracing-source-accessor.hh` | TracingSourceAccessor |
| `src/libexpr/include/nix/expr/file-hash-cache.hh` | FileHashCache |
| `src/libexpr/tracing-*.cc`, `file-hash-cache.cc` | Implementations |
| `src/libexpr-tests/trace-types.cc` | Trace types unit tests |
| `src/libexpr-tests/file-hash-cache.cc` | FileHashCache unit tests |
