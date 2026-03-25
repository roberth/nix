# Trace Index Data Model

This document describes the data model for the trace index.

It takes `tracing-abstract-model.md` as the theoretical foundation,
then progressively refines an optimally naive data model into an efficient index format.

## Step 0: Naive Data Model

The simplest data model is a set of traces:

```
Index = { τ₁, τ₂, τ₃, ... }
```

Each trace τ is a sequence of events as defined in the abstract model: Queries, Results, Requests, Responses.

### Operation: Replay Step

Replay is driven by external queries—the user (or calling code) issues a sequence of Queries, and we attempt to answer each from the cache. Between queries, we may maintain state.

**Replay state**: S := () for the naive model (no state kept between queries).

**Input**:
- Query Q
- R_obs: memoization map from Request → Response

**Output**: `R{Q}` or cache miss

```
replay(Q, R_obs):
    for each trace τ in Index:
        if Q appears in τ:
            let R{Q} be the Result for Q in τ
            let D = deps(τ, position of R{Q})
            if validate(D, R_obs) = valid:
                return R{Q}
    return cache miss

validate(D, R_obs):
    for each response r in D:
        if r.request in R_obs:
            if R_obs[r.request] ≠ r:
                return invalid
        else:
            current := environment.query(r.request)
            if current ≠ r:
                return invalid
    return valid
```

### Operation: Record

Recording ingests a trace into the index. Like replay, it is externally driven, but unlike replay it requires no immediate output. This allows processing on the fly, in batches, concurrently, or delayed completely. We choose to model it as the latter for simplicity.

**Input**: A complete trace τ_new

**Output**: Updated index

```
record(τ_new):
    Index := Index ∪ { τ_new }
```

## Step 1: Stateful Replay

We maintain a shrinking set of traces between replay steps.

**Replay state**: S := Index (set of traces still in play)

```
replay(Q, R_obs):
    for each trace τ in S:
        if Q appears in τ:
            let R{Q} be the Result for Q in τ
            let D = deps(τ, position of R{Q})
            if validate(D, R_obs) = valid:
                return R{Q}
            else:
                S := S \ { τ }
    return cache miss
```

When validation fails for a trace, we remove it from S. Subsequent replay steps only consider traces that haven't been eliminated.

## Step 2: Analysis of Lookups

Walking through `replay` and `validate`, we identify the lookups performed:

```
replay(Q, R_obs):
    for each trace τ in S:              # (1) iterate traces in play
        if Q appears in τ:              # (2) find Q in trace
            let R{Q} be the Result      # (3) get Result for Q
            let D = deps(τ, position)   # (4) get Responses before position
            if validate(D, R_obs):
                return R{Q}
            else:
                S := S \ { τ }
    return cache miss

validate(D, R_obs):
    for each response r in D:           # (5) iterate dependencies
        if r.request in R_obs:          # (6) lookup in observed set
            if R_obs[r.request] ≠ r:    # (7) compare responses
                return invalid
        else:
            current := environment.query(r.request)  # environment can memoize - fast
            if current ≠ r:
                return invalid
    return valid
```

**Expensive operations:**

- **(1) + (2)**: Finding Q across all traces requires scanning each trace
- **(5) + (6)**: Validating deps requires iterating all dependencies and checking each against R_obs
- **(7)**: Comparing prefixes redundantly. For queries at positions k₁ < k₂ in the same trace, validating deps [0, k₂) re-checks [0, k₁). Over n queries, this is O(n²).

**Observations:**

- **(4) deps retrieval**: Assuming random access into traces, deps is just an interval [0, position). Not expensive.
- **(3) and (4)**: Once we find Q in a trace, Result and deps are at known positions
- **deps is static**: For a given (trace, Query), the deps never change

**Possible index structures:**

For finding Q (using Merkle hash as Query identity):
- Index from hash(Q) → (trace, position, Result, deps)⁺

For deps:
- Store as interval reference into trace: (start, end)
- Or precompute and store the actual deps with each entry

**Replay state to address (7):**

Track validated position per trace: S := { τ → p } where p is the position up to which deps have been validated.

When validating deps [0, k) for trace τ:
- Let p = S[τ] (previously validated position, default 0)
- Only validate [p, k)
- Update S[τ] := k

