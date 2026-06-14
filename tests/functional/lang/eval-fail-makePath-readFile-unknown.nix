# `type = "unknown"` nodes have no readable contents. `readFile`
# fails the same way it would on a real filesystem entry of an
# unsupported type.
builtins.readFile (builtins.makePath { root = { type = "unknown"; }; })
