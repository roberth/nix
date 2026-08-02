# Custom two-site probe for the eval-cache warm-recursion bug.
#
# Purpose: reproduce the bug in a plain `lib.evalModules` call, WITHOUT
# `evalConfig` / NixOS baseModules / module-list.nix. Isolates the trigger
# to a shape that's tractable to reason about.
#
# Run:
#   NIX_TRACING_CACHE_DIR=/tmp/repro-cache \
#     nix --extra-experimental-features tracing-eval-cache eval \
#     --impure -f doc/design/eval-cache-repro-two-site.nix
#
# Cold: prints a `.drv` path (success).
# Warm: prints `«error: infinite recursion encountered»` inlined into
# the output stream (rc=0 due to Nix printer quirk).
#
# Structure (mirrors NixOS's system.build.toplevel):
#   - Site A (docsProbe) is `optionAttrSetToDocList` over a
#     `submoduleWith` whose modules include `pkgs.ghostunnel.services.default`,
#     wrapped in a `pkgs.runCommand` producing a derivation. This is
#     nixpkgs's `documentation.nix` behavior condensed.
#   - Site B (pkgRefBuilder) is a separate module that produces a
#     derivation referencing an unrelated by-name pkg (`pkgs.hello`).
#     This mirrors switchable-system.nix's `${pkgs.<by-name>}`
#     interpolation into `system.<x>BuilderCommands`.
#   - Site C (toplevel) folds both sites into a single output
#     derivation and is what `.drvPath` forces. This is
#     `system.build.toplevel` in miniature.
#
# What DOES NOT reproduce (previously tested):
#   - Two-site combination in a single `lib.evalModules` module (one
#     module with both site A and site B inlined) → cold and warm
#     both succeed.
#   - Two modules where site B holds both the pkgs.hello ref AND
#     the toplevel-composing derivation → cold and warm both succeed.
#   - Three-module shape but returning `docsProbe + "\n" + pkgRef`
#     from `in ...` (no shared derivation) → cold and warm both succeed.
#
# So the essential ingredients are:
#   1. cachedNixpkgs (lib.cache wrapper on nixpkgs)
#   2. Site A: pkgs.ghostunnel.services.default via submoduleWith,
#      forced through optionAttrSetToDocList, wrapped in a derivation
#   3. Site B: a *separate module* whose config produces a derivation
#      referencing some other by-name pkg (pkgs/by-name/...)
#   4. Site C: a *third module* whose config folds both sites'
#      derivations into a single output derivation
#   5. Force via `.drvPath` on Site C's derivation

let
  lib = import /home/sandbox/nixpkgs/lib;
  cachedNixpkgs = lib.cache { import = /home/sandbox/nixpkgs; };
  pkgs = cachedNixpkgs { config.allowUnfreePredicate = _: true; overlays = [ ]; };

  eval = lib.evalModules {
    modules = [
      { _module.args.pkgs = pkgs; }

      ({ pkgs, lib, ... }: {
        options.docsProbe = lib.mkOption { type = lib.types.package; };
        config.docsProbe =
          let
            inner = lib.evalModules {
              modules = [{
                options."<svc>" = lib.mkOption {
                  type = lib.types.submoduleWith {
                    modules = [ pkgs.ghostunnel.services.default ];
                  };
                };
              }];
            };
          in
            pkgs.runCommand "docs-probe" {
              options = builtins.unsafeDiscardStringContext (builtins.toJSON (
                lib.optionAttrSetToDocList inner.options
              ));
              passAsFile = [ "options" ];
            } "cp $optionsPath $out";
      })

      ({ pkgs, lib, ... }: {
        options.pkgRefBuilder = lib.mkOption { type = lib.types.package; };
        config.pkgRefBuilder =
          pkgs.runCommand "pkg-ref-builder" { } ''
            ln -sf ${pkgs.hello} $out
          '';
      })

      ({ config, pkgs, lib, ... }: {
        options.toplevel = lib.mkOption { type = lib.types.package; };
        config.toplevel = pkgs.runCommand "toplevel" { } ''
          mkdir $out
          ln -s ${config.docsProbe} $out/docs
          ln -s ${config.pkgRefBuilder} $out/pkgref
        '';
      })
    ];
  };
in
  eval.config.toplevel.drvPath
