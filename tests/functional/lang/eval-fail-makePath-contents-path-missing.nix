# `contents` as a path is read through the path's own accessor on
# force. If that path doesn't exist, the underlying FileNotFound
# propagates with both contexts visible: the tree-side reference
# and the actual missing disk path.
let
  p = builtins.makePath {
    root = {
      type = "directory";
      entries = {
        "f" = {
          type = "regular";
          contents = ./this-does-not-exist;
        };
      };
    };
  };
in
builtins.readFile (p + "/f")