This reduces repeated validation from O(n²) to O(n) for n queries in the same trace.

**Unsolved: environment filtering**

A Query index hash(Q) → results⁺ may return many results from traces with incompatible environments. With just this index, we must iterate and validate each result until one matches. This will degrade performance significantly after many file edit cycles. We need to do better.

## Step 3: Trie Structure and Shortcut Table

### Trie with Alternating Node Types

We can view the lookup process as traversing a trie with alternating node types:

```
Query → Request/Response* → Result → Query → Request/Response* → Result → ...
```

Each path through the trie represents a trace prefix. This naturally exploits the prefix property: once we've validated a prefix, we only validate new Request/Response pairs.

**Node types:**
- **Query node**: Indexed by hash(Q)
- **Request/Response node**: Each pair is a single node
- **Result node**: The cached result for the preceding Query

### Problem: User Order Dependence

The trie encodes the order in which Queries were issued. This creates unwanted coupling:

- `nix-build foo bar` produces trace: Q_foo → ... → R_foo → Q_bar → ... → R_bar
- `nix-build bar` looks for: Q_bar → ...
- No match: Q_bar only exists as a child of R_foo in the trie

The user's command order shouldn't affect cache hits for independent evaluations.

### Solution: Shortcut Table

Create an index table that points into the trie, allowing direct access to any node regardless of its ancestors.

**Shortcut table entry:**
- Trie node ID

**Trie nodes have parent pointers**, allowing us to trace back and reconstruct the exact dependency set for validation.

**Lookup process:**
1. Query shortcut table by hash(Q) → candidate trie node IDs⁺
2. For each candidate, trace back via trie parent pointers to get deps
3. Validate deps against R_obs and environment
4. Return first valid result

**Candidate ordering heuristics:**
- Most recent trace
- Smallest dependency set (may be more robust—recency assumes relevance, but branch switching breaks this)

**Future: Search prioritization metadata**

Additional metadata in the shortcut table could help prioritize candidates:
- Cost estimate (depth, validation work)
- Bloom filters: one for Requests, one for Response hashes (where Response includes its Request)
- Use filter overlap to rank candidates before full validation
  - Data:
    - Filter pair stored in shortcut entry
    - Filter pair maintained to match R_obs
  - Computation:
    - Mismatch parameter: Intersect filters by type.
      Bit count in Response intersection should be proportional to Request intersection.
      If Response intersection is smaller than expected, penalize (environment likely changed).
      Unreliable when filters approach saturation.
    - Loading cost: Shortcut Requests minus R_obs Requests estimates new environment queries needed.

Whether this complexity is worthwhile remains to be seen.

### Future: Decision Trees for Request/Response Sections

Instead of storing Request/Response pairs as a flat sequence, we could organize them as a decision tree:

- Identify "pivotal" Requests that determine subsequent Requests (based on history)
- Group independent Requests for concurrent loading
- Summarize groups with a single hash for faster index lookup

This is an optimization opportunity—defer until the basic structure is working.

### Recording

**record(τ):**
- Walk trace, find or create trie path
- Insert shortcut entry for each Query node
- Deduplication: if identical prefix already exists, share it

### Garbage Collection

**Simple strategy:** Drop the entire db file when it grows too large. Occasionally surprising to power users, but sufficient.

**Advanced strategy:** Generational scheme with symlink.

Directory structure:
```
{cache-dir}/eval-tracing-cache-index-v1-gen-{n}/index.sqlite
{cache-dir}/eval-tracing-cache-index-v1-latest -> gen-{n}/
```

- Symlink points to latest generation
- Clients read from symlink, can also check previous generation
- When db exceeds threshold, create new generation n+1, update symlink
- Drop generation n-2 (keep current + one previous)
- All operations idempotent—clients agree on transitions
- Directory per generation simplifies removal regardless of SQLite file flags

### Concurrency

SQLite handles concurrent access. Using content hashes instead of auto-increment IDs makes the schema more stateless and reduces contention.

## Step 4: Schema

