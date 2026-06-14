# A name absent from `entries` is not in the directory; reading it
# fails with the usual file-not-found error.
let
  p = builtins.makePath {
    root = {
      type = "directory";
      entries = {
        "present" = {
          type = "regular";
          contents = "";
        };
      };
    };
  };
in
builtins.readFile (p + "/absent")
