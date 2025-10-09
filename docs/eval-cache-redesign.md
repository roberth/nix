# Evaluation Cache Redesign

## Problem Statement

The current evaluation cache maps entire flakes to their output attribute values. This coarse granularity makes cache entries useless as soon as even a small change is made to any part of the flake, even if that change doesn't affect the specific attribute being evaluated.

Example: Changing a file in `src` invalidates the cache for all packages in the flake, despite it having no effect on `nix develop`.

## Design Goals

1. **Fine-grained cache entries**: Cache entries should remain applicable as long as the *relevant* files have not changed
2. **Dependency tracking**: Accurately track which files/inputs each evaluation depends on
3. **Reusability**: Maximize cache hit rate across small changes to a codebase
4. **Purity**: Cache only pure computations; maintain Nix's evaluation purity guarantees

## Solution Overview

We redesign the evaluation cache as a "man in the middle" layer that sits between the environment and evaluator. The evaluator becomes a pure message processor with no direct access to external state. Instead of caching coarse-grained flake outputs, we cache fine-grained traces of evaluator-environment interactions.

**Important**: The "message passing" described throughout this document is a conceptual model for understanding how we trace and cache evaluation, not an actual message-passing implementation. The real implementation uses direct function calls through well-defined interfaces. We use message terminology because it provides a clear mental model for reasoning about traces, dependencies, and cache replay.

Each trace records the specific files read, their content hashes, and the resulting values. Cache entries remain applicable as long as the recorded file hashes still match the current environment, enabling cache reuse even when unrelated files change.

## Definitions

- **Message**: An in-memory object representing a request or response exchanged between the evaluator and environment interfaces (e.g., `READ_FILE`, `FILE_CONTENT`). These are passed as direct function calls, not serialized.

- **Idempotence**: A message is idempotent if repeating the same request within an evaluation context always returns the same response. This follows from Nix's purity model.

- **Handle**: An identifier (like `v0`, `v1`) used in messages to refer to values or resources during evaluation.

- **Content addressing / Flattening**: Converting handles to content-based identifiers (likely merkle hashes) so that two handles referring to the same content are considered equivalent across evaluations.

- **Trace**: A persistent record in the cache of messages exchanged during an evaluation, capturing both the evaluator's requests and the environment's responses. Traces are distinct from the messages themselves—they are the stored history. Note that, unlike Messages, trace entries do not store full file contents from the environment; they store hashes for inputs (for applicability checking) and only store full content for outputs/results.

- **Event group**: A collection of messages (events) that occur together as part of a single logical computation. These groups form the nodes in the cache's graph structure.

- **Applicability**: A cache entry is applicable when the current environment state matches the cached assumptions (e.g., file hashes match). This is distinct from validity—we never invalidate cache entries, only check if they apply.

- **Context extension**: The pattern where evaluation proceeds incrementally: after evaluating one expression, the environment provides the next task, building on the previous context (e.g., "now evaluate the `packages` attribute of the flake you just loaded").

