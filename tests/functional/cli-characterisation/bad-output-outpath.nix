{
  type = "derivation";
  name = "bad-output-outpath-1.0";
  outPath = builtins.toFile "out" "";
  outputs = [ "foo" ];
  foo = {
    outPath = "not-a-store-path";
  };
}
