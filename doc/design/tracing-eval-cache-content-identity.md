# Content-defined identity for `builtins.cache`

`builtins.cache` runs an inner evaluator whose interactions with the
surrounding (ambient) evaluator are content-traced: observations
crossing the inner-ambient boundary are recorded as facts and
participate in cache validation. This is the existing model, shipped
in Step G of
[`tracing-eval-cache-primop.md`](./tracing-eval-cache-primop.md).

This note refines an aspect of that model: the identity scheme used
to label values that cross the inner-ambient boundary. The current
implementation uses counter-derived identifiers (`seed:N`, `local:N`,
`virtual:N`) — positional labels assigned in observation order. This
design replaces them with content-defined identities rooted in
observation factsets, and reframes what the cache records about each
crossing so the recorded fact is free-standing.

It is a *design plan*, not an implementation guide. The implementation
section sketches a phased landing strategy, but the bulk of the doc is
the data model and its semantics. Read
[`tracing-eval-cache.md`](./tracing-eval-cache.md) (the v13 trie
itself) and the Step G section of the primop doc first.

## Motivation

The current identity scheme uses counter-derived strings: `seed:N`,
`local:N`, `virtual:N`, allocated in the order values are
encountered during recording. This ties identity to lazy forcing
order — and forcing order isn't stable across runs in any
non-trivial cached closure.

The concrete failure mode was observed during empirical testing of a
flake that caches the nixpkgs function and applies it with a
`rewriteURL` callback. Cold record and warm replay both return the
correct value, but the walk falls through to inner re-evaluation
because the same Request hash dispatches to different live
responses between record and replay. Tracing the responses back
through the producer chain showed nixpkgs's lazy forcing order
shifting which actual attrset element each `seed:N` label was
tagging. Details of the investigation are in
[`tracing-eval-cache-primop.md`](./tracing-eval-cache-primop.md),
§Open Questions item 1 ("Counter id collisions"), under "Observed
in practice (nixpkgs)."

Two failure modes follow from counter-based identity:

- **Spurious walk fall-throughs.** The cache returns the correct
  value (the fall-through path runs inner re-evaluation) but
  performance degrades to baseline-or-worse since cache overhead is
  paid without cache benefit. This is what manifested in the
  nixpkgs investigation, and it's the common case under counter
  collisions.
- **Stale hits.** If two values with different content collide on
  the same id *and* their observation responses happen to match —
  e.g. both `ResultType{"function"}` for two distinct outer
  functions — the walk hits stale and returns the wrong answer.
  No confirmed instance to date, but the current design has no
  structural defence against it.

The underlying problem is that identity is tied to observation
order rather than observation content. Partial fixes (namespacing
counter strings, tighter validity checks at dispatch) close some
collision domains but leave the order-dependence intact. The design
that follows replaces counter-derived identity with content-derived
identity rooted in factsets: two values with the same observed
shape collapse to the same identity by construction, regardless of
when or in what order the observations happened, and, by
referential transparency, an apply's recorded facts depend only on
observations through its own fn and arg — so unrelated callbacks
cannot pollute one another's factset preconditions.

## Principles

- Numbered identifiers only at the CLI level, and to identify generalized curry depth: reverse De Bruijn. *Only the CLI gets a counter*; everything else is grounded in the real structure of the expressions.
- Combine structural identification with intrinsic hashes to identify relevant states
- Make sure replay gets all the information to reproduce the facts and intrinsic hashes, so that more-specific facts can be acquired from less-specific states
- Maintain breadcrumbs relations between more-specific and less-specific hashes, so that less-specific facts can be retrieved at a later time
- Make sure all queries are specific enough. Ambiguities (>1 row) need to be resolved, ideally by query specificity, otherwise by building a custom index (e.g. Asks, decision trees, ...), only temporarily by iteration.

### Why counters are allowed at the CLI

The CLI is the only carve-out for numbered identifiers. It's not that
the counter is fundamentally good there — it's that the CLI creates
seed-introducing constructs in predictable patterns (a small set of
top-level entry points enumerated in a fixed order), and an
enumeration is sufficient. We're not investing in a fancier identity
scheme at the CLI because there's no real benefit. Everything below
the CLI — including the `builtins.cache` primop path — bridges into
content-defined identity at the cache boundary, where the patterns
aren't predictable and a counter would create cross-call aliasing.

## Core principle: identity is observation-derived


We never inspect a value to summarise its content directly. Two
things rule that out:

- **Laziness.** At the moment a value crosses the inner-ambient
  boundary, much of it is a thunk. Inspecting its content would
  force the thunk, which is exactly what we're trying to trace
  rather than perform.
- **Opacity.** A function value's "content" is its closure, which
  has no canonical structural representation we could hash. Nix has
  no `hash(value)` operation that is both meaningful and cheap.
  This idea might constitute an interesting direction for research,
  but was considered too invasive by the project plan.

Identity instead emerges from the tracing itself. Each value is
identified by the hash of the factset of observations made on it.
The distinction from "direct content hash" is load-bearing:

- A **direct content hash** would be `hash(value-as-bytes-or-AST)`
  — inspecting the value to summarise it.
- A **content-defined identity** is `hash(factset-of-observations)`
  — the value's identity is built up incrementally from facts as
  observations accumulate. This is paradoxical: normally an identity
  is stable, but in this environment no stable identity exists, and
  an evolving identity is the next best thing.

These are different in kind. Direct content hashes are unavailable
to us (laziness, opacity); content-defined identities are what the
existing tracing already produces, and we just need to use them as
identifiers rather than tagging values with positional counters.

The scheme is symmetric across the boundary:

- An **outer value reaching into the inner** — today represented by
  `AmbientObject` — has its identity built from the inner's
  observations on it.
