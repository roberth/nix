# Tracing Eval Cache Implementation

This document describes the implementation architecture for the tracing evaluation cache prototype, as implemented in the last two commits of the `eval-cache` branch.

For the high-level design rationale, see `docs/eval-cache-redesign.md`.
For the `Evaluator` and `Object` interfaces, see `doc/evaluator-architecture.md`.

## Overview

The tracing eval cache records all I/O operations during evaluation to enable fine-grained cache invalidation. The implementation follows a **decorator pattern**: each tracing component wraps an inner component, intercepting operations to record them before delegating.

```
┌─────────────────────────────────────────────────────────────────┐
│                        User (nix build)                         │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                      TracingEvaluator                           │
│  - Logs user queries (evalFile, evalExpr) to TraceFile          │
│  - Preloads files from previous trace on construction           │
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

### TracingEvaluator (`tracing-evaluator.hh`)

Wraps an `Evaluator` to trace user queries.

**Tracing**: Logs `QueryImport` / `QueryExpr` before evaluation, logs `ResultType` after, returns `TracingObject` wrappers.

**Preloading**: On construction, reads the previous trace and preloads files:

1. Get file paths from `TracingDatabase::getTracedFilePaths()`
2. Read file contents in parallel using `ThreadPool`
3. Parse files sequentially (EvalState parsing is not thread-safe)
4. Insert into `fileEvalCache` wrapped in `ExprSpeculativeParseTrigger`

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
DECLARE_QUERY_RESULT(QueryGetAttr, ResultType)
DECLARE_QUERY_RESULT(QueryGetString, ResultString)
// ... etc
```

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

**Not implemented**:
- Flakes not yet supported (embedded store paths make cache hits impossible)
- Replay not yet implemented (tracing only)

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
| `src/libexpr/include/nix/expr/trace-types.hh` | Trace entry type definitions |
| `src/libexpr/include/nix/expr/tracing-database.hh` | TraceFile and TracingDatabase |
| `src/libexpr/include/nix/expr/tracing-evaluator.hh` | TracingEvaluator |
| `src/libexpr/include/nix/expr/tracing-object.hh` | TracingObject |
| `src/libexpr/include/nix/expr/tracing-environment.hh` | TracingEnvironment |
| `src/libexpr/include/nix/expr/tracing-source-accessor.hh` | TracingSourceAccessor |
| `src/libexpr/tracing-*.cc` | Implementations |
