# This is a helper to callFlake() to lazily fetch flake inputs.

# The contents of the lock file, in JSON format.
lockFileStr:

# A mapping of lock file node IDs to { sourceInfo, subdir } attrsets,
# with sourceInfo.outPath providing an SourceAccessor to a previously
# fetched tree. This is necessary for possibly unlocked inputs, in
# particular the root input, but also --override-inputs pointing to
# unlocked trees.
overrides:

# This is `prim_fetchFinalTree`.
fetchTreeFinal:

let
  inherit (builtins) mapAttrs;

  lockFile = builtins.fromJSON lockFileStr;

  # Resolve a input spec into a node name. An input spec is
  # either a node name, or a 'follows' path from the root
  # node.
  resolveInput =
    inputSpec: if builtins.isList inputSpec then getInputByPath lockFile.root inputSpec else inputSpec;

  # Follow an input attrpath (e.g. ["dwarffs" "nixpkgs"]) from the
  # root node, returning the final node.
  getInputByPath =
    nodeName: path:
    if path == [ ] then
      nodeName
    else
      getInputByPath
        # Since this could be a 'follows' input, call resolveInput.
        (resolveInput lockFile.nodes.${nodeName}.inputs.${builtins.head path})
        (builtins.tail path);

  allNodes = mapAttrs (
    key: node:
    let
      hasOverride = overrides ? ${key};
      isRelative = node.locked.type or null == "path" && builtins.substring 0 1 node.locked.path != "/";

      parentNode = allNodes.${getInputByPath lockFile.root node.parent};

      # Raw fetcher result. `lazy = true` keeps `outPath` as a path
      # value so `import (flakePath + "/flake.nix")` walks the
      # accessor without forcing a store copy. Relative inputs
      # inherit their parent's fetchResult since they share its tree.
      fetchResult =
        if hasOverride then
          overrides.${key}.sourceInfo
        else if isRelative then
          parentNode.fetchResult
        else
          # FIXME: remove obsolete node.info.
          # Note: lock file entries are always final.
          fetchTreeFinal (node.info or { } // removeAttrs node.locked [ "dir" ] // { lazy = true; });

      subdir = overrides.${key}.dir or node.locked.dir or "";

      # Internal path value used to `import` the flake.nix without a
      # store copy when the fetchResult is path-typed.
      #
      # The relative branch needs the same `+ subdir` tail as the
      # absolute branch: a relative input with `?dir=` (e.g.
      # `path:./sub?dir=inner`) lives at `parent + "/sub/inner"`,
      # not `parent + "/sub"`. Without the tail, `import (flakePath
      # + "/flake.nix")` would try to load the wrong flake.nix.
      flakePath =
        if !hasOverride && isRelative then
          parentNode.flakePath
          + (if node.locked.path == "" then "" else "/" + node.locked.path)
          + (if subdir == "" then "" else "/" + subdir)
        else
          fetchResult.outPath + (if subdir == "" then "" else "/" + subdir);

      flake = import (flakePath + "/flake.nix");

      # User-facing string outPath: built by string concatenation so
      # the subdir appends to the root storePath. Coercing
      # `flakePath` directly would copy the subdir as a leaf-named
      # store object.
      #
      # Why this stays a string and not a path value (i.e. why
      # there's no `inputs.self.lazyPath` here): a flake.outPath
      # has to identify *both* the flake's source tree and its
      # subdir-relative position within that tree. A store path
      # naturally does (one store object = root, subdir is a
      # subpath inside it). A path value `flakePath + "/relative"`
      # only identifies a copy of the subpath, dropping the root
      # and parents — so it can't serve as the flake's outPath
      # without losing the ability to e.g. import sibling files.
      # Making `self` lazy in that sense needs new flake machinery
      # that's out of scope here.
      outPath =
        if !hasOverride && isRelative then
          parentNode.outPath
          + (if node.locked.path == "" then "" else "/" + node.locked.path)
          + (if subdir == "" then "" else "/" + subdir)
        else
          "${fetchResult.outPath}" + (if subdir == "" then "" else "/" + subdir);

      sourceInfo = fetchResult // {
        outPath = "${fetchResult.outPath}";
      };

      inputs = mapAttrs (inputName: inputSpec: allNodes.${resolveInput inputSpec}.result) (
        node.inputs or { }
      );

      outputs = flake.outputs (inputs // { self = result; });

      result =
        outputs
        # We add the sourceInfo attribute for its metadata, as they are
        # relevant metadata for the flake. However, the outPath of the
        # sourceInfo does not necessarily match the outPath of the flake,
        # as the flake may be in a subdirectory of a source.
        # This is shadowed in the next //
        // sourceInfo
        // {
          # This shadows the sourceInfo.outPath
          inherit outPath;

          inherit inputs;
          inherit outputs;
          inherit sourceInfo;
          _type = "flake";
        };

    in
    {
      result =
        if node.flake or true then
          assert builtins.isFunction flake.outputs;
          result
        else
          sourceInfo
          // {
            inherit sourceInfo outPath;
          };

      inherit
        outPath
        flakePath
        fetchResult
        sourceInfo
        ;
    }
  ) lockFile.nodes;

in
allNodes.${lockFile.root}.result
