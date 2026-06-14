let
  p = builtins.makePath {
    root = {
      type = "directory";
      entries = {
        "link" = {
          type = "symlink";
          target = "../../escape";
        };
      };
    };
  };
in
builtins.readFile (p + "/link")
