with import ../config.nix;
let
  normal = mkDerivation {
    name = "normal-1.0";
    buildCommand = "mkdir -p $out";
  };
in
{
  type = "derivation";
  name = "output-name-with-context-1.0";
  outPath = builtins.toFile "out" "";
  outputs = [ "${normal}" ];
}