- An **inner value reaching into the outer** through a callback —
  the moral opposite, today represented by `TracingLocalObject` at
  recording time and `ReplayLocalObject` at replay — has its
  identity built from the outer's observations on it.

Same machinery, opposite direction. Whichever side is observing
builds the identity of what is being observed.

Two consequences fall out:

- **Stability across runs.** Two recordings that make the same
  observations produce the same identity by construction. No
  counter to disagree, no positional choice to drift.
- **Same-shape collapse.** Two values with identical observed shape
  get identical content-defined identities. This is correct: by
  extensionality, values indistinguishable by observation are
  interchangeable, and the cache treats them as such. Two unrelated
  callbacks that happen to receive the same arg shape share a
  cache entry; this is a feature, not a collision.

## Two scope notions

Two structural mechanisms shape what the cache records, and they
must be separated cleanly to avoid confusing path navigation with
frame structure.

### Cached-value curried apply stack

This is *not* the Nix call stack. It is a structural property of
the cached value itself: when the cached value is a curried
function — e.g. `{ x }: y: x + y` — each successive outer
application opens a new stack frame. Stack=0 holds the first
apply's args; stack=N holds the N-th curried application's args.
Frames at higher stack levels can reference bindings at lower
levels (the curried function closed over them).

For a cached value that isn't a function (e.g. an attrset literal)
or a non-curried function (e.g. `{ x }: x + 1`), the stack stays
at depth 0. The cached value is reached by a single apply (or no
apply at all, for an attrset), and everything else happens inside
that one frame.

**Intermediate attrset and list traversals do not push stack
frames.** `(cached).foo.bar` reaches a deep attr through two
navigation steps but stays at stack=0 throughout. Similarly for
list item retrievals. The stack only deepens on function
application.

This produces an observable asymmetry between behaviourally-
equivalent representations:

```nix
# Form A — attrset of functions, one stack frame
lib = { square = x: x * x; double = x: 2 * x; };
# usage: lib.square 5

# Form B — function returning function, two stack frames
lib = sym:
  if sym == "square" then x: x * x
  else if sym == "double" then x: 2 * x
  else throw "function not found";
# usage: lib "square" 5
```

Both produce 25 for the same usage, but Form B introduces an
extra closure scope: when the inner constructs `x: x * x` inside
the `if`-branch, that lambda closes over `{ sym = "square" }`.
Form A's `x: x * x` closes over only the outer lexical scope,
which is empty of apply-introduced bindings.

Attribute selection does not require an otherwise identifier-less
value to be given a synthetic identifier — the resulting value's
identifier is already implicit in the path (e.g. `(parent).foo`
identifies it structurally). Function application, by contrast,
produces a value whose only identifier is the content-defined
identity we synthesize from the arg's observations. The synthesis
step is what opens a new frame; navigation that doesn't synthesize
doesn't.

### Within-frame Query path navigation

Inside any one stack frame, the inner does work that extends a
Query path expression rooted at the cached value. Three
navigation operations:

- **Attribute selection** — `.attr`, when the path so far yields
  an attrset.
- **List item retrieval** — `[<H>]`, when the path so far yields
  a list. The index is content-addressed, the same as apply args.
- **Callback application** — `<H>`, when the path so far yields a
  function and the inner applies it, with `<H>` the
  content-defined identity of the inner-supplied arg.

These extend the Query tree but do *not* push new stack frames. A
callback like `f 10` inside the cached body adds an apply node to
the Query path and records facts about the apply's outcome in the
current frame's factset. It does not open stack=1.

This is the load-bearing distinction. Stack frames are about
curried applies of the cached value itself. Path navigation is
about everything else: attribute selection, list item retrieval,
and callback applies on functions reached through navigation. All
the within-body callback structure of a typical cached expression
sits inside one frame as a path tree.

### Interaction: state creep up the linked list

**State creep** is the phenomenon that in our black-box interpreter
model, any input we have observed must be assumed to affect every
subsequent result. We cannot prove which subset of past
observations a given computation actually depended on, so we
conservatively treat all of them as preconditions.

The mechanism: when a higher stack frame references a binding from
a lower one — the curried `y: x + y` case — observations on the
lower binding are attributed to its owning frame, not to whichever
frame happens to be syntactically executing. At log-write time,
the lower frame's factset feeds into the content-hashes computed
for any facts emitted in the higher frame's scope; this is state
creep manifesting in the recorded log, as intended.

For sibling stack frames — e.g. two independent cache invocations,
each with its own stack=0 — we still cannot drop arbitrary history,
but we can exploit referential transparency. Without it we would
have to generate unique identifiers for each call to disambiguate
them; with it, we address calls by their argument content, as
developed in the addressing section and the Request/Response section.

State creep is overapproximation — a broadening of the cache
precondition. We have to accept this, because the black-box
interpreter model gives us no way to prove which past observations
actually matter. It widens the cache key beyond strict minimum but
never invalidates incorrectly.

## Positional and content-defined addressing

Values bound at frame depths in the cached-value curried-apply
stack (the Two scope notions section) — the cache call's argument at depth 0, the argument
of the N-th curried apply at depth N — are addressed in two
distinct ways at two different stages of the recording/replay
cycle. The two addressing schemes are connected by a translation
that happens at fact emission. Callback-local values that don't
occupy frame positions have a different addressing scheme; see
the final subsection.

For frame bindings, the scheme is symmetric across the two
directions of crossing: whichever side is observing the binding,
it uses the same scheme to refer to it. Outer-side bindings
reached via `AmbientObject` (when observed by the inner) and
inner-side bindings reached via `TracingLocalObject` /
`ReplayLocalObject` (when observed by the outer during a curried
apply of a function the cache returned) both follow the rules
below.

### Recording-time: reverse De Bruijn