```sql
-- Query nodes
CREATE TABLE Queries (
    nodeHash BLOB PRIMARY KEY,   -- trie identity: hash(afterHash, queryHash)
    queryHash BLOB NOT NULL,     -- semantic: hash(operation, params, inputHashes)
    afterHash BLOB,              -- temporal predecessor (Result nodeHash, or NULL for root)
    structuralParent BLOB        -- Result nodeHash for attr/index lookups (NULL when query is not a lookup)
);

-- Query payloads (cold, for inspection only)
CREATE TABLE QueryPayloads (
    queryHash BLOB PRIMARY KEY,
    payload BLOB NOT NULL
);

-- Response nodes (Request/Response pairs)
CREATE TABLE Responses (
    nodeHash BLOB PRIMARY KEY,   -- trie identity: hash(afterHash, request, response)
    afterHash BLOB NOT NULL,     -- temporal predecessor (Query or Response nodeHash)
    request BLOB NOT NULL,
    response BLOB NOT NULL
);

-- Result nodes
CREATE TABLE Results (
    nodeHash BLOB PRIMARY KEY,   -- trie identity: hash(afterHash, payload)
    afterHash BLOB NOT NULL,     -- temporal predecessor (Response or Query nodeHash)
    payload BLOB NOT NULL
);

-- Shortcut index: semantic query hash → Query node
-- Also serves as the queryHash lookup (no separate index on Queries.queryHash needed)
-- Can hold additional metadata for candidate prioritization (future: Bloom filters, cost estimates)
CREATE TABLE Shortcuts (
    queryHash BLOB NOT NULL,     -- semantic hash for lookup
    nodeHash BLOB NOT NULL,      -- points to Queries.nodeHash
    createdAt INTEGER DEFAULT (unixepoch()),  -- proxy for relevance
    PRIMARY KEY (queryHash, nodeHash)
);

CREATE INDEX ShortcutsQueryHash ON Shortcuts(queryHash);

-- Forward traversal indexes (temporal trie)
CREATE INDEX QueriesAfter ON Queries(afterHash);
CREATE INDEX ResponsesAfter ON Responses(afterHash);
CREATE INDEX ResultsAfter ON Results(afterHash);

-- Structural lookup (for attr/index access on known Result)
CREATE INDEX QueriesStructural ON Queries(structuralParent, queryHash);
```

**Hot/cold split:** `Queries` is kept small for traversal; `QueryPayloads` holds payload data accessed only for inspection. `Responses` and `Results` keep their payloads inline since they're needed during validation and return.

**Hash computation:**
- Query `nodeHash = hash(afterHash, queryHash)` where `queryHash = hash(operation, params, inputHashes)`
- Response `nodeHash = hash(afterHash, request, response)`
- Result `nodeHash = hash(afterHash, payload)`

**Cascading Lookup Strategy:**

Three strategies tried in order of expected performance:

1. **Trie following (common case):** Start from root or last known position, traverse forward.
   - Expected when access pattern matches previous runs (e.g., repeated `nix develop`)
   - Fast: follow parent→child edges without validation backtracking
   ```sql
   -- From Result, find child Queries
   SELECT * FROM Queries WHERE afterHash = ?

   -- From Query, find child Responses
   SELECT * FROM Responses WHERE afterHash = ?

   -- From Response, find child Responses or Results
   SELECT * FROM Responses WHERE afterHash = ?
   SELECT * FROM Results WHERE afterHash = ?
   ```
   - Validate responses incrementally as we go

2. **Structural lookup:** Direct access on a known Result.
   - We have Result R (specific trie node), want `getAttr "bar"`
   - O(1) index lookup, no candidate iteration
   ```sql
   -- structuralParent = R.nodeHash
   -- queryHash = hash("getAttr", R.nodeHash, "bar")  -- Nix order: attrset.attr
   SELECT * FROM Queries
   WHERE structuralParent = :structuralParent
     AND queryHash = :queryHash
   ```
   - Then validate that subtrie's deps (similar to shortcut)

   Note: `structuralParent` is more general than "container"—and may be generalized to:
   - User-initiated function application with data arg (memoizable calls)
   - Further generalization: data arg can be lazy, with its evaluation modeled as Responses

3. **Shortcut lookup (fallback):** Jump directly to a Query node, trace back for validation.
   - Used when switching traces: current trace ends, or continues with a response mismatch
   - Also for different access patterns (e.g., `nix flake show` then `nix build`)
   - Less common, less critical to be fast
   ```sql
   SELECT nodeHash FROM Shortcuts WHERE queryHash = ?
   ```
   Then trace back via `afterHash` across tables to collect deps.

