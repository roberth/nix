# Two-site probe: diagnosis from `_NIX_TRACING_CACHE_LOGGING=1`

Companion analysis of `doc/design/eval-cache-repro-two-site.nix`.

## Access sequence up to the failure

Walker replays the drvPath computation of the joint derivation
`c` (Site C). The last successful dispatches are:

```
apply: HIT self=b9a9266b20d6 whnf=set        # pkgs.derivation apply → attrset
TRO::maybeGetAttr 'drvPath' -> Q=4f974f356cee
history Q=4f974f356cee ... walks Ask chain through 6 curs
dispatch outer: getAttr name="args" from=b9a9266b20d6 resp=list size=2
dispatch outer: getListElem index=0 from=b8ced1d4023f resp="-c"       # OK
dispatch FAIL   getListElem index=1 from=b8ced1d4023f (no current response)
    <-- infinite recursion at nixpkgs/lib/modules.nix:372:15
```

At `index=1` the walker's `dispatch` invokes live inner evaluation
to reproduce the value (to compare against the recorded response).
The live value is the shell script string with `${docsProbe}`
interpolated. Forcing `docsProbe.outPath` transitively runs
`optionAttrSetToDocList inner.options`, which forces
`config._module.freeformType`, which forces the `options` module
argument — and that fixed-point diverges.

## Why cold succeeds and warm doesn't

Cold's initial evaluation of `docsProbe` runs entirely inside
`TracingEvaluator` making live probes into cachedNixpkgs.
`pkgs.probepkg.services.default` returns the actual module attrset;
the module system's `options` fixed point converges via ordinary
Nix laziness.

Warm's live re-evaluation of the SAME expression runs inside the
walker's dispatch. `pkgs.probepkg.services.default` this time is
served by a TRO (TracingReplayObject), and every subsequent
`.imports`, `.probes.package`, etc. is a cache-bridge dispatch too.
The module system's `let options = merged.matchedOptions; in ...`
fixed point still LOOKS right, but the cache-bridge inserts extra
`SelectorGetAttr` dispatches on the `options` attrset that force
it beyond what the raw fixed point requires. Concretely, the walker
appears to force `config._module.freeformType` first, which
requires `options`, which… loops.

The recursion stack witnessed:
```
… forcing cached attribute 'args' across a `builtins.cache` boundary
… evaluating the option `_module.freeformType`
… forcing cached attribute 'config' across a `builtins.cache` boundary
… forcing cached attribute 'options' across a `builtins.cache` boundary
… evaluating the module argument `options` in "imports.2 imports.1 imports.1 imports.1"
  error: infinite recursion encountered at lib/modules.nix:372:15
```

Four levels of "forcing cached attribute … across a builtins.cache
boundary" — this is where the extra work is coming from.

## Load-bearing ingredients (recap)

- `submoduleWith { modules = [ pkgs.probepkg.services.default ]; }`
  where `pkgs.probepkg.services.default` is cache-bridged.
- `config = builtins.seq options { }` inside probepkg's service.nix.
  Any way of forcing the `options` module arg during config eval
  works; `options ? <name>` is equivalent.
- At least one option declaration (`options.p = mkOption { … };`).
  Modules with zero options don't fire.
- The docsProbe string must land as an argument of a derivation
  built via `pkgs.derivation` (a cache-bridged navigation to a
  derivation-returning attr on pkgs), and that derivation must be
  composed with a SECOND cache-bridged derivation-building call.

## Not yet tracked down

- Which specific SelectorGetAttr on `options` inserts the extra
  force. The cell-chain dumps in the log (~350 lines each) show
  the walker's fact accumulation but not the C++-level force site.
- Whether the fix belongs in `TracingReplayObject::maybeGetAttr`'s
  routing for module-system option attrs, or in the writer's
  recording basis that produces the trace warm is now replaying,
  or somewhere else entirely.
