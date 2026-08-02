# Custom two-site probe for the eval-cache warm-recursion bug.
#
# Purpose: isolate the trigger to the smallest, most transparent
# shape reachable without NixOS module system, without stdenv,
# without runCommand. Uses a minimal 6-line probepkg module
# supplied by nixpkgs (see pkgs/by-name/pr/probepkg/).
#
# Run:
#   NIX_TRACING_CACHE_DIR=/tmp/repro-cache \
#     nix --extra-experimental-features tracing-eval-cache eval \
#     --impure -f doc/design/eval-cache-repro-two-site.nix
#
# Cold: prints a `.drv` path (success).
# Warm: prints `«error: infinite recursion encountered»` inlined
# into the output stream (rc=0 due to Nix printer quirk); stderr
# points to nixpkgs/lib/modules.nix:372:15 (the "options"
# module-arg site).
#
# The submoduleWith-fed module (pkgs.probepkg.services.default) is
# essentially:
#     { }:
#     { lib, options, ... }:
#     {
#       options.p = lib.mkOption { type = lib.types.bool; default = false; };
#       config = builtins.seq options { };
#     }
# Forcing the `options` module argument to WHNF (via seq or the
# equivalent `options ? <name>`) is what recurses under warm replay.
# The `options.p` declaration is required — a module with zero
# options doesn't fire.
#
# Ingredients that must all be present:
#   1. cachedNixpkgs (lib.cache-wrapped nixpkgs).
#   2. Docs probe: submoduleWith + optionAttrSetToDocList over
#      pkgs.probepkg.services.default. The `options ? <name>`
#      check inside its config block is the essential shape.
#   3. ≥2 calls to pkgs.derivation. The specific attr doesn't
#      matter as long as it's navigated through cachedNixpkgs.
#   4. The two pkgs.derivation calls compose: Site C's args
#      reference Site B's outPath, forming a joint derivation.
#   5. Force via `.drvPath` on Site C.
#
# Companion patches on the eval-cache-minimization-repro branch of
# nixpkgs:
#   - pkgs/top-level/all-packages.nix: expose builtins.derivation
#     as pkgs.derivation.
#   - pkgs/by-name/pr/probepkg/{package,service}.nix: minimal
#     probemod that mirrors the ghostunnel-service-default shape
#     to fire the trigger.
#
# What was tried and does NOT reproduce:
#   - Raw `derivation` builtin (no cache-bridge navigation) — gone.
#   - `pkgs.seq` (cache-bridged navigation to non-derivation-building
#     primop) — gone.
#   - Only ONE `pkgs.derivation` call — gone.
#   - Two `pkgs.derivation` calls but no docs probe — gone.
#   - Docs probe over a submodule whose config doesn't reference
#     `options ? <name>` (just declares options and returns empty
#     or plain config) — gone.
#   - Docs probe over a plain `${pkgs.<name>}` interpolation instead
#     of submoduleWith + optionAttrSetToDocList — gone.

let
  lib = import /home/sandbox/nixpkgs/lib;
  cachedNixpkgs = lib.cache { import = /home/sandbox/nixpkgs; };
  pkgs = cachedNixpkgs {
    config.allowUnfreePredicate = _: true;
    overlays = [ ];
  };

  docsProbe =
    let
      inner = lib.evalModules {
        modules = [{
          options."<svc>" = lib.mkOption {
            type = lib.types.submoduleWith {
              modules = [ pkgs.probepkg.services.default ];
            };
          };
        }];
      };
    in
      builtins.unsafeDiscardStringContext (builtins.toJSON (
        lib.optionAttrSetToDocList inner.options
      ));

  b = pkgs.derivation {
    name = "b";
    system = "x86_64-linux";
    builder = "/bin/sh";
    args = [ "-c" "ln -sf ${pkgs.probepkg} $out" ];
  };

  c = pkgs.derivation {
    name = "c";
    system = "x86_64-linux";
    builder = "/bin/sh";
    args = [ "-c" "echo ${docsProbe} ${b} > $out" ];
  };
in
  c.drvPath