**Insert:**
```sql
INSERT OR IGNORE INTO Queries (...) VALUES (...)
INSERT OR IGNORE INTO Shortcuts (...) VALUES (...)
```

`OR IGNORE` makes inserts idempotent.

## Step 5: Implementation Architecture

This section describes the concrete implementation of the concepts above.

### TracingWriter: Unified Trace Output

`TracingWriter` is the central component that writes trace events to both storage formats:

```
TracingWriter
├── TraceFile (JSON log)     -- For debugging, validation, benchmarking
└── TracingIndex (SQLite)    -- For fast replay lookups
```

**Why both formats?**
- **TraceFile**: Sequential JSON log file. Human-readable, useful for:
  - Debugging and validating the cache implementation
  - Replaying to verify correctness
  - Reassembling the index from already-evaluated traces (e.g., after index format changes)
  - Potential stable "distribution format" if there's interest in sharing cached evaluations
- **TracingIndex**: SQLite database with the trie structure. Optimized for fast lookups during replay.

Both receive the same events, but structure them differently for their purposes.

### File Read Capture: TracingEnvironment and TracingSourceAccessor

File reads are captured via a wrapper around the filesystem:

```
TracingEnvironment
├── inner: Environment (real filesystem)
├── writer: TracingWriter &
└── tracingAccessor: TracingSourceAccessor
    ├── inner: SourceAccessor (from Environment)
    └── logFn: callback → TracingWriter.logResponse()
```

**Flow:**
1. `TracingEvaluator` uses `TracingEnvironment` as its environment
2. When Nix evaluates `import ./foo.nix`, it calls `fsRoot()->readFile(path)`
3. `TracingSourceAccessor::readFile()` delegates to `inner->readFile()`, then:
   - Computes `SHA256(contents)` as the response
   - Calls `logFn(Response{request: path, response: hash})`
4. `logFn` routes to `TracingWriter::logResponse()`, which writes to both TraceFile and TracingIndex

**Environment variables** follow the same pattern: `TracingEnvironment::getEnv()` wraps the inner call and logs the result.

### Recording Flow

During evaluation with tracing enabled:

```
User command (nix build)
    │
    ▼
InstallableFlake::toDerivation()
    │
    ├── Creates TracingWriter(traceFile, tracingIndex)
    │
    ├── Creates TracingEnvironment(sysEnv, writer)
    │       └── Creates TracingSourceAccessor
    │
    └── Creates TracingEvaluator(writer, innerEval, db)
            │
            ▼
        Evaluation proceeds...
            │
            ├── Query: eval "foo.bar"
            │   └── writer.logQuery(...)
            │
            ├── Response: file read /nix/store/.../default.nix
            │   └── writer.logResponse(...)  [via TracingSourceAccessor]
            │
            ├── Response: getEnv "HOME"
            │   └── writer.logResponse(...)  [via TracingEnvironment]
            │
            └── Result: { type: "derivation", path: "/nix/store/..." }
                └── writer.logResult(...)
```

### Replay Flow

During evaluation with replay enabled:

```
TracingReplayEvaluator
├── tracingIndex: TracingIndex &
├── position: TriePosition           -- Current position in trie
└── validatedNodes: set<NodeHash>    -- Nodes with validated deps
```

**Cascading lookup** (from tracing-replay-object.cc):

```
lookupResult(Q):
    # Strategy 1: Trie following
    for child in position.lookupQueryChildren(Q):
        if validateToValidatedNode(child.nodeHash):
            if result = findAndValidateResult(child):
                markValidated(result.nodeHash)
                return result

    # Strategy 2: Structural lookup (for getAttr/getIndex)
    if Q has structuralParent:
        for child in lookupStructuralChild(Q):
            if validateToValidatedNode(child.nodeHash):
                if result = findAndValidateResult(child):
                    markValidated(result.nodeHash)
                    return result

    # Strategy 3: Shortcut lookup
    for candidate in tracingIndex.lookupQueryBySemanticHash(Q.queryHash):
        if validateToValidatedNode(candidate.nodeHash):
            if result = findAndValidateResult(candidate):
                markValidated(result.nodeHash)
                return result

    return cache miss
```

### Validated Nodes Optimization

