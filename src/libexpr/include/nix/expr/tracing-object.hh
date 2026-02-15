#pragma once

#include "nix/expr/evaluator.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/util/ref.hh"

#include <optional>
#include <string>

namespace nix {

/**
 * Object wrapper that logs all operations to a trace file and optionally to a trie index.
 */
class TracingObject : public Object
{
    ref<Object> inner;
    TracingWriter & writer;
    // TODO: unused? look into referencing strategy
    uint64_t valueNum;
    std::optional<TriePosition> triePos;

    TracingObject(ref<Object> inner, TracingWriter & writer, uint64_t valueNum, std::optional<TriePosition> triePos);

public:
    /**
     * Create a tracing object with the given value number and optional trie position.
     */
    static ref<TracingObject> create(
        ref<Object> inner,
        TracingWriter & writer,
        uint64_t valueNum,
        std::optional<TriePosition> triePos = std::nullopt);

    std::shared_ptr<Object> maybeGetAttr(const std::string & name) override;
    std::vector<std::string> getAttrNames() override;
    std::string getStringIgnoreContext() override;
    std::pair<std::string, NixStringContext> getStringWithContext() override;
    SourcePath getPath() override;
    bool getBool(std::string_view errorCtx = "") override;
    NixInt getInt(std::string_view errorCtx = "") override;
    std::vector<std::string> getListOfStringsNoCtx() override;
    ObjectType getTypeLazy() override;
    ObjectType getType() override;
    RootValue defeatCache() override;
};

} // namespace nix
