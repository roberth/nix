#pragma once
///@file

#include "nix/util/pos-idx.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/source-path.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/expr/value.hh"

namespace nix {

class EvalState;

/**
 * SourceAccessor whose contents are described by a Nix `Value` tree
 * — the data model accepted by `builtins.makePath`. Each read walks
 * the Value graph from the root, forcing just enough nodes to
 * answer; path-valued nodes mount foreign accessors at their
 * position.
 *
 * Holds a raw back-ref to `EvalState` (sound because the wrapping
 * `SourceRoot` lives in `EvalState::rootCache`) and a GC-rooted
 * `Value *` for the tree root (`RootValue` pins it).
 */
struct ValueSourceAccessor : SourceAccessor
{
    ValueSourceAccessor(EvalState & state, Value & root);

    void anchor() override;

    std::optional<Stat> maybeLstat(const CanonPath & path) override;
    DirEntries readDirectory(const CanonPath & path) override;
    std::string readLink(const CanonPath & path) override;
    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override;

private:
    EvalState & state;
    RootValue root;

    /* Per-accessor cached symbols for attributes not in `state.s`. */
    Symbol sEntries, sContents, sTarget, sExecutable;

    struct Resolved
    {
        Value * node = nullptr;
        std::optional<SourcePath> delegate;
        /* Position of the attribute binding for the leaf entry (e.g.
           `"f"` in `entries.f = …`). Set when `walk` returns
           normally; consulted by per-method catch blocks to position
           errors raised after walk succeeded. `noPos` for the root. */
        PosIdx entryPos = noPos;
    };

    Resolved walk(const CanonPath & path);
    Resolved interpret(Value & v, const CanonPath & at);
    std::string_view nodeType(Value & node, const CanonPath & at);
    const Bindings & nodeEntries(Value & node, const CanonPath & at);
};

} // namespace nix