- **Environment**: All information the evaluator can pull in. This includes files, environment variables, source fetching. User/caller input can be lumped in; see [Evaluator as Pure Message Processor](#Evaluator-as-Pure-Message-Processor). Retrieval operations are assumed Idempotent.

- **Build impurity**: In-derivation behavior that does not always produce the same derivation output.

- **Impure fetching**: Retrieval of non-unique information. Example: reading a file from the local file system. Non-example: fetching a Git tree by a commit hash.

## Core Concepts

### Cache as "Man in the Middle"

The evaluation cache sits between the user layer (e.g. `nix` CLI) and the evaluator, implementing the same interface as the real evaluator (`Interpreter`).

```mermaid
graph LR
    User <--> Evaluator <--> Environment
```

It can:

1. **Record traces** of evaluator-environment interactions during real evaluation
2. **Replay traces** when the environment state matches a cached trace
3. **Check applicability** of cached traces against current environment state
4. **Fall through** to real evaluation when any cache miss occurs

The "man in the middle" architecture: `EvalCache` intercepts messages between the environment and the real evaluator (`Interpreter`), recording traces and serving cached results when applicable.

### Evaluator as Pure Message Processor

The "man in the middle" model provides a good analogy for how this works, but when naively elaborated into an implementation, it is more complicated than necessary.

Instead of modeling two streams of messages, we "merge" the ambient environment and the user layer into a single `Environment`.

```mermaid
graph LR
    Environment["Environment and User"] <--> Evaluator
```

This way, we model the evaluator as a pure function that communicates with its environment solely through a message protocol. The evaluator:
- Receives all inputs (including what to evaluate) via messages from the environment
- Produces all outputs (including evaluation results) via messages to the environment
- Has no direct access to filesystem, store, or other external state
- Initiates conversation by asking the environment "what should I do?" (via `READY()` message)

This is conceptually similar to a precursor of Haskell's IO model, where `main` is a lazy function from a list of inputs to a list of actions.

The evaluator is completely reactive — it only processes messages.

### Trace Structure: DAG Overlaying Linear Sequences

<!-- Status: unclear; could be redesigned entirely -->

While messages are idempotent (same request always produces same response within an evaluation), the actual evaluation follows a linear sequence. The cache consists of **event groups**-collections of messages that occur together as part of a computation. These groups form a directed acyclic graph (DAG):

- Each group may lead to different subsequent groups depending on environment responses (e.g., if a file contains different imports, different READ_FILE requests follow)
- The DAG overlays many possible linear sequences of events
- Only one linear sequence is actually executed in any given evaluation
- The graph structure exists because we record multiple evaluation paths over time

**Duality**: From the evaluator's perspective, evaluation is a linear sequence of messages. From the cache's perspective, the accumulated traces form a graph that can be explored to predict which files will be needed next.

### Causality and Path Dependencies

While messages are idempotent, there is implicit ordering due to causality:

- The content of a file determines which other files to read (e.g., imports in `default.nix`)
- Previously loaded files won't be re-requested within the same evaluation context (path dependency)
- The set of files read after processing `flake.nix` is determined by its content, so that set is predictable given the file's hash

This means the cache can check applicability by:
1. Verifying initial file hashes match
2. Following the recorded trace as long as environment responses match
3. Falling through to real evaluation when any response differs

## Architecture

### Component Roles

**Evaluator**: Abstract interface for evaluation that both `Interpreter` and `EvalCache` implement. Processes messages and maintains no external side effects. Similar to `EvalState`, which is an implementation detail of `Interpreter`.

**Interpreter**: Real evaluator implementation. In the refactored design, `EvalState` becomes an implementation detail of `Interpreter` rather than being directly exposed.

**EvalCache**: Cache layer that implements the same interface as `Interpreter`. Records traces during cache misses, replays traces on cache hits.

**Environment**: Provides I/O operations to the evaluator. The same interface serves both the real evaluator and the cache, regardless of whether the cache is replaying or tracing.

### Message Protocol

The evaluator initiates conversation by asking what to do. It does this whenever it is "done"—including when it has just started. The `READY()` message may be useful for segmenting traces if needed (this is uncertain).

**Example message flow:**

```
Evaluator → Environment: READY()
Environment → Evaluator: EVAL_FILE(path="/path/default.nix", accessor_handle=src0, value_out_handle=v0)
Evaluator → Environment: READ_FILE(path="/path/default.nix", accessor_handle=src0)
Environment → Evaluator: FILE_CONTENT(path="/path/default.nix" accessor_handle=src0, content=<bytes>)
Evaluator → Environment: READ_FILE(path="/path/lib.nix", accessor_handle=v0)
Environment → Evaluator: FILE_CONTENT(path="/path/lib.nix", accessor_handle=v0, content=<bytes>)
... (more file reads as evaluation proceeds)
Evaluator → Environment: VALUE_RESULT(value_handle=v0, type=ATTRSET)
Evaluator → Environment: READY()  // perhaps, or implied by VALUE_RESULT
Environment → Evaluator: EVAL_ATTR(value_handle=v0, attr="hello", value_out_handle=v1)
Evaluator → Environment: READ_FILE(path="/path/hello.nix", accessor_handle=src0)
Environment → Evaluator: FILE_CONTENT(path="/path/hello.nix", accessor_handle=v0, content=<bytes>)
Evaluator → Environment: VALUE_RESULT(handle=v1, type=ATTRSET)
...
```

This shows the context extension pattern: after the evaluator reports it has an attrset, the environment requests evaluation of a specific attribute, and evaluation continues in that extended context.

## Design Details

### Cache Entry Structure

Cache entries are fine-grained, capturing individual computations with their dependencies.

Cache entries store:
- **For inputs** (e.g., file reads): Only hashes, used for applicability checking
- **For outputs** (e.g., evaluation results): Full content, since this is what we're caching
- **Metadata**: Context pointers, request/response types, usage statistics

This design minimizes storage by not duplicating file contents that already exist in the filesystem, while preserving the actual evaluation results we need to replay.

<!-- Status: VERY DRAFT -->

From a code perspective, the data model could be similar to:

```c++
// C++ pseudo-code
struct Entry {
  optional<Entry *> context;
  variant<Request, Response> message;
  int times_used; // stats
}
```

The database could be written as a log, with indexes that make the entries queryable in the reverse direction. Naively:

```c++
using Index =
  map<
    // context (or start) and next request
    pair<optional<Entry *>, Request>,

    // map from response hash to the entry containing that response
    map<
      hash<Response>,  // hash of the response for quick lookup
      Entry *          // the actual entry containing this response
    >
  >;
```

A single command like `nix build` may use dozens or hundreds of these fine-grained entries. Each entry represents one event group in the cache's DAG.

### Trace Selection and Applicability

The cache contains multiple traces—different evaluation paths taken in past runs. Applicability checking is the process of selecting which trace (if any) matches the current environment state.

**During replay mode:**

1. **EvalCache acts as Evaluator**, replaying a previously recorded evaluation
2. **At each step**, when EvalCache generates a request (e.g., `READ_FILE(path)`):
   - Query cache index: `Index[(current_context, request)]`
   - Get map of candidate responses from different past traces
   - Each candidate represents a different branch (e.g., same file had different contents in different runs)

3. **Select the applicable trace branch**:
   - For each candidate response (e.g., `FILE_CONTENT(path, hash=X)`):
     - Query current Environment: "What's the current hash of this path?"
     - If current hash matches X, this trace branch is applicable
   - At most one candidate matches (file content is deterministic at a point in time)
   - If no candidates match, cache miss—fall through to tracing mode

   Note: Multiple evaluation traces naturally form a graph structure in the cache. When different evaluations share common prefixes (e.g., they all start by reading `flake.nix` with the same content), those shared portions become common paths in the graph. The traces diverge only when they encounter different file contents or make different evaluation choices, creating branches in the graph.

4. **Continue on selected trace**:
   - Use the selected response entry as the new context
   - Continue replaying, making trace selections at each step
   - Each selection narrows down which recorded evaluation we're following

5. **Result**: By incrementally selecting applicable trace branches, the cache replays a complete evaluation without running the real Interpreter

Note: We never "invalidate" cache entries. Entries remain in storage; we select which traces are applicable to the current environment state at query time.

### Recording vs Replay Modes

**Recording mode** (cache miss):
- EvalCache forwards messages to Interpreter
- Records both requests and responses
- Builds new trace entries in the cache storage

**Replay mode** (cache hit):
- EvalCache looks up cached entry for current context
- Checks applicability (hash matching)
- If applicable, returns cached result directly
- If not applicable, switches to recording mode

## Implementation Strategy

This is a significant refactoring project requiring substantial changes before message protocol design can begin.

**Implementation Architecture**: While we use "message passing" as a conceptual model throughout this document, the actual implementation uses direct function calls through well-defined interfaces. The `Evaluator` and `Environment` interfaces exchange in-memory objects (the "messages"), and the `EvalCache` layer records these exchanges as traces for caching purposes. There is no IPC or serialization involved in the message exchange itself - only in the persistent storage of traces.

The approach:

### Phase 1: Prepare codebase

Define Interfaces

1. Create abstract `Evaluator` interface
2. Create abstract `Environment` interface

Refactor Existing Evaluator

1. Identify all side effects in `libexpr` (file reads, store access, etc.)
   - Audit `eval.cc`, `primops.cc` for filesystem access
   - Find all `readFile`, `import`, `readDir` operations
   - Identify store operations (`isValidPath`, `queryPathInfo`)
   - Locate network operations (fetchers, downloads)
   - Map environment variable access points

2. Route them through `Environment` interface
   - Create methods like `readFile()`, `readDir()`, `queryStore()`
   - Replace direct filesystem calls with `Environment` method calls
   - Ensure all I/O goes through this single interface

3. Make `EvalState` an implementation detail of `Interpreter`
   - Most current `EvalState` consumers should be switched to use the `Evaluator` interface instead
   - `Interpreter` becomes the concrete implementation that internally uses `EvalState`
   - Only `Interpreter` should directly access `EvalState`; all other code uses the abstract `Evaluator` interface

4. Unify existing coarse-grained cache under new `Evaluator` interface
   - Adapt current `eval-cache.cc` to work through the new interfaces
   - This validates the interface design before implementing fine-grained caching

This phase is exploratory: starting by unifying the existing cache and evaluator under the new interfaces will help validate the design.

### Phase 2: Implement Fine-Grained Cache

1. Define message types for "protocol"
2. Enumerate all operations that need to flow through these interfaces
3. Implement trace recording in `EvalCache`
4. Implement cache lookup/replay logic
5. Implement cache storage format

### Phase 3: Optimization
1. Evaluate performance characteristics
2. Optimize cache storage format
3. Tune granularity of cache entries
4. Address performance-sensitive decisions from Open Questions

## Open Questions

1. **Handle equivalence**: In more dynamic or non-deterministic usage, handles will be created in a different order.
   In the current design, an incrementing number is assigned to value handles and source handles.
   These are not stable identifiers. Possible directions:
   - Maintain a handle renaming map when ingesting traces (reminiscent of *unification* in type systems)
   - Use fingerprints whenever possible. E.g. `(sourceref, number)` where the number is only a fallback.
     This loses worktree location invariance if not taken into account.

2. **Store operations**: Should we include store operations (`isValidPath`, relevant `builtins.readFile` calls) in traces?
   - **Option A**: Assume purity, don't trace (simpler, faster, but breaks with build impurities)
   - **Option B**: Include in trace (more accurate, larger storage, handles impure builds)
   - **Option C**: For fetched sources, we may use file granularity or course store objects, or perhaps both (somewhat like a "skip list")
   - **Tentative decision**: Include by principle, evaluate performance impact empirically

3. **Concurrency opportunities**: Can message idempotence enable parallel evaluation? What are the challenges? Expected:
   - Handle equivalence more important
   - Gradual refinement of traces: when tracing for the second time with a different set of requests, and the intersection of responses is the same, it seems valid to push requests that are outside the intersection into the subsequent contexts. Does this improve replay performance (cache hits, amount of I/O for applicability checking)

4. **Cache prediction/prefetching**: Can we use the DAG structure to predict and prefetch files before they're requested?

5. **IFD handling**: Import-from-derivation requires building during evaluation.
   - IFD becomes another Environment operation that builds and returns store paths
   - Cache traces would record both the store path and a store object hash
   - Applicability checking would verify the store object hash matches

6. **Non-determinism**: How to detect and handle genuinely non-deterministic evaluation?
   - Interpreter non-determinism: evaluation order, attrset iteration order
   - Caller non-determinism: different evaluation requests in different orders
   - These may affect handle assignment order (see handle equivalence question)
   - Primops like `builtins.currentTime` are handled like other environment-reading operations

7. **Cache storage**: SQLite? Custom files? How to handle concurrent access?

8. **Cache eviction**: When/how to remove old cache entries?

9. **Performance tuning**: What's the overhead of replayed vs traced vs direct evaluation?

## Success Metrics

1. **Cache hit rate**: >90% when changing unrelated files in a flake
2. **Evaluation speed**: Cache hits should be at least 5× faster than full evaluation
3. **Cache size**: <100MB for typical project with ~1000 evaluations cached
4. **Correctness**: Cached results must be identical to fresh evaluation (zero tolerance)

## Related optimizations

The following optimizations are related to the evaluation cache, but are considered mostly out of scope for this project.

### File hash cache

Quick access to file hashes further improves the performance of the system.
It is possible to maintain a mapping:

    local file path -> (mtime, hash)

If the real mtime changes, the entry is discarded on lookup.

This approach is similar to Git's index, which also uses mtime to avoid re-hashing unchanged files. The mtime-based approach has known limitations when mtimes are explicitly set (e.g., archive extraction, some build tools). Content-addressed stores naturally avoid this problem by making the hash the primary identifier.

Can be mitigated by opt-in setting, async verification (less I/O improvement, but may allow concurrent replays to proceed), or a change monitoring solution built on inotify or similar.

This is implementable behind the `SourceAccessor` interface whereas the new eval cache only *calls* that interface.

### AST cache, bytecode

These optimizations apply independently to the `Interpreter` component, and can improve its performance.
Optimizations within the `Interpreter` generally do not affect the caching strategy.

## References

- Current eval cache: `src/libexpr/eval-cache.hh`, `src/libexpr/eval-cache.cc`
- Current evaluator: `src/libexpr/eval.hh`, `src/libexpr/eval.cc`
- Flake evaluation: `src/libflake/flake.cc`
