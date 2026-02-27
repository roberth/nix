{
  type = "derivation";
  name = "bad-output-type-1.0";
  outPath = builtins.toFile "out" "";
  outputs = [ "foo" ];
  foo = "not-an-attrset";
}