The `validatedNodes` set implements the optimization described in Step 2, point 7.

**Problem:** For queries at positions k₁ < k₂ in the same trace, validating deps [0, k₂) re-checks [0, k₁). Over n queries, this is O(n²).

**Solution:** Track validated node hashes. When validating deps for a query:

```
validateToValidatedNode(queryNodeHash):
    if queryNodeHash in validatedNodes:
        return true  # Already validated

    # Walk back through trie, collecting Response nodes
    responses = []
    current = queryNodeHash
    while current != NULL:
        if current in validatedNodes:
            break  # Stop at already-validated node
        node = lookupNode(current)
        if node is Response:
            responses.prepend(node)
        current = node.afterHash

    # Validate only the new responses
    for r in responses:
        actual = environment.query(r.request)
        if actual != r.response:
            return false

    validatedNodes.insert(queryNodeHash)
    return true
```

**Key insight:** Once we've validated a node, all its ancestors are implicitly validated. We only need to validate the path from the query back to the nearest validated ancestor.

**Result:** O(n) total validation work for n queries in a trace, regardless of query order.

### Merkle Identity and Provenance

Each node's hash includes its temporal predecessor (`afterHash`), creating a Merkle chain:

```
Query₁ ─────────────────────────────────────┐
  nodeHash = H(NULL, queryHash₁)            │
                                            │
Response₁ (file read) ──────────────────────┤
  nodeHash = H(Query₁.nodeHash, req, resp)  │
                                            │
Result₁ ────────────────────────────────────┤
  nodeHash = H(Response₁.nodeHash, payload) │
                                            ▼
Query₂ ─────────────────────────────────────┐
  nodeHash = H(Result₁.nodeHash, queryHash₂)│
  afterHash = Result₁.nodeHash              │
  from (in trace) = Result₁                 ▼
```

This means:
- Same query after different histories → different nodeHash
- Changing any Response invalidates all downstream nodes
- `afterHash` serves as both trie edge and provenance chain

## Step 6: Future Directions

### Forward-Steering Traversal

**Current limitation:** The Request is embedded in the Response node:
```
Response.nodeHash = hash(afterHash, request, response)
```

This means we cannot know *which requests to make* without first traversing the trie. But we cannot traverse the trie without knowing which branch to take, which requires knowing the responses. This circular dependency forces backward-looking validation: walk forward speculatively, then validate after the fact.

**Forward-steering optimization:** Split Request from Response:
```
Request node:  (requestHash, afterHash)
Response node: (responseHash, requestNodeHash, responseValue)
```

With this structure, Strategy 1 (trie following) could work purely forward:
1. At current position, look up child Request nodes
2. For each Request, query the environment to get actual response
3. Use index `(requestNodeHash, actualResponse) → responseNodeHash`
4. Navigate directly to the correct branch—no backtracking, no after-the-fact validation

**Design note:** Merging Request into Response was simpler initially but prevents this optimization. The current schema encodes "what happened" but not "what to ask next".

### Request Grouping and Parallelism

Beyond the Request/Response split, we could group independent Requests:
- Multiple file reads that could execute in parallel
- Hash the combined responses to navigate with a single lookup

**Complication:** When an early file changes, the *set* of subsequent requests may change. If `a.nix` imports `b.nix` in one version but `c.nix` in another, the request sets diverge.

This requires moving from pure immutable upserts to mutable graph structures:

1. **Radix tree approach:** Variable-length keys (request sequences). Pull common prefix requests forward. Reorganize as new branches are discovered.

2. **Decision tree approach:** Pull forward the *intersection* of requests (needed regardless of branch). Better concurrency for the common case, but requires tracking which requests are branch-independent.

Either approach introduces mutation—the structure evolves based on observed access patterns rather than being purely append-only.

### Value of the Intermediate Split

Given that grouping requires mutation anyway, the intermediate step (split Request/Response, but not grouped) may still be valuable:

- **Source of truth:** Raw append-only log of individual requests and responses
- **Validation:** Replay against the split data to verify optimized structures are correct
- **Incremental adoption:** Can implement forward-steering for Strategy 1 without committing to the full grouping design

The TraceFile (JSON log) already serves some of these purposes, but a structured Request/Response split in SQLite would enable efficient queries and could serve as the canonical format for index reconstruction.
