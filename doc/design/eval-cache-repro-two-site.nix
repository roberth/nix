# Custom two-site probe for the eval-cache warm-recursion bug.
#
# Purpose: reproduce the bug WITHOUT the NixOS module system —
# no evalConfig, no baseModules, no outer lib.evalModules. Only
# plain `let` bindings, plus one small internal lib.evalModules
# call to drive submoduleWith's option-doc forcing.
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
# Structural essentials — the three let-bindings and the join:
#   - docsProbe: a runCommand whose build inputs include the
#     JSON dump of optionAttrSetToDocList on a submoduleWith whose
#     modules include pkgs.ghostunnel.services.default. This is
#     nixpkgs/nixos/modules/misc/documentation.nix condensed.
#   - pkgRefBuilder: a separate runCommand referencing any
#     by-name pkg via ${pkgs.hello}. Mirrors
#     switchable-system.nix's ${pkgs.<by-name>} interpolation.
#   - toplevel: a joint runCommand that folds both derivations
#     via ln -s. Forced via .drvPath.
#
# What was tried and does NOT reproduce:
#   - Replacing Site A's submoduleWith+optionAttrSetToDocList with
#     a plain ${pkgs.ghostunnel} interpolation → cold and warm both
#     succeed. So the specific forcing pattern (submoduleWith over a
#     module-fn, forced through optionAttrSetToDocList) is required.
#   - Skipping the joint toplevel runCommand and returning
#     `docsProbe.drvPath + pkgRefBuilder.drvPath` → cold and warm
#     both succeed.
#   - Wrapping the three bindings in lib.evalModules modules with
#     types.raw options → same behaviour as this file. So the
#     module system was not load-bearing in the original three-
#     module probe; the let-binding version has the identical drv
#     hash and identical fire.

let
  lib = import /home/sandbox/nixpkgs/lib;
  cachedNixpkgs = lib.cache { import = /home/sandbox/nixpkgs; };
  pkgs = cachedNixpkgs { config.allowUnfreePredicate = _: true; overlays = [ ]; };

  # Site A: force pkgs.ghostunnel.services.default through the
  # module-system's option-doc walker.
  docsProbe =
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

  # Site B: any by-name pkg (pkgs/by-name/...); pkgs.hello works.
  pkgRefBuilder = pkgs.runCommand "pkg-ref-builder" { } ''
    ln -sf ${pkgs.hello} $out
  '';

  # Site C: joint fold via ln -s. Forced via .drvPath below.
  toplevel = pkgs.runCommand "toplevel" { } ''
    mkdir $out
    ln -s ${docsProbe} $out/docs
    ln -s ${pkgRefBuilder} $out/pkgref
  '';
in
  toplevel.drvPath
