# Per-arg CDI completion

Implementation status for argument-level content-defined identity,
as specified in
[`tracing-eval-cache-content-identity-via-asks.md`](./tracing-eval-cache-content-identity-via-asks.md).

## What's in tree

The cidasks math is implemented and argument-level only:

- `cidasks::contentIdAt(DerivedSubject, ...)` traps via
  `nix::unreachable()`. Only argument-bearing subjects
  (`PositionalSeed`, `ApplyResultSubject`, `OpaqueContentSubject`)
  have CDIs.
- `cidasks::structuralAddress(subject, scope, walk, edgeIndex)` /
  `structuralAddressAfter` exposes a content-addressed identifier
  for *any* subject — for derived subjects it returns the producer
  query's hash, computed without going through `contentIdAt`'s
  trap. Callers that need a `Hash` handle for any proxy route
  through this.
- `ApplyResultSubject`'s `contentIdAt` arm composes via
  `structuralAddress` on its constituents, so a Derived `fn` or
  `arg` participates correctly without re-entering the trap.
- `QueryApply` carries optional `fromCIDs`/`fnPath`/`argPath`/
  `fnRootIndex`/`argRootIndex` fields alongside the legacy
  `fn`/`arg` (mode-tagged via `fromCIDs` population).
- `cidasks::makeApplyResultQuery` builds the per-arg-encoded
  `QueryApply` payload.
- `cidasks::subjectFromObjectIdentity` bridges `Object` identity
  to `Subject`.

The writer's per-arg flush (`tracing-writer.cc:flushPendingAmbient`)
collapses derived chains to the cb_arg root via
`pathAndRootsFromSubject` and stamps every fact with
`from = root_cdi` plus `path`/`fromCIDs`. Depth-1 facts feed
`v13FactSet`; depth-2 facts group per cb-apply into `AmbientAsks`
chains.

The depth-2 local objects (`TracingLocalObject` /
`ReplayLocalObject`) emit queries per-arg via `stampPerArgFields`.

## Red tests (23/25 cb-\* pass)

Two tests are red because the principled mechanism is not fully
built. The red is expected; do not chase it by violating
principles.

### `cb-385` deep-indep test 4

Test 3 records `{a=1; b=99}` in its own nix invocation; test 4
warm-replays in a fresh invocation. `a = args.x.val` hits; `b =
args.y.val` falls through.

**Diagnosis.** The writer's per-arg flush stamps `b`'s fact at
the cb_arg root's CDI as evolved at writer's edge index N (= the
seed cdi after N prior facts have been XOR-folded into its
own-loop). The warm walker's `ResolutionContext::runningWalk`
advances per Asks-edge commit, on a different schedule than the
writer's per-`logResult` `d1EdgeIndex`. When the walker tries
to resolve the writer-stamped CDI, it computes the same subject
at its own current edge index and gets a different CDI. Mismatch.

**Root cause.** Writer and walker evolve CIDs on different
granularities. The principled fix is to align them — both sides
advancing per the same unit (= per dispatched fact, or per
Asks-edge-commit on both sides). Removing evolution to "fix" the
mismatch is not an option (it violates the content-defined
identity principle; see "Cautionary tale" below).

### `cb-sibling-discrimination-via-observation`

Two sibling cb-applies of the same cached fn whose pre-apply
observations match but whose apply-result observations diverge.

**Mechanism (per via-asks Principles 3, 6, 8).** Cumulative
observations evolve the cb_arg root's CID per Foundational #9 +
Principle #3. Within the same warm invocation, sibling A's
`.whatever = 100` folds into `cur` and into the apply-result's
CID (via the apply-result's recursive constituent CID); sibling
B's subsequent `.whatever` lookup uses the post-A `cur` and the
post-A CID, landing at a different trie position from sibling A.

**Root cause of the red.** The walker isn't reproducing the
evolved CIDs at lookup points (= same root cause as cb-385) and,
separately, the apply-result observation pathway that feeds
divergent observations back through to the next sibling's
constituent CIDs is incomplete. Both depend on principled
evolution being in place on both sides.

## Cautionary tale: Fix A

A prior attempt ("Fix A") sidestepped the writer/walker evolution
misalignment by removing evolution from both sides — stamping
facts at the cb_arg root's `contentIdAfter(root, scope, {})`
(empty walk) regardless of accumulated observations. This was
rationalized in an earlier version of this doc as: "Cumulative
is about *which facts are in the dependency set*, not about how
the facts' `from` field is encoded."

That framing splits the CDI principle into "the important part"
(= which facts) and "the encoding" (= the `from` field) and
discards the latter — but content-defined identity is precisely
the link between observed content and identity encoding. Drop
the encoding link and you've dropped CDI; what remains is
structural (positional/derived) identity, the very thing CDI was
designed to refine.

Fix A turned cb-385 green and left cb-sibling red. It was
reverted. Outcome: no new insights, wasted time. The lesson is
in [`../../CLAUDE.md`](../../CLAUDE.md).

## What the principled fix looks like

Per via-asks Principle 6 ("walker advances content ids in
lockstep with `cur`"):

> As each Asks edge dispatches its requests, the walker
> XOR-folds each dispatched response into both the cumulative
> factSetHash and the content ids of relevant subjects.
> Symmetric to recording.

The writer needs to evolve subject CIDs at the same granularity
(= per dispatched fact within an Asks edge), and stamp facts'
`from` fields at the corresponding evolved CID. The walker
reproduces the evolution as it dispatches. Then a fact's `from`
written by the writer and computed by the walker match at every
point in the chain.

This requires:

- A writer-side mechanism that maintains per-subject evolving
  CIDs alongside the cumulative factSetHash, advancing each
  relevant CID when a fact about that subject (= matching
  `from` field) is added.
- A walker-side mechanism that mirrors this: per-subject CIDs
  that advance when dispatching a fact whose recorded `from`
  identifies that subject at some prior chain point.
- Either both sides keyed by Subject (= stable identifier) and
  reading the current CID via a per-walk evaluator, or both
  sides materializing the CIDs incrementally at the same edge
  boundaries.

The "function characterization" follow-up (= task #87 in
via-asks line 155) folds apply-result observations back into
fn's own-loop, so two siblings that differ only in apply
behavior end up with different fn CIDs after their respective
applies are observed. Whether that's separate work from the
alignment fix or falls out of it naturally is open.
