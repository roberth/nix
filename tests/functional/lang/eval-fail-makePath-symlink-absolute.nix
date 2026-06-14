let
  p = builtins.makePath {
    root = {
      type = "directory";
      entries = {
        "link" = {
          type = "symlink";
          target = "/etc/passwd";
        };
      };
    };
  };
in
builtins.readFile (p + "/link")