During recording, frame bindings are addressed by their position
in the active frame stack. We use reverse De Bruijn indexing:
`ambient-0` is the outermost binding (the cache call's argument),
`ambient-1` is one level deeper, `ambient-N` is N levels deep. The
index counts from the outer expression inward, matching the
structural depth of the binding.

These positional handles are cheap to maintain — the active proxy
carries an observation-state stack whose linked list *is* the
addressing structure, and looking up `ambient-N` is N hops up the
list. They are stable within a single recording session: the same
lambda binding always gets the same depth.

What they are *not* stable across is different recording sessions,
or even between call evaluations within the same session.
Positional handles depend on the call structure being executed at
that moment; they cannot serve as cache keys.

### Recorded fact: content-defined factset hash

In a recorded fact, frame bindings are identified by the hash of
their factset at the moment of recording. This identity is
content-defined (the Core principle section) and mostly stable across sessions: same
observations → same hash, regardless of run identity. It is not
stable when evaluation order differences and state creep combine
(see the Open issues section).

The same logical value appearing in different facts at different
observational states carries different hashes. This is
non-destructive: the placeholder is not rewritten in-place; each
snapshot produces its own content-id, and facts at different
snapshots use different content-ids.

### Translation: at fact emission

The translation from positional handle to content-defined hash
happens when a fact is emitted to the log. The active proxy
resolves `ambient-N` to the current factset hash of the binding at
depth N — including any inherited state-creep contributions from
frames above N — and the emitted fact carries the resolved hash.

After fact emission, more observations may land on the same
`ambient-N`, growing its factset. Subsequent facts emitted later
refer to the same `ambient-N` via the positional handle, but the
resolution produces a different content-hash. The recorded log
thus contains facts that name the same logical value by different
content-ids across its observational history.

This gives the recorded fact the free-standing property without
burdening the recording-time representation. Recording stays simple
— positional handles, linked list of mutable factsets. The recorded
fact stays stable — content-defined, free-standing. Translation is
local to the fact-emission step.

### Callback-local values

Inner-supplied values passed to outer functions via callbacks
(the Two scope notions section's "callback application") do not occupy positions in the frame
stack. They are addressed directly by their content-defined hash
from the start — there is no positional-handle phase.

The recording-time mechanism is buffer-then-hash: observations on
the callback local are buffered, and the content-hash is computed
from the buffered observations at the moment a fact referencing
the local is emitted. The recording-time representation is
effectively a pending content-hash being built up as observations
land, rather than a positional label that later gets translated.

The "positional handle vs content-defined hash" distinction of the
preceding subsections therefore doesn't apply to callback locals
the same way. What does carry over is the non-destructive
behaviour: as observations accumulate, the content-hash advances,
and facts emitted at different points reference the local under
different content-hashes — exactly as the first-order callback example's worked example
illustrates.

## Request/Response shape

The cache's existing Request/Response envelope (in v13) has shapes
like `ReadFile` and `GetEnv` as variants. This design adds an
ambient variant alongside, and reshapes what `Query` means in the
context of an ambient interaction.

### The types

```
data Request
  = ReadFile      { absPath : String }
  | GetEnv        { name    : String }
  | AmbientQuery  { query   : Query }

data Response
  = FileReadResponse { contentHash : Hash }
  | GetEnvResponse   { value       : Maybe String }
  | AmbientResponse  { result      : Result }
```

The new variants are `AmbientQuery` and `AmbientResponse`.

### Query: the navigation chain with typed leaves

`Query` is the existing recursive Query type, lifted to a typed-leaf
representation. Today's `from` field is a hex string (a hash);
under this design it becomes a typed sum:

```
data QueryLeaf
  = Ambient { index : Int }    -- positional reference during recording
  | Content { hash  : Hash }   -- content-defined factset hash in recorded facts
```

A Query expression is a tree of operations whose leaves are
`QueryLeaf` values:

- **Atomic observations** at the top: `QueryGetType`, `QueryGetInt`,
  `QueryGetString`, `QueryGetAttrNames`, `QueryGetBool`,
  `QueryGetFloat`, `QueryGetListSize`, `QueryGetPath`,
  `QueryMaybeGetAttr`, `QueryGetFunctionInfo`.
- **Path constructors** in the middle: `QueryAttr name`,
  `QueryListElem index`, `QueryApply arg`.
- **Leaves** at the bottom: `Ambient { index }` or
  `Content { hash }`.

Example, in a freeform notation:

```
QueryGetType {
  from = QueryApply {
    arg  = Content { hash = <H_for_10> },
    from = QueryAttr {
      name = "f",
      from = Ambient { index = 0 }
    }
  }
}
```

This Query says: "get the type of the value at
`(ambient-0).f applied with content-id H_for_10`." Recording-time
form, with the `Ambient` leaf. The recorded form has all `Ambient`
leaves translated to `Content` per the addressing section.

### Result: the answer payload

`Result` is a variant union of the atomic answer payloads,
mirroring the atomic observation types:

```
data Result
  = ResultType          { type    : String }
  | ResultInt           { value   : NixInt }
  | ResultString        { value   : String, context : [String] }
  | ResultMaybeType     { type    : Maybe String }
  | ResultListOfStrings { values  : [String] }
  | ResultBool          { value   : Bool }
  | ResultFloat         { value   : NixFloat }
  | ResultListSize      { size    : Int }
  | ResultPath          { path    : String }
  | ResultFunctionInfo  { hasInfo : Bool,
                          formals : Map String Bool,
                          ellipsis: Bool }
```

Each atomic observation has exactly one `Result` variant. The
Response wraps it: `AmbientResponse { result : Result }`.

The Response does *not* carry the proxy's accumulated state. The
cumulative factset hash that the walker maintains as it advances
through Asks edges is the walker's responsibility, computed by
XOR-folding `(Hash(Request), Hash(Response))` pairs. There is no
separate "factset hash" field in the Response.

### `QueryApply` is a path constructor, not a query

Worth being explicit because it is a temptation to model otherwise:
applies appear in Query paths as a `QueryApply` constructor with an
`arg` field, but applies do not produce their own
`AmbientResponse`. The result of an apply is observed by
*subsequent* atomic queries on the apply expression — e.g.
`QueryGetType` whose `from` is the `QueryApply`.

So `(root.f) <H_arg>` doesn't have its own Response; observations
of the apply's outcome — getType, getInt, getFunctionInfo, etc. —
each have their own Request/Response pair.

This keeps the model uniform: every Request maps to exactly one
observation, and that observation has exactly one Response shape.
The path tree gives us structure; the atomic top gives us the
question.

### Fact emission

A Fact is `(Hash(Request), Hash(Response))`, XOR-folded into the
cumulative factset hash. This is unchanged from v13. The new
payload shapes affect what gets hashed, but the trie structure
(Asks edges, Terminals, RequestSet pool) is untouched.

## Walker semantics

The walker is the replay-side component that dispatches recorded
Requests against the live evaluator state and folds the responses
into a cumulative factset hash. Existing v13 walker logic handles
`ReadFile` and `GetEnv` Requests directly; ambient Requests need
new dispatch logic.

### Given a recorded `AmbientQuery`

The recorded form of an `AmbientQuery` has a Query path whose
leaves are `Content { hash = X }` — content-defined factset hashes
(per the addressing section). The walker:

1. Parses the Query's path expression top-down.
2. Walks from the path's root, applying each path constructor
   in turn. The root is the live outer evaluator's value at the
   cache call's argument position (for paths rooted at an
   `ambient-N` binding, after content-hash resolution) or a
   stand-in materialised from the Responses pool (for paths rooted
   at an inner-supplied callback local that is no longer live):
   - `QueryAttr name` → call `maybeGetAttr(name)` on the current
     Object. The result becomes the next Object on the path.
   - `QueryListElem index` → call `getListElem` with the
     materialised index.
   - `QueryApply arg` → materialise a stand-in for `arg` from its
     `Content` hash, apply the current Object to it.
3. Fires the atomic query at the final position against the
   current Object.
4. Hashes the resulting `Result`, yielding the live `ResponseHash`.
5. Compares against the recorded `ResponseHash`. Match → fold into
   cumulative factset, walk advances. Mismatch → walk falls
   through, cache miss.

### Materialising `Content` leaves

A `Content { hash = X }` leaf names a value by its observational
history — the set of `(Request, Response)` pairs recorded about it.
To materialise the value for live dispatch, the walker uses the
Step G machinery:

- The Responses pool maps `RequestHash → Response payload` for
  every observation made during recording.
- A stand-in object (today's `ReplayLocalObject` for inner-supplied
  values, and a symmetric thing for outer-side values) is a proxy
  whose Object methods read recorded responses out of the Responses
  pool, keyed by the same Request hashes the recorder used. It has
  no live evaluator underneath; every answer it returns is one the
  recorder stored.

Under content-defined identity, the stand-in is keyed by the
`Content` leaf's hash. The walker constructs the stand-in, then
applies the live operation (apply, getAttr, etc.) using it. The
live operation observes the stand-in via its standard Object
interface; the stand-in serves recorded responses; the walker then
queries the live result.

This is symmetric for both directions:

- For an inner-supplied callback arg (the local-id case), the
  stand-in serves what the outer recorded observing the inner's
  value during the recording's callback evaluation.
- For an outer-supplied value reached through an ambient query (the
  ambient-id case), the stand-in serves what the inner recorded
  observing the outer's value.

### Why this works

The walker doesn't need to know what `ambient-N` was at recording
time. The recorded leaves are content-hashes, not positional
handles. To validate a recorded fact, the walker just needs to
reproduce the same response hash from the live system — and the
response hash is determined by:

1. The live cached value's structure (unchanged across record/replay
   for valid hits).
