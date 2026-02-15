
Assorted ideas

- Generate realistic-ish sequence of invocations with result paths and traces, save as JSON
  - Implement (hidden?) nix subcommands to work with the index
    - `nix eval-tracing-internal import <timestamp> 1.json <timestamp> 2.json`
    - or add timestamp to JSON files
  - Make it log timing info, lookup flow stats.

- Killer app: evaluate NixOS tests in CI
  - Take cache from master branch or hydra
  - `--log-changed-file FILE`?
  - `--build-changed` flag?
  - `--max-derivations NUM`?
