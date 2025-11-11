#include "nix/fetchers/fetchers.hh"
#include "nix/fetchers/typed-input-variant.hh"
#include "nix/fetchers/path-typed.hh"
#include "nix/fetchers/git-typed.hh"
#include "nix/fetchers/tarball-typed.hh"
#include "nix/fetchers/github-typed.hh"
#include "nix/fetchers/mercurial-typed.hh"
#include "nix/fetchers/indirect-typed.hh"

#include <any>

namespace nix::fetchers {

/**
 * Check if an Input has a typed input stored.
 */
bool hasTypedInput(const Input & input)
{
    return input.typedInput.has_value();
}

/**
 * Try to get the typed input from an Input, if present.
 * Returns nullopt if no typed input is stored.
 */
std::optional<AnyTypedInput> getTypedInput(const Input & input)
{
    if (!input.typedInput.has_value())
        return std::nullopt;

    try {
        return std::any_cast<AnyTypedInput>(input.typedInput.value());
    } catch (const std::bad_any_cast &) {
        return std::nullopt;
    }
}

/**
 * Set the typed input for an Input.
 * This stores the typed input alongside the existing attrs.
 */
void setTypedInput(Input & input, const AnyTypedInput & typedInput)
{
    input.typedInput = typedInput;
}

/**
 * Try to create a typed input from an Input's attrs based on its type.
 * Returns nullopt if the type is unknown or conversion fails.
 */
std::optional<AnyTypedInput> attrsToTypedInput(const Settings & settings, const Attrs & attrs)
{
    auto typeOpt = maybeGetStrAttr(attrs, "type");
    if (!typeOpt)
        return std::nullopt;

    auto type = *typeOpt;

    try {
        if (type == "path") {
            return pathInputFromAttrs(settings, attrs);
        } else if (type == "git") {
            return gitInputFromAttrs(settings, attrs);
        } else if (type == "tarball" || type == "file") {
            return tarballInputFromAttrs(settings, attrs);
        } else if (type == "github" || type == "gitlab" || type == "sourcehut") {
            return githubInputFromAttrs(settings, attrs);
        } else if (type == "hg") {
            return mercurialInputFromAttrs(settings, attrs);
        } else if (type == "indirect") {
            return indirectInputFromAttrs(settings, attrs);
        }
    } catch (...) {
        // If conversion fails, return nullopt
        return std::nullopt;
    }

    return std::nullopt;
}

// Helper template to convert any typed input state to Attrs
template<typename T>
Attrs typedInputToAttrsImpl(const T & input)
{
    // Use C++20 if constexpr with template parameter
    using BaseType = std::decay_t<T>;

    if constexpr (
        std::is_same_v<BaseType, PathUnlockedInput> || std::is_same_v<BaseType, PathLockedInput>
        || std::is_same_v<BaseType, PathFinalInput>) {
        return pathInputToAttrs(input);
    } else if constexpr (
        std::is_same_v<BaseType, GitUnlockedInput> || std::is_same_v<BaseType, GitLockedInput>
        || std::is_same_v<BaseType, GitFinalInput>) {
        return gitInputToAttrs(input);
    } else if constexpr (
        std::is_same_v<BaseType, TarballUnlockedInput> || std::is_same_v<BaseType, TarballLockedInput>
        || std::is_same_v<BaseType, TarballFinalInput>) {
        return tarballInputToAttrs(input);
    } else if constexpr (
        std::is_same_v<BaseType, GitHubUnlockedInput> || std::is_same_v<BaseType, GitHubLockedInput>
        || std::is_same_v<BaseType, GitHubFinalInput>) {
        return githubInputToAttrs(input);
    } else if constexpr (
        std::is_same_v<BaseType, MercurialUnlockedInput> || std::is_same_v<BaseType, MercurialLockedInput>
        || std::is_same_v<BaseType, MercurialFinalInput>) {
        return mercurialInputToAttrs(input);
    } else if constexpr (
        std::is_same_v<BaseType, IndirectUnlockedInput> || std::is_same_v<BaseType, IndirectLockedInput>
        || std::is_same_v<BaseType, IndirectFinalInput>) {
        return indirectInputToAttrs(input);
    } else {
        // This should never happen with our variant
        static_assert(sizeof(BaseType) == 0, "Unhandled typed input type");
    }
}

/**
 * Convert a typed input back to Attrs.
 */
Attrs typedInputToAttrs(const AnyTypedInput & typedInput)
{
    return std::visit([](const auto & input) -> Attrs { return typedInputToAttrsImpl(input); }, typedInput);
}

/**
 * Try to populate an Input's typedInput field from its attrs.
 * Returns true if successful, false if conversion failed or type unknown.
 */
bool tryPopulateTypedInput(Input & input)
{
    if (auto typed = attrsToTypedInput(*input.settings, input.attrs)) {
        input.typedInput = *typed;
        return true;
    }
    return false;
}

} // namespace nix::fetchers
