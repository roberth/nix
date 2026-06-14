# A deep mount cycle is recursed through via the SourceAccessor's
# delegate dispatch. Without an explicit callDepth bump per recursion,
# deep paths blow the C stack; with one, Nix raises StackOverflowError
# at the `max-call-depth` setting first.
let
  cyc = builtins.makePath {
    root = {
      type = "directory";
      entries = {
        "leaf" = {
          type = "regular";
          contents = "deep";
        };
        "self" = cyc;
      };
    };
  };
  selfs = builtins.concatStringsSep "" (builtins.genList (_: "/self") 200);
in
builtins.readFile (cyc + (selfs + "/leaf"))
