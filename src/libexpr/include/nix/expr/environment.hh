#pragma once
/**
 * @file
 * Abstract Environment interface for evaluation I/O operations.
 */

#include <memory>
#include <optional>
#include <string>
#include "nix/expr/observation-set.hh"
#include "nix/expr/trace-types.hh"
#include "nix/util/hash.hh"
#include "nix/util/ref.hh"

namespace nix {

struct SourceAccessor;
class Store;
struct EvalSettings;
struct ArgCell;  // forward decl — outerQuery accepts an opaque attribution cell

/**
 * Environment interface for evaluation I/O operations.
 *
 * This interface abstracts external state access needed during evaluation.
 */
class TraceSink;

class Environment
{
public:
    virtual ~Environment();

    /**
     * Get the root filesystem accessor.
     * Delegates filesystem operations (readFile, pathExists, readDirectory, etc.)
     * to the SourceAccessor interface.
     * @return Reference to the root source accessor
     */
    virtual ref<SourceAccessor> fsRoot() = 0;

    /**
     * Get environment variable value.
     * @param name Variable name
     * @return Optional value (nullopt if not set)
     */
    virtual std::optional<std::string> getEnv(const std::string & name) = 0;

    /**
     * Get the SHA-256 content hash of a file.
     *
     * Reads through the environment's accessor chain so that tracing
     * layers can observe the access. Default implementation reads
     * via fsRoot() and hashes the contents.
     *
     * TODO: currently assumes absolute paths on the root filesystem.
     * Non-`/` source accessors (e.g. flake inputs) will need a
     * SourcePath-based variant.
     */
    virtual Hash getFileHash(const std::string & path);

    /**
     * Issue an outer query and return the result. `producer` and
     * `attributionCell` route the observation to the correct cell
     * on the tracing override; the default implementation just
     * delegates to the supplied resolve callback.
     */
    virtual trace::ResultVariant outerQuery(
        ref<const trace::Selector> query,
        std::function<trace::ResultVariant(ref<const trace::Selector>)> resolve,
        ref<const trace::Selector> /* producer */,
        const std::shared_ptr<const ArgCell> & /* attributionCell */ = {})
    {
        return resolve(query);
    }

    /**
     * Get the trace sink, if tracing is enabled.
     * @return Pointer to TraceSink, or nullptr if not tracing
     */
    virtual TraceSink * getTraceSink()
    {
        return nullptr;
    }
};

} // namespace nix
