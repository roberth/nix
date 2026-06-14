let
  p = builtins.makePath {
    root = {
      type = "directory";
      entries = {
        "x" = 42;
      };
    };
  };
in
builtins.readFileType (p + "/x")
