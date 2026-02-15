# Trace-based Caching Abstract Model

Given a purely functional call-by-need language extended with idempotent read operations, we can cache previous computation results by tracing and replaying the interactions between the evaluator and its environment.

User interactions:
- Query: a user request to evaluate, which may reference previous results
- Result: the result of such evaluation
  For the purpose of our model we include the query in the result.

Environment interactions:
- Request: a read operation on the environment
- Response: an idempotent result of such a read operation.
  For the purpose of our model we include the request in the response.

Design constraints:
- The call-by-need heap is treated as a black box, yet is highly consequential for all interactions.
  It prevents us from treating the subsequences of Query, Responses, Result as isolated memoizable entries.
  We can not extract complete "causal" relations between these interactions, but we can use ordering in time to infer which Responses *may* have affected the Results.
  For the correctness of our predictions, we must (a priori) assume *all* preceding Responses affect the Result.
  We could refine this temporal precedence relationship progressively into more of a causality relationship by "intersecting" with other interaction sequences produced with different heaps. I do not know whether this refinement is a productive line of inquery - start without it.
- We tend to keep the Query and Result small for fine grained caching, appropriate for a lazy language.

## I/O Automaton Formalization

By formalizing the cache, we gain a better understanding.
We make a reasonable effort to show why properties hold, but we don't (yet) reach the level of proof, let alone formal proof. <!-- We should be as thorough as we reasonably can. -->

We model the evaluator as a deterministic I/O automaton:

```
Evaluator = (S, s₀, In, Out, δ)

S     = set of states (opaque; includes the call-by-need heap)
s₀    = initial state
In    = Query ∪ Response       — input alphabet
Out   = Result ∪ Request       — output alphabet
δ     : S × In → S × Out*      — transition function (deterministic)
```

The evaluator receives inputs and produces zero or more outputs per transition:
- `Query` → may produce `Request*` then `Result`
- `Response` → may produce `Request*` (and eventually contribute to a `Result`)

### Traces

A **trace** is the observable I/O sequence:

```
τ = [(in₁, out₁*), (in₂, out₂*), ...]
```

### Determinism and State Recovery

**Claim:** For a deterministic automaton, the trace determines the state.

Given initial state `s₀` and input sequence, the output sequence and final state are uniquely determined.

**Corollary:** If two runs from `s₀` have identical I/O up to point P, their states at P are identical.

This is crucial—it means the trace is sufficient for caching. We don't need to serialize the heap.

### Caching via Trace Replay

**Recording:** Execute normally, recording the trace `τ`.

**Replay:** Given a new run, follow recorded trace:
1. For each `Query` in τ, check it matches the current query
2. For each `Response` in τ, check environment still gives same response
3. If all match, return the recorded `Result`
4. If mismatch, abandon replay and evaluate freshly

**Soundness:** By determinism, matching traces produce matching results.

### The Dependency Set

We write `R{Q}` for the Result of Query Q.

For a trace τ, define:

```
deps(τ) = { Response r : r appears in τ }
deps(τ, k) = { Response r : r appears in τ before position k }
```

For a Result `R{Q}` at position k, its **conservative dependency set** is `deps(τ, k)`—all Responses that *could* have influenced it.

We cannot shrink this set without looking inside the automaton. The heap is opaque; any prior Response might have forced a thunk representing a read operation, whose value now resides in the heap and may affect subsequent Results.

### Refinement by Intersection

Given multiple traces producing the same Result `R{Q}`:

```
τ₁ = [..., R{Q}]  with deps D₁
τ₂ = [..., R{Q}]  with deps D₂
```

`R{Q}` depends on at most `D₁ ∩ D₂`.

Any Response in `D₁ \ D₂` wasn't necessary for `R{Q}`, since τ₂ produced `R{Q}` without it (or with a different value).

This allows progressive refinement of dependency sets as we observe more traces.

## Query Identity

<!-- might want to minimize this, but keep the current level of detail, to see what we need later -->

We have not yet specified the structure of Queries. For the caching model, we need to identify "the same query" across traces.

### Structure of Queries

Queries may reference previous Results. For example, "get attribute `foo` from Result R₁" or "apply Result R₁ to Result R₂" (or in Nix: `R₁.foo`, `R₁ R₂`). Since the language is purely functional:

- Results are immutable once computed
- The order in which Queries are issued does not affect the values produced
- A Query's result depends only on the values of its inputs, not on when or how they were computed

### Identity Mechanisms

Three approaches to identifying Queries across traces:

1. **By value**: Include the full values of referenced Results
   - Sound: identical input values → identical Result
   - Impractical: values may be large or infinite

2. **By handle**: Use trace-local identifiers (e.g., "Result #7")
   - Problem: handles are not stable across traces
   - Handle 7 in trace τ₁ may not correspond to handle 7 in trace τ₂

3. **By provenance (Merkle hash)**: Hash the Query structure, including hashes of input Queries
   ```
   hash(Q) = hash(operation, params, hash(Q₁), hash(Q₂), ...)
   ```
   Where Q₁, Q₂, ... are the Queries whose Results are referenced by Q.

### What the Hash Identifies

The Merkle hash identifies a Query in context:
- The operation and its parameters
- The identity of each input (via their Query hashes)

It does **not** include:
- The actual values of the inputs (too large)
- The Responses that occurred during evaluation (those form the dependency set)
- The Result (that is what we are caching)

### Why Provenance Works

**Soundness**: If `hash(Q) = hash(Q')`, then Q and Q' have identical operation, parameters, and input hashes. By induction, identical input hashes imply identical input Queries. Given the same environment Responses, identical Queries produce identical Results.

**Order independence**: Since Results are immutable, environment reads are idempotent, and the language is pure, the order in which we evaluate Queries does not affect their Results. Evaluating Q₁ before Q₂, or Q₂ before Q₁, yields the same Results. The heap state differs only in which thunks have been forced, not in what values they hold.

**Conservativeness**: Merkle hashing is intensional—it identifies by provenance, not by extensional value. Two different Queries that happen to produce the same Result receive different hashes. This may cause cache misses, but never incorrect results.

## Open Questions

1. **Refinement practicality**: Is intersection-based refinement worth implementing? It requires tracking multiple traces and comparing their dependency sets. (Deferred to implementation.)

## Notes

**On determinism**: The real interpreter has internal non-determinism (pointer addresses, symbol IDs), but these are not observable—neither to the user nor to expressions. We handle this by defining S as either an idealized deterministic interpreter, or as equivalence classes of real interpreter states that are observationally indistinguishable. The model abstracts away the non-determinism rather than assuming it away.

**On dependency composition**: If Q₂ references `R{Q₁}`, then Q₂'s dependencies transitively include Q₁'s. This is already captured by the heap state—Q₁ must be evaluated before Q₂ can reference its Result, so Q₁'s Responses precede Q₂'s in the trace. The dependency union happens implicitly through trace ordering.