2. The recorded stand-ins serving the same observed values as
   during recording.

If both hold, every dispatched `(Request, Response)` pair matches
recorded → the XOR-fold reproduces the recorded cumulative factset
hash → the Asks edge walks to the same Terminal → walk hits.

If the live cached value differs (e.g. different `cached.nix`), the
navigation fails or produces different observations early. If the
live outer fn's behaviour has changed in a way that produces
different responses from the recorded callback evaluation (i.e.
applying the live fn to a stand-in yields an apply-result with
observations that don't match the recorded ones), the dispatch
falls through — this is the Step G invalidation discipline carried
over.

### Same Asks-edge machinery as v13

The trie structure — Asks edges from the cached Q advancing through
RequestSets, with the walker dispatching each Request and folding
responses into the cumulative factset hash — is unchanged. The new
payload shapes (`AmbientQuery`, `AmbientResponse`) just expand what
the dispatcher can handle. The trie-walk algorithm itself does not
change.

## Argument scope: it rides on the proxy graph

The structure required to translate positional handles (the
addressing section) into content-defined hashes is the same
structure the runtime already has: the graph of function proxies and
ambient/local Object wrappers at the cache boundary. There is no
separate frame stack on the writer, no `pushFrame`/`popFrame`, no
evaluator-global "currently active scope" pointer. The proxies form
an immutable linked tree by their construction, and that tree *is*
the scope structure.

### What an argument scope holds

Each proxy at the cache boundary carries a `parent` pointer
(`shared_ptr` — no cycle exists in the opposite direction, and a
child without its parent is semantically invalid). Apply-result
proxies additionally carry an `argScope` cell: the AmbientId and
live Object for the argument of *this* apply. Navigation children
(reached via `maybeGetAttr` / `getListElem`) don't add a new cell;
they hold a reference to the producer query's discriminator (attr
name, list index) so the chain id can be recomputed by walking back
via `parent`.

