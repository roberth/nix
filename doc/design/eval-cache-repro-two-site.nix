# Custom two-site probe for the eval-cache warm-recursion bug.
#
# Purpose: isolate the trigger to the smallest, most transparent
# shape: cache-bridged navigations to a derivation-building attr
# on `pkgs`, from two separate call sites, one of which is the
# docs-probe pattern (submoduleWith + optionAttrSetToDocList over
# pkgs.ghostunnel.services.default).
#
# No NixOS module system. No stdenv. No runCommand. `derivation`
# is injected into `pkgs` via an overlay so both `pkgs.derivation`
# call sites are cache-bridge navigations of the SAME shape.
#
# Run:
#   NIX_TRACING_CACHE_DIR=/tmp/repro-cache \
#     nix --extra-experimental-features tracing-eval-cache eval \
#     --impure -f doc/design/eval-cache-repro-two-site.nix
#
# Cold: prints a `.drv` path (success).
# Warm: prints `«error: infinite recursion encountered»` inlined
# into the output stream (rc=0 due to Nix printer quirk); stderr
# points to nixpkgs/lib/modules.nix:372:15.
#
# Ingredients that must all be present:
#   1. cachedNixpkgs (lib.cache-wrapped nixpkgs).
#   2. Docs probe: submoduleWith + optionAttrSetToDocList over
#      pkgs.ghostunnel.services.default, folded into a string that
#      lands in Site C's args.
#   3. ≥2 calls to `pkgs.<derivation-builder>` (here two
#      pkgs.derivation calls; also fires with pkgs.runCommand or
#      pkgs.stdenv.mkDerivation — the specific attr doesn't matter
#      as long as it's navigated through cachedNixpkgs).
#   4. The two pkgs.<builder> calls compose: Site C's args reference
#      Site B's outPath, forming a joint derivation.
#   5. Force via `.drvPath` on Site C.
#
# What was tried and does NOT reproduce:
#   - Raw `derivation` builtin (no cache-bridge navigation) — fire gone.
#   - `pkgs.seq` (cache-bridge navigation to a non-derivation-building
#     primop) — fire gone. So it isn't cache-bridged attr access alone;
#     the callee must return a derivation.
#   - Only ONE `pkgs.derivation` call — fire gone.
#   - Two `pkgs.derivation` calls but no docs probe — fire gone.
#   - Docs probe forced through plain `${pkgs.ghostunnel}` interpolation
#     instead of submoduleWith + optionAttrSetToDocList — fire gone.
#   - Wrapping the whole thing in lib.evalModules modules
#     (types.package or types.raw either way) — identical drv hash
#     and identical fire. So the NixOS module system was noise in
#     earlier variants; the trigger is purely the shape below.

let
  lib = import /home/sandbox/nixpkgs/lib;
  cachedNixpkgs = lib.cache { import = /home/sandbox/nixpkgs; };
  # NOTE: `derivation` is exposed as a top-level pkgs attr by a
  # companion patch on the eval-cache-minimization-repro branch of
  # nixpkgs (see pkgs/top-level/all-packages.nix, commit
  # 9b92077d8a33). Overlay-injection also works and produces the
  # same drv hash and same fire, but nixpkgs-native placement
  # confirms the trigger doesn't depend on the overlay mechanism.
  pkgs = cachedNixpkgs {
    config.allowUnfreePredicate = _: true;
    overlays = [ ];
  };

  # Site A: force pkgs.ghostunnel.services.default through the
  # module-system's option-doc walker. This is nixpkgs's
  # documentation.nix behaviour condensed.
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
      builtins.unsafeDiscardStringContext (builtins.toJSON (
        lib.optionAttrSetToDocList inner.options
      ));

  # Site B: first pkgs.derivation call, referencing another by-name
  # pkg (any by-name pkg works; pkgs.hello is arbitrary).
  b = pkgs.derivation {
    name = "b";
    system = "x86_64-linux";
    builder = "/bin/sh";
    args = [ "-c" "ln -sf ${pkgs.hello} $out" ];
  };

  # Site C: second pkgs.derivation call, folding docsProbe and Site B.
  c = pkgs.derivation {
    name = "c";
    system = "x86_64-linux";
    builder = "/bin/sh";
    args = [ "-c" "echo ${docsProbe} ${b} > $out" ];
  };
in
  c.drvPath
