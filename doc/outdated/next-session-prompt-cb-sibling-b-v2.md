# Next session: close cb-sibling-b-depends-on-a

Prior sessions have made substantial progress but the test still
fails. Baseline is **323 pass, 1 fail (cb-sibling-b-depends-on-a),
7 skipped**. Your task: land the last mile.

## Step 1 — Required reading (do NOT skip)

Read fully, in this order:

1. `doc/design/tracing-eval-cache.md` — primary design doc, especially
   the "Navigation invariant: IDs flow into lookups as keys, never
   out of lookups" section
2. `doc/design/tracing-eval-cache-content-identity-via-asks.md` —
   principle 3 (per-arg centralization), principle 6 (navigation
   invariant), and section on `PostulatedIdempotentRead` contract
3. `doc/design/tracing-eval-cache-primop.md` — how primops thread
   through the cache boundary; especially "The fix: producer query
   as id" section
4. `doc/design/tracing-eval-cache-per-arg-completion.md` — spells
   out the remaining architectural gap; note that this document's
   "next principled edit" is now partially addressed but not
   completed
5. `/home/sandbox/.claude/CLAUDE.md` — user's global instructions
6. `/home/sandbox/nix/CLAUDE.md` — project's design principles
7. Memory index: `/home/sandbox/.claude/projects/-home-sandbox-nix/memory/MEMORY.md`
   — especially `cb-sibling-b-shared-Q-collision.md`,
   `cb-sibling-b-dispatch-routing.md`,
   `cb-sibling-cdi-partition-mismatch.md`
8. Recent commit log: `git log --oneline eval-cache-v13-primop -20`
   — the last ~15 commits are all incremental improvements toward
   this test. Read the messages carefully; each commit's message
   explains what it fixed and why cb-sibling-b remains open

## Step 2 — Rebuild your understanding of the failure

Sibling A: `cached { f = x: {whatever = x*100 }; x = 1 }` → a.whatever = 100
Sibling B: `cached { f = x: {whatever = x*1000}; x = a.whatever }` → b.whatever = 100000
Expected: `a.whatever + b.whatever = 100100`

