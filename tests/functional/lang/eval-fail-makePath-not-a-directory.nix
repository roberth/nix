# Traversing through a non-directory mid-path (a regular file used
# as if it were a directory) is rejected at the offending segment.
let
  p = builtins.makePath {
    root = {
      type = "directory";
      entries = {
        "default.nix" = {
          type = "regular";
          contents = "hi";
        };
      };
    };
  };
in
builtins.readFile (p + "/default.nix/cantwork")