We use `argScope` rather than `scope` because "scope" is overloaded
— the interpreter has its own lexical scope. These scopes are
limited to cache-boundary arguments.

### How argument scope propagates

The propagation rules mirror the existing returned-structure
walk:

- `builtins.cache x` returns the cached value at the root, with
  no parent.
- An apply on a proxy — *any* proxy that resolves to a function,
  reached however it was — produces a new proxy whose `parent` is
  the fn proxy and whose `argScope` cell binds the new argument's
  identity (its content-defined hash at apply time, which is the
  empty-set hash for a freshly-introduced argument).
- An attribute selection or list-element retrieval on a proxy
  produces a child proxy with the same `parent`/`argScope` view as
  its parent. No new cell.

A new `argScope` cell opens at any function-application site that
crosses the cache boundary — not just at syntactic-curry positions
(`f: x: ...`). A function reached via attribute selection
(`(c args).f arg2`), via a list element
(`(builtins.elemAt (c args) 0) arg2`), via the return value of any
of these (`(c args).f arg2 arg3` curries through `f`'s return), or
via any nested combination — applies the same way and opens a new
cell.

Within-frame callbacks — `f 10` inside the cached body where `f` is
a function reached through path navigation — are *not* `argScope`
introductions; they extend the Query tree of the current scope's
observations, with the callback arg appearing as a content-hash
leaf in the path expression.

### Where observations are attributed

A query whose path roots at `ambient-N` contributes its observation
to the scope cell at depth N, walked-to via `parent` from whichever
proxy is being observed. This is independent of which depth is
syntactically executing at the moment of the observation.

Example, in the curried `y: x + y` case: when the depth-1 body
forces `x` (which is the binding at depth 0), the resulting
observations on `x` are recorded with paths rooted at `ambient-0`,
and they accumulate at depth 0's scope cell — reached via
`parent.parent` from the depth-1 proxy. Depth 1's cell only
collects observations whose path roots at `ambient-1`
(i.e. observations on `y`).

An observation's home is determined by where it roots in the path,
not by who is running when it happens.

### State creep at fact emission

When a fact is emitted to the log, the recorder computes the
content-defined hash for each `ambient-N` referenced in the fact's
path. It walks `parent` from the current proxy to depth N,
XOR-folding each ancestor cell's accumulated factset into the
final hash, and substitutes the content-hash into the path before
emission.

The walked-and-folded sum is the content-defined identity for the
`ambient-N` at this moment, as discussed in the addressing section.
It widens the cache key beyond strict minimum (the Two scope
notions section state creep) but never invalidates incorrectly.

### Sibling isolation and the argument-scope tracking rule

Observations are tracked *at* the argument scope cell of the
binding they're observed against — nowhere else in the proxy graph.
This single location rule is what gives the recorded facts two
complementary properties:

- **State creep**: when a fact is emitted, the recorder XOR-folds
  the entire ancestor chain's accumulated observations into the
  fact's content-defined hash for each `ambient-N` referenced.
  Everything an ancestor cell has seen contributes; no ancestor
  observation is ever omitted. This is the same widening described
  in the Two scope notions section, expressed through the proxy
  graph's `parent` walk.

- **Referential transparency**: siblings — distinct branches that
  share an ancestor in the proxy graph — see only their own and
  their ancestors' observations, never each other's. A sibling's
  observations are not in the current node's ancestor chain, so
  they don't fold in. This is structural, not bookkeeping: there's
  no path from a node's emission walk to a sibling's scope cell.

The tracking rule (observations at the argument, not elsewhere) is
what allows both properties to hold simultaneously. If observations
were stored higher up — at a shared root, say — siblings would see
each other's history; if stored lower, ancestors couldn't contribute
to state creep without lateral lookups.

A direct consequence is that two sibling `builtins.cache`
invocations — e.g. `(builtins.cache x) args1 + (builtins.cache x) args2`,
or `let c = builtins.cache x; in c args1 + c args2` where `c` is the
primop itself — are isolated from one another naturally. They share no
ancestor in the proxy graph; each apply opens its own fresh root, so
there's no shared scope cell to leak through and their recordings stay
disjoint. Same property as any other sibling pair, applied at the top.

The shared-`c` example is not `c = (builtins.cache x) y`; that form
binds `c` to a partial application result, and `c args1` + `c args2`
are curried applies that *do* share a depth-0 cell holding `y` — that
sharing is exactly what propagates `y`'s observations into both
deeper recordings via state creep.

## Boundary-trace-only discipline

The wrappers that mediate observations at the cache boundary —
`AmbientObject` for outer values reached by the inner, and the
recording-side `TracingLocalObject` for inner values reached by
the outer through a callback — *trace once*. They are not
reentrant proxies that you wrap once and reuse: each observation
made through a wrapper is recorded against that wrapper's identity,
and the wrapper's identity advances as observations land. Two
forces of the same value at different observational moments are
*not* the same observation, and reusing one wrapper across both
would silently merge them.

The consequence is a strict construction-and-use locality rule.
Every `AmbientObject` / `TracingLocalObject` must be constructed
right next to the cache-boundary call that needs it, and used
within that call. They must *not* be cached in any map that would
let a later boundary crossing find an earlier wrapper. Specifically:

- No `Value *` → `Object *` map that returns a wrapper for the
  same `Value` across distinct boundary crossings.
- An id → Object map keyed by a wrapper's id (e.g. an ambient
  resolver's `outerValues` / `localValues`) leans toward being the
  same hazard: it returns the same wrapper instance to later
  lookups that resolve to the same id. Use with care; prefer
  constructing the wrapper at each boundary crossing.

`ExprFromObject` and `ExprFromObjectAttr` are *not* this hazard.
They are part of a Value's thunk machinery — released when the
thunk transitions from suspended to WHNF — and they're how state
creep through factsets and parent links is modelled at the
boundary. They're necessary machinery, not a place where
observations get obscured.

## Worked examples

Four examples that together cover the design surface, plus a fifth
targeted at the original failure case.

### First-order callback

```nix
# cached.nix
{ f }: f 10
```

Outer: `(builtins.cache { import = ./cached.nix; }) { f = x: x + 1; }`.

**Frames**: One. The cache call's argument is bound at depth 0; the
cached body returns Int 11 (not a function), so no further frames.

**Facts recorded** (with leaves already resolved to content-hashes):

```
getType(ambient-0)                                        → set
getAttrNames(ambient-0)                                   → ["f"]
maybeGetAttr("f", ambient-0)                              → function
getType(QueryAttr("f", ambient-0))                        → function
getType(QueryApply(QueryAttr("f", ambient-0), <H_10>))    → int
getInt(QueryApply(QueryAttr("f", ambient-0), <H_10>))     → 11
```

`<H_10>` is the content-defined identity of the inner literal
`10`, computed from the observations the outer's `x + 1` body
makes on it: `(getType, int)` and `(getInt, 10)`.

Those outer-side observations are themselves recorded facts. Their
paths use the local's content-hash at the moment of recording, so
each observation references the local under a different evolving
hash:

```
getType(<H_local_empty>)        → int   -- recorded at first force
getInt (<H_local_after_getType>) → 10   -- recorded at second force
```

where `<H_local_empty>` is the content-hash of an empty factset and
`<H_local_after_getType>` incorporates the first observation. After
both, the local's content-hash stabilises to `<H_10>`, which is the
leaf used by the inner-side queries on the apply result. The
non-destructive substitution behaviour (the addressing section) is visible here: the
same logical local appears in three different facts under three
distinct content-hashes representing three points in its
observation history.

These observations also land in the Responses pool keyed by the
same content-hashes, so a replay-side stand-in for `<H_10>` can
serve them.

**Replay**: The walker dispatches each Request against the live
outer args. For the apply Request, it materialises a stand-in for
`<H_10>` from the Responses pool, applies the live `f` to it,
queries the live result. Identical outer args → identical
responses → walk hits.

This case works cleanly because the inner literal `10`'s
observation set is small and stable: the outer's `x + 1` body
forces it via `getType` then `getInt`, and after those two
observations its content-defined identity is fully determined.
No order-dependency issue can arise.

### Sibling callbacks

```nix
# cached.nix
{ f }: { a = f 10; b = f 20; c = f 30; }
```

Outer: `(builtins.cache { import = ./cached.nix; }) { f = x: x + 1; }`.

**Frames**: One. Cached body returns an attrset.

**Facts**: Same shape as the first-order callback example for the formals matching, plus three
sets of inner-side apply observations differing only at the
rightmost leaf (each set has both `getType` and `getInt`; only
`getInt` shown for brevity):

```
getInt(QueryApply(QueryAttr("f", ambient-0), <H_10>))  → 11
getInt(QueryApply(QueryAttr("f", ambient-0), <H_20>))  → 21
getInt(QueryApply(QueryAttr("f", ambient-0), <H_30>))  → 31
```

The three apply paths share a common prefix `QueryAttr("f",
ambient-0)` and diverge at the content-addressed arg leaf. The
hashes are distinct because the underlying values differ in
content.

Each of the three locals (`10`, `20`, `30`) has its own outer-side
observation set, recorded just as the first-order callback example details for its single
local. The three sets of facts are entirely independent — `<H_10>`,
`<H_20>`, `<H_30>` are computed from disjoint observation
histories, and none of them is referenced from any other's facts.
The three Responses-pool sub-pools are likewise disjoint.

**Replay**: Three independent stand-ins materialised; live `f`
applied to each. Dispatches are independent; the XOR-fold is
commutative, so dispatch order doesn't affect the cumulative hash.

This is the "same middle and bottom, varying top" pattern that the
rewriteURL workload reduces to: a stable common-prefix path
(`QueryAttr("f", ambient-0)`) plus distinct rightmost leaves, one
per callback invocation.

### Curried cached value

```nix
# cached.nix
{ x }: y: x + y
```

Outer: `(c { x = 10; }) 20`.

**Frames**: Two. Stack=0 holds `{ x = 10; }`; stack=1 holds `20`
with `parent = stack=0`.

**Facts**: The first apply records observations rooted at
`ambient-0`. The second apply pushes stack=1 and records
observations rooted at `ambient-1`. Concretely, during the body's
`x + y` evaluation:

- Forcing `x` lands observations in depth-0's factset: `getType(QueryAttr("x", ambient-0))` → int, `getInt(QueryAttr("x", ambient-0))` → 10.
- Forcing `y` lands observations in depth-1's factset: `getType(ambient-1)` → int, `getInt(ambient-1)` → 20.
- The sum produces 30 as the apply's terminal result.

**State creep at fact emission**: when a fact whose path references
`ambient-1` is emitted, the recorder computes the content-defined
hash for `ambient-1` by XOR-folding depth-1's factset with
depth-0's (walking up the linked list). The recorded content-hash
thus incorporates depth-0's observations even when only depth-1's
binding was directly involved. This wider key is overapproximation
(the Two scope notions section) — never gives a false hit, but may prevent a hit if depth-0's
factset differs across record/replay.

**Replay**: The walker handles each apply event in order. The first
apply's dispatches populate depth-0's observations live; the second
apply's dispatches populate depth-1's. Same outer args at each
level → same responses → walk hits.

### Higher-order callback

```nix
# cached.nix
{ f }: f (x: x + 1)
```

Outer: `(builtins.cache { import = ./cached.nix; }) { f = g: g 5; }`.

The inner constructs an inner-defined lambda `x: x + 1` and passes
it as the arg to `f`. The outer's `f` then applies that lambda to
5.

**Frames**: One. The cached body returns the result of
`f innerLambda`, which is Int 6 — no curried push.

**Facts**: Two boundary crossings in opposite directions, both
producing content-defined leaves:

- The inner-supplied lambda `x: x + 1` is observed by the outer's
  `g: g 5` body. The outer queries its type, applies it to 5, and
  queries the apply result. All of those observations land in the
  lambda's factset and contribute to its content-hash.
- The outer-supplied literal `5` (in the outer's `g 5`) is observed
  by the inner lambda's `x + 1` body when it forces `x`. Its
  content-hash builds from inner-side observations:
  `(getType, int)` and `(getInt, 5)`. Same form as `<H_10>` in
  the first-order callback example, except the observing side is now the inner.

The lambda's content-hash evolves as observations on it accumulate;
each fact uses whichever snapshot was current at emission. A
representative ordering, with `<H_lambda_0>` denoting the
empty-factset hash and `<H_lambda_1>` the hash after `getType` lands:

```
-- Outer-side observations on the lambda and on (lambda 5)'s result:
getType(<H_lambda_0>)                                          → function
getType(QueryApply(<H_lambda_1>, <H_5>))                       → int
getInt (QueryApply(<H_lambda_1>, <H_5>))                       → 6

-- Inner-side observations on f(lambda)'s overall result:
getType(QueryApply(QueryAttr("f", ambient-0), <H_lambda_1>))   → int
getInt (QueryApply(QueryAttr("f", ambient-0), <H_lambda_1>))   → 6
```

(Whether apply-result facts also advance the lambda's hash is a
design choice we have not yet committed to; both readings keep the
cache correct, with different distributions of discrimination
between identity hashes and Response hashes. The Open issues section's deferred
list flags this.)

The non-destructive substitution property of the addressing section is visible: the
lambda appears under at least two distinct content-hashes
(empty, post-getType) across the fact log.

Depth does not grow with higher-orderness here because the cached
value itself isn't curried. There's one outer-driven apply (the
cache call). All the inner→outer→inner crossings happen as path
navigation within stack=0's factset.

**Replay**: Same machinery as the first-order callback example. The walker dispatches each
Request. For the lambda's content-hash leaves, it materialises a
stand-in from the Responses pool. The outer's `f` body forces the
stand-in to a function value (served from the pool), then applies
it. The application produces a result that the walker queries; the
result's observations also dispatch against recorded facts.

### rewriteURL through cached nixpkgs

```nix
{
  inputs.nixpkgs.url = "github:NixOS/nixpkgs";
  outputs = { self, nixpkgs }: {
    cachedNixpkgs = builtins.cache { import = nixpkgs; };
    pkgs = mirror: self.cachedNixpkgs {
      system = "x86_64-linux";
      config = {
        allowUnfree = true;
        rewriteURL = url: mirror + url;
      };
    };
  };
}
```

**Frames**: One. The cached value is the nixpkgs lambda, applied
once with `{ system, config }`. No curried push.

**Sibling callbacks**: `rewriteURL` is invoked many times by
nixpkgs's evaluation, once per distinct URL it sees during a
consumer's queries. Each invocation is a sibling callback at the
same depth, distinguished by the content-defined identity of its
URL argument (a string).

**Why content-defined identity matters here**: under counter-based
identity, the order in which nixpkgs forces its lazy structure
shifts which `local:N` label tags which URL across record/replay.
Cold records URLs in one order; warm replay encounters them in
another; counter labels collide on the wrong URLs; walk falls
through. (See the Motivation section and the primop doc's Open Question 1 for the
empirical investigation that surfaced this.)

Under content-defined identity, each URL's content-hash is
determined entirely by its observed string content — concretely,
`getType` + `getStringWithContext` on the inner-supplied URL
string. Cold records `<H_for_"xz.tar">`; warm produces the same
`<H_for_"xz.tar">` regardless of the order it encountered that
URL in. The labels can no longer collide. The sibling-callback
structure (the sibling callbacks example pattern) handles the rest.

**State creep is bounded**: the cached function is non-curried, so
there are no parent frames to creep up from. Each URL apply's
recorded facts are minimal: the URL string's content-hash and the
outer-side `rewriteURL` invocation's observed result. This is why
this workload is the initial-target case for this iteration — the
open issue around evaluation-order × state creep (the Open issues section) doesn't
enter the picture.

## Implementation phasing

The design touches the `Query` data type, the writer's
bookkeeping, the Responses pool, and the walker's dispatch logic.
Landing it all at once would mean a long-lived branch with a single
large diff. The phasing below breaks the work into chunks that are
each independently shippable, with the existing test surface green
throughout.

The starting point is "Step G" of the primop doc — the live-apply-
replay shape that ships `ReplayLocalObject` plus the localArg
sidecar.

### Phase 1: typed `from` field

Mechanical refactor. Today's `from` is a hex string. Promote it to:

```
data QueryLeaf
  = Ambient { index : Int }
  | Content { hash  : Hash }
```

Existing strings interpreted as `Content` (they are already hex
hashes of counter-derived placeholders). All read/write sites for
`from` updated. No semantic change.

This is the foundation — subsequent phases need typed leaves to
distinguish recording-time positional handles from recorded
content-hashes.

### Phase 2: argument scope on the proxy graph

The argument-scope structure (the Argument scope section) is added
to the proxy graph itself: each cache-boundary proxy gets a
`parent` pointer and apply-result proxies get an `argScope` cell.
No frame stack on the writer. No imperative push/pop. Wiring only
— nothing reads `argScope` yet.

A previous iteration of this design introduced a `Frame` /
`pushFrame` / `popFrame` mechanism on `TracingWriter`. That was the
wrong shape — argument scope rides on existing structures, not on a
separate imperative stack — and it gets removed as part of this
phase.

### Phase 3: resolve through the proxy graph

Replace the replay evaluator's global `ambientState` /
`idToObject` member with proxy-graph resolution. Methods on
`TracingReplayObject` thread the proxy through the walker; the
walker's dispatch lambda routes `from` resolutions through the
proxy's `parent`/`argScope` chain.

This is the user-visible correctness phase for multi-call
scenarios. Two `builtins.cache` invocations in the same process no
longer share resolution state through an evaluator-global pointer;
each lazy result Object carries its own argument-scope chain and
forces against it.

### Phase 4: align Local descendants with the chain

Descendant Locals (children of a callback argument's
TracingLocalObject reached via `maybeGetAttr` / `getListElem`) get
their `from` field set to the chain hash, matching what replay's
`ReplayLocalObject.maybeGetAttr` computes. The recording side stops
substituting descendant placeholders to their per-object intrinsics
in `flushPendingAmbient` — top-level Locals (callback args
themselves) still use the intrinsic substitution that backs the
localArg sidecar.

Restoring §2 same-shape collapse for descendants is deferred (see
the Open issues section). The minimum viable Phase 4 trades
descendant collapse for correctness alignment; descendants can
collapse later via an Asks-shaped index from descendant chain to
descendant intrinsic.

### Phase 5: retire seed-counter usage in the cache path

Top-level Ambient seeds at the cache boundary stop using
`hash("seed:N")` placeholders. The seed is given its
content-defined identity at apply time — the empty-set hash, since
no observations on the seed have happened yet. All
`builtins.cache` calls collide on the same apply Q hash;
multi-Terminal disambiguates via the per-call factset produced by
live dispatch through the per-proxy argument scope (Phase 3).

The CLI carve-out (the Principles section) keeps the counter at
the CLI entry points where the patterns are predictable and an
enumeration is sufficient. Only the cache path moves to
content-defined seed identity.

### Phase 6: retire local-arg counter usage

Same treatment for the inner-supplied callback-arg side
(`hash("local:N")` placeholders). The local arg's content-defined
identity at apply time is the empty-set hash; multiple locals in a
single recording start at the same identity and diverge through
their first observations.

A correctness invariant is enforced when this phase lands: every
`AmbientObject` / `TracingLocalObject` construction site must be
right next to its cache-boundary call (the Boundary-trace-only
discipline section). Any caching layer that returns the same
wrapper instance to a later boundary crossing must be reviewed —
the `outerValues` / `localValues` id-keyed maps in
`AmbientResolver` are the suspects.

## Open issues and deferred work

- **Evaluation order × state creep interaction**: content-defined
  factset hashes are stable when the same observations are made,
  but evaluation order differences combined with state creep can
  cause two recordings of the same logical computation to attribute
  observations to different ancestor frames, producing different
  factset hashes for the same value. The Asks / RequestSet
  machinery does not naturally support "a superset of observed
  facts is also a hit," so we cannot easily mitigate at lookup
  time. Mitigation candidates — re-querying with smaller prior
  states; superset-needle lookup over the Asks table — are
  deferred.

  For the initial target case (rewriteURL through cached nixpkgs),
  the cached function is non-curried and lower-frame state creep
  does not enter the picture; this iteration is sufficient. "Many
  top-of-stack calls with the same middle-and-bottom" is the
  practically common case and is handled cleanly.
- **Pointer-identity preservation**: structurally impossible — the
  cache proxies values across the inner-ambient boundary by value,
  not by reference, so no pointer survives the crossing. The
  user-facing wording lives in the `builtins.cache` docstring; the
  rationale is in §Out-of-scope of the primop doc.
- **`QueryApply` Response shape**: applies are path constructors,
  not observation queries; their consequences appear as subsequent
  atomic observations on the apply expression. No separate Response
  shape needed for the apply itself. This is a deliberate
  simplification that may need re-checking once implementation
  surfaces edge cases — for example, semantics that depend on the
  apply step itself rather than on properties of its result.
- **Whether apply-result observations contribute to the applied
  value's content-hash**: a value's identity is its factset of
  observations on it. Apply-result observations are formally
  observations on the *apply result*, not on the function value
  being applied. The conservative reading: apply-result facts do
  not advance the function's content-hash; discrimination between
  behaviourally-different-but-structurally-identical functions
  happens via Response hash mismatch on the apply-result fact. The
  alternative reading folds apply-result observations back into the
  function's factset, giving the function more distinct snapshots
  in the recorded log. Both keep the cache correct; they shift
  where in the (Request hash, Response hash) pair the
  discrimination lives. the higher-order callback example's worked example uses the conservative
  reading. Implementation should pick one and stay consistent.
- **Cross-process counter alignment for `seed:N` / `virtual:N`**:
  today's code uses positional counters for outer-side seeds too.
  Content-defined identity removes this concern for locals, but
  outer-side seeds still need a stable rooting. The cache call's
  own structural identity (`Q_import` or `Q_expr`) provides one:
  each curried apply of the cached value is a deterministic step
  in the call structure, so `seed:N` remains stable across
  processes as long as the call structure is. `virtual:N` for
  values lacking structural identity collapses into content-defined
  hashes under the new scheme.
- **Performance and storage**: content-defined identity adds
  computational cost (hash a factset on each observation) and
  storage cost (the same logical value may appear under several
  distinct content-hashes across its observation history, each
  potentially backed by entries in the Responses pool). Counter-
  based identity was free on both axes. The expected payoff is
  cache hit rate under realistic workloads (nixpkgs and similar)
  — verifying that the hit-rate gain outweighs the recording
  overhead is part of the Phase 4 validation.
