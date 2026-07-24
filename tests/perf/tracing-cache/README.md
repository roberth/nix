# Tracing eval cache performance + correctness benches

Bench harnesses for the tracing eval cache — the persistent
walk-based cache described in `doc/design/tracing-eval-cache.md`.
These are developer tools, not automated tests: they need a built
`nix` binary and (for the scale-testing scripts) a nixpkgs checkout.

## Environment

- `NIX_BIN_DIR` — directory containing the `nix` binary to test
  (defaults to `$repo_root/build/src/nix`).
- `NIX_TRACING_CACHE_DIR` — override for the cache directory. When
  set, the DB lives at `$NIX_TRACING_CACHE_DIR/decision-graph.sqlite`;
  otherwise at `$XDG_CACHE_HOME/nix/eval-tracing-decision-graph/index.sqlite`.
- `NIXPKGS` — path to a nixpkgs checkout, for scripts that traverse
  attribute sets (`scale.sh`, `scaling-threshold.sh`,
  `nixpkgs-validate.sh`, `multi-branch-bench.sh`,
  `git-history-bench.sh`).

## Runnable without nixpkgs

- `smoke.sh` — verifies the SQLite DB gets created for a trivial eval.
- `synthetic.sh` — correctness across file edits and reverts (uses
  direct SQLite for row-count stats; the `nix eval-cache stats`
  subcommand it originally used is removed and listed under Future
  Work in `doc/design/tracing-eval-cache-primop.md`).
- `cold-warm.sh` — cold vs three warm runs of the same trivial eval;
  timings + Ask / Terminal / Requests / Results / Selectors row counts.
- `hit-rate.sh` — file-edit-driven workload; counts `replay hit` /
  `replay miss` / `replay fallback` log lines per pass.
- `complex-workload.sh` — larger eval with lib import, conditionals,
  attribute traversal; three warm runs plus edit passes.
- `with-reads.sh` — cold/warm timings on an eval that reads files.
- `inspect.sh` — dumps the current DB's row counts by table.

## Requires nixpkgs

- `scale.sh` — walks a wide list of nixpkgs attrs, checkpointing
  warm-latency on an anchor at K=5/10/20/40/80.
- `scaling-threshold.sh` — K=1..1000 sweep to find soft-regression
  thresholds.
- `nixpkgs-validate.sh` — end-to-end correctness across nixpkgs
  commits and attribute variants.
- `multi-branch-bench.sh`, `git-history-bench.sh` — cross-branch /
  cross-commit cache reuse.
- `vs-uncached.sh`, `vs-uncached-expensive.sh` — cache-on vs cache-off
  timing.

## Baseline (2026-07-09)

Trivial workloads on the sandbox, wall-clock per eval:

| Script | Cold | Warm |
|---|---|---|
| `cold-warm.sh` (arithmetic + string ops) | 23 ms | 19-25 ms |
| `hit-rate.sh` (two file reads) | 22 ms | 22-25 ms |
| `complex-workload.sh` (import + conditional) | 21 ms | 20-23 ms |

At these scales the walk hits are dominated by process startup
(~20 ms) so warm doesn't beat cold visibly on wallclock; the useful
signal is that hits fire (hit-rate reports 3 hits on warm-same;
complex-workload reports 15 hits on warm-1). The scaling-behaviour
tests that quantify walker cost per Q require a nixpkgs checkout.

Design goals the harness validates against are in
`doc/design/tracing-eval-cache.md` under "Performance goals" —
no linear search, no unbounded backtracking, session-cumulative
work proportional to observed change, structural storage sharing.
