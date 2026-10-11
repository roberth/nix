with import ./config.nix;

{
  # Larger than the part of its NAR that the server sends before it
  # stalls, and incompressible, so that the compressed NAR is too.
  big = mkDerivation {
    name = "substitute-cancel-big";
    buildCommand = "head -c 1000000 /dev/urandom > $out";
  };

  # Fails once the server has stalled the NAR of `big`, so that the
  # substitution of `big` runs when the failure ends the build.
  fail = mkDerivation {
    name = "substitute-cancel-fail";
    serverLog = builtins.getEnv "SERVER_LOG";
    buildCommand = ''
      while [[ "$(< "$serverLog")" != *"stalled nar/"* ]]; do
        sleep 0.1
      done
      exit 1
    '';
  };
}