At warm walk of Q=5b16d671c5ac (getWHNF on sibling B's `.whatever`),
walker's dispatch of `req=9f84b3c5f6a3` returns `int 1000` (mixing
sibling A's `x=1` × sibling B's f-multiplier `1000`) instead of cold's
recorded `int 100000` (sibling B's `100 × 1000`). The wrong response
gives a wrong nextCur → no recorded edge → walk misses → falls
through to inner (which `_NIX_DISALLOW_CACHE_INTERPRET_INNER=1`
forbids) → test fails.

The mixing happens because walker's live invocation of `f(x)` uses
`fnObj->queryApply(argObj)` inside `navigatePath`. This fires
`AmbientObject::queryApply → applyFn → AmbientApply::run`. Inside
`AmbientApply::run` (line 292 of `src/libexpr/expr-from-object.cc`),
`Hash argScope = resolverHandle->callScope;` reads the resolver's
current callScope — which is `6d0279be9719` (the oldScope /
sibling-agnostic default), NOT the sibling-specific siblingScope.

## Step 3 — The precise architectural gap

`TracingEvaluator::apply` (line 448-483 of
`src/libexpr/tracing-evaluator.cc`) has a `CallScopeGuard` that sets
`resolver.callScope = siblingScope` at cb-apply time, then RESTORES
it in the destructor. My commit `e750beab9` mixes
`writer.v13FactSetHash` into siblingScope so sibling A and B produce
distinct values.

The problem: `AmbientApply::run` fires **later** than
`TracingEvaluator::apply` returns. The outer's forcing of the apply
result (e.g., `a.whatever + b.whatever` forcing) happens after the
guard has destructed. By then, `resolver.callScope` is back to
`oldScope` (6d0279be9719).

Confirmed via debug logs (commit `83af47c87`):
- Cold log line 84: `siblingScope=00878c5cbcc2` (sibling A)
- Cold log line 103: `AmbientApply::run: argScope=6d0279be9719` (restored)
- Same for sibling B: siblingScope_B=`7c30cba66a6a` set at line 3049,
  but AmbientApply::run reads 6d0279be9719

Result: both siblings' inner-apply `argId = 31755538319b` (same).
The sidecar payload lands under one reqhash. LRM has ONE entry
(first-writer wins → sibling A's `x=1`). Walker's warm dispatch of
sibling B's f queries LRM for `.x`, gets `1`, computes `1 × 1000 = 1000`.

## Step 4 — Solve it

Fix the "sibling identity is lost between TracingEvaluator::apply and
AmbientApply::run fire time" problem. The user's guidance in prior
session:

> "The scope evolves too. If you're always sampling the CDIs and not
> storing intermediate hashes for later emission it should be fine"

Interpretation: sample siblingScope FRESHLY at AmbientApply::run
fire time, don't freeze at closure-creation. Never emit stored
intermediate hashes; always re-compute.

Two directions previously attempted and reverted:

### Direction A — Close-time capture of siblingScope

Modify `AmbientApplyFn` signature and closure in
`makeCachedFnPrimOp` to capture `resolver->callScope` at closure
CREATION time. This is FREEZING (user said don't) — but the value
snapshot is a **scope input**, not a CDI. Debate: is capturing a
scope-that-evolves at a specific moment "freezing" in the forbidden
sense? User's message suggests it IS problematic because scopes
evolve. Verdict: don't pursue this.

### Direction B — Don't restore callScope in the guard

Remove the guard's restore. Sibling A's siblingScope stays on
resolver.callScope after its inner->apply returns. Sibling B's
subsequent guard captures oldScope=siblingScope_A → different
siblingScope_B. AmbientApply::run samples current callScope freshly.

**Attempted this session — regressed 18 cb-* tests.** Reverted.
Root cause of regression: other cb tests rely on callScope being
restored to a specific value at nested/curried cb-applies. Persist-
ing siblingScope across boundaries broke that assumption.

### Direction C (recommended) — Sample fnObj's inheritedScope at fire time

The sidecar's reqhash IS the argIdStr (`argId.to_string(hex)`).
argId = `scopeStateIdAfter(seed(depth), argScope, {})`. If argScope
reflects sibling identity, argId differs across siblings → distinct
sidecar entries in LRM → walker's warm dispatch retrieves the
correct sibling's response.

**Correct source for argScope**: fnObj's `inheritedScope`,
NOT `resolverHandle->callScope`.

Why this is principled:

1. `AmbientObject::inheritedScope` is set exactly once at each
   object's construction (line 60 for navigation children;
   line 262 for apply-result wrappers; line 581 of `expr-from-object.cc`
   for the seed AmbientObject). At that moment, `resolver->callScope
   = siblingScope` (guard active during makeCachedFnPrimOp's impl
   running inside TracingEvaluator::apply's inner->apply).
2. AmbientObjects are **per-sibling** — sibling A's cb-arg is one
   AmbientObject; sibling B's is a different AmbientObject. Their
   inheritedScopes were populated at their respective apply times
   (with their respective siblingScopes).
3. Reading `fnObj->getInheritedScope()` at fire time IS sampling —
   we're reading a stable per-object field, not a snapshot we
   ourselves stored. The AmbientObject IS the sibling; its scope IS
   the sibling's identity.
4. No new closures, no new fields, no captured shared_ptrs, no
   guard changes. Existing paradigm.

**Concrete implementation:**

In `AmbientApply::runOn` (line 274 of `expr-from-object.cc`),
replace:
```cpp
Hash argScope = resolverHandle->callScope;
```
with:
```cpp
Hash argScope = fnObj->getInheritedScope();
// Fallback to resolver->callScope if fnObj is not an AmbientObject
// or its inherited scope is default-zero (non-cb-apply paths).
if (argScope == Hash(HashAlgorithm::SHA256))
    argScope = resolverHandle->callScope;
```

`Object::getInheritedScope()` is a virtual method with a default
returning zero-hash; `AmbientObject` overrides it (see
`ambient-object.hh` line 122's TracingObject override for the
pattern). Verify AmbientObject has the override, add if not.

Walker mirror: same change in `AmbientApply::runOn`'s walker path
(if there's a separate one). The walker's warm-time AmbientObject
is constructed by `dispatchApplyLive`'s ReplayLocalObject path,
which propagates the sidecar's `scope` field into the RLO — so at
warm, `fnObj->getInheritedScope()` returns the recorded sibling's
scope. Verify this — it may already work by construction.

### Why NOT these other directions

- **Freezing at closure creation** (my earlier Direction A):
  captures at BEFORE-guard time, so captures oldScope not
  siblingScope. Also violates the "don't freeze scopes that
  evolve" principle.
- **shared_ptr<Hash> for live sampling**: this IS "storing
  intermediate hashes for later emission" (something writes the
  hash; the closure dereferences later). Same paradigm violation.
- **Adding an `applyOverride` field to AmbientObject**: redundant
  with the existing `inheritedScope` field, which is already the
  right value at the right moment.
- **No-restore in the guard**: regressed 18 cb-* tests this
  session. Breaks callScope-restore assumptions elsewhere.

## Step 5 — Test scope

Any fix MUST preserve ALL of:
- `cb-sibling-b-depends-on-a` (the target)
- `cb-sibling-discrimination-via-observation`
- `cb-sibling-a-has-bottom`
- `cb-forcedness-independence` (principle #8)
- `cb-same-shape-collapse`
- `cb-same-shape-distinguish`
- `cb-xor-evolution-repeated-cb-apply` (nested applies, repeat calls)
- `cb-higher-order` and `cb-higher-order-nested` (curried applies)
- `cb-385` (single-cb-apply baseline)
- `cb-curried-state-creep`
- `cb-arg-itself-lazy`
- All `cb-stats-*-baseline` (cache statistics assertions)
- `builtins-cache` (aggregate)

Test cadence:
- Fast iteration: `meson test -C build cb-sibling-b-depends-on-a --print-errorlogs`
- Full cb-* verification: `meson test -C build $(meson test -C build --list 2>&1 | grep "cb-\|builtins-cache" | awk '{print $NF}' | tr '\n' ' ')`
- Pre-commit: full `meson test -C build`

## Step 6 — Debug harness

There's an existing debug harness at `/tmp/cbdbg/`:
```
/tmp/cbdbg/fn.nix         # {f, x}: f x
/tmp/cbdbg/run.sh         # runs nix eval with cache logging enabled
```

Usage:
```
rm -rf /tmp/cbdbg/cache
/tmp/cbdbg/run.sh "" > /tmp/cbdbg/cold.log 2>&1
/tmp/cbdbg/run.sh 1 > /tmp/cbdbg/warm.log 2>&1
```

`""` runs cold (no `_NIX_DISALLOW_CACHE_INTERPRET_INNER`); `1`
runs warm with `_NIX_DISALLOW_CACHE_INTERPRET_INNER=1` set (the
strict mode the test uses).

Key log signals for cb-sibling-b:
- `AmbientApply::run: argScope=...` — sibling identity at run time
- `writer apply: fn=... arg=... scope=... -> applyScopeStateId=...` —
  the applyResult wrapper's identity at apply time
- `dispatch ambient: req=... payload=... from=... resp=...` — walker's
  live dispatches during walk
- `dispatch FAIL req=... payload=... (no current response)` — failed
  resolves
- `MATCH via cross-Q pool pull (k=..., ... obs)` — the pool pull
  mechanism successfully bridging CDI gaps
- `walk Q=... startCur=... outgoing=... rs=... useful=... nextCur=...` —
  walker's walk progression
- `NO EDGE COMMITTED at cur=... -> miss` — the walk failure

## Step 7 — Guardrails

**Don't freeze CDIs.** Sample freshly at use time. If you find
yourself writing `Hash captured = something.cdi;` and using
`captured` later, ask whether the scope has evolved between the
capture and the use. If yes, capture is wrong.

**Don't emit stored intermediate hashes.** Under the Asks paradigm,
hashes flow *into* lookups as keys, never out. If you're plumbing
a Hash value from one place to another for "later emission," you're
likely on the wrong path.

**Always both sides.** Any change to recorder (`TracingEvaluator`,
`TracingWriter`, `TracingLocalObject`) must be paired with the
corresponding walker (`TracingReplayEvaluator`, `TracingReplayObject`,
`ReplayLocalObject`) change. Walker miss with "no cached response"
under DISALLOW_CACHE_INTERPRET_INNER is a symptom of one-sided change.

**Don't break forcedness independence.** Cache identity of the same
call at the same source position must not depend on whether the
argument arrives forced or as a thunk.

**Preserve baseline throughout.** After every code change:
`meson test -C build cb-sibling-b-depends-on-a` first (fast); then
if that changes state, run the full cb-* suite.

## Step 8 — Landed work (16 commits) — DO NOT REPEAT

Recent commits on the branch:
```
83af47c87 debug(eval-cache): log argScope in AmbientApply::run
478f3ed88 feat(eval-cache): seed RLO walkFacts with currentProxy applyContext obs
8f9413794 feat(eval-cache): progressive cross-Q pool pull for multi-hop CDIs
085bd8eca feat(eval-cache): iterative multi-round fold + remove pool-pull dedup
e1ab82c60 refactor(eval-cache): dedup crossQPulled against cidasksWalk in extendedWalkForMatch
618dc1fef feat(eval-cache): allow nested cross-Q pool pulls for different targets
c96a736cc refactor(eval-cache): iterative and collected fold use extendedWalkForMatch
bad72a88c fix(eval-cache): cell-chain match uses extendedWalk with crossQPulled
6e24854d4 refactor(eval-cache): dedupe cross-Q pool pull obs against effective walk
321d4b4f9 refactor(eval-cache): pass dispatchedRequestSet as startCurRequests at parentAnchor
4cfd1c9cb fix(eval-cache): use ctx.currentProxy for parentAnchor lookup
e750beab9 refactor(eval-cache): mix v13FactSetHash into sibling callScope
44eb270ac refactor(eval-cache): stop committing rejected-edge obs to cidasksWalk
6d0e8d9ec docs(eval-cache): make navigation invariant explicit; drop speculative-depth comment
76bfb8187 fix(eval-cache): reject XOR-coincidence cell matches via LRM probe
d251ad3dd feat(eval-cache): persist cross-Q pulls across walks
```

Each of these is a working improvement. Read commit messages for
context. Do NOT try to re-implement the mechanisms these commits
introduced.

## Step 9 — Working style

- Small, principled commits. Each should build cleanly and either
  preserve baseline or make measurable forward progress.
- Conventional Commits format. Focus commit messages on **why**.
- Update or add design docs when your fix reveals a new invariant.
- Update memory when your fix reveals a pattern worth cross-session
  persistence.
- If you find yourself repeating "we know 78b1d6c0d465 resolves via
  pool pull, but sibling routing is still off" — stop and re-read
  memory files. That answer is already documented.

## Step 10 — Commit and update memory when done

If you close cb-sibling-b:
1. Verify full baseline: `meson test -C build` — expect 324/0.
2. Commit the fix with a clear "fixes cb-sibling-b-depends-on-a"
   message.
3. Update `MEMORY.md` and archive the "shared Q collision" /
   "dispatch routing" memories with a resolution note.
4. Update `doc/design/tracing-eval-cache-per-arg-completion.md`
   to reflect that the "next principled edit" has landed.
