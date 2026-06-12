#include "nix/fetchers/fetch-settings.hh"
#include "nix/fetchers/attrs.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/util/tests/gmock-matchers.hh"
#include "nix/util/url.hh"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <string>

namespace nix {

using fetchers::Attr;

struct InputFromAttrsTestCase
{
    fetchers::Attrs attrs;
    std::string expectedUrl;
    std::string description;
    fetchers::Attrs expectedAttrs = attrs;
};

class InputFromAttrsTest : public ::testing::WithParamInterface<InputFromAttrsTestCase>, public ::testing::Test
{};

TEST_P(InputFromAttrsTest, attrsAreCorrectAndRoundTrips)
{
    fetchers::Settings fetchSettings;

    const auto & testCase = GetParam();

    auto input = fetchers::Input::fromAttrs(fetchSettings, fetchers::Attrs(testCase.attrs));

    EXPECT_EQ(input.toAttrs(), testCase.expectedAttrs);
    EXPECT_EQ(input.toURLString(), testCase.expectedUrl);

    auto input2 = fetchers::Input::fromAttrs(fetchSettings, input.toAttrs());
    EXPECT_EQ(input, input2);
    EXPECT_EQ(input.toAttrs(), input2.toAttrs());
}

INSTANTIATE_TEST_SUITE_P(
    InputFromAttrs,
    InputFromAttrsTest,
    ::testing::Values(
        // Test for issue #14429.
        InputFromAttrsTestCase{
            .attrs =
                {
                    {"url", Attr("git+ssh://git@github.com/NixOS/nixpkgs")},
                    {"type", Attr("git")},
                },
            .expectedUrl = "git+ssh://git@github.com/NixOS/nixpkgs",
            .description = "strips_git_plus_prefix",
            .expectedAttrs =
                {
                    {"url", Attr("ssh://git@github.com/NixOS/nixpkgs")},
                    {"type", Attr("git")},
                },
        }),
    [](const ::testing::TestParamInfo<InputFromAttrsTestCase> & info) { return info.param.description; });

/* `Input::toUnpinnedURL` returns the URL of an Input with
   version-resolution-output attributes stripped (`rev`, `revCount`,
   `narHash`, `lastModified`, `__final`), so that two Inputs that
   resolve to the same source-of-truth share a URL even if one is
   locked and the other isn't. Used as the SourceRoot identifier so
   the eval cache can hit across versions of the same source. */
struct InputToUnpinnedURLTestCase
{
    fetchers::Attrs attrs;
    std::string expectedUnpinnedURL;
    std::string description;
};

class InputToUnpinnedURLTest : public ::testing::WithParamInterface<InputToUnpinnedURLTestCase>, public ::testing::Test
{};

TEST_P(InputToUnpinnedURLTest, stripsVersionFields)
{
    fetchers::Settings fetchSettings;
    const auto & tc = GetParam();
    auto input = fetchers::Input::fromAttrs(fetchSettings, fetchers::Attrs(tc.attrs));
    EXPECT_EQ(input.toUnpinnedURL(), tc.expectedUnpinnedURL);
}

INSTANTIATE_TEST_SUITE_P(
    InputToUnpinnedURL,
    InputToUnpinnedURLTest,
    ::testing::Values(
        InputToUnpinnedURLTestCase{
            .attrs =
                {
                    {"type", Attr("github")},
                    {"owner", Attr("NixOS")},
                    {"repo", Attr("nixpkgs")},
                    {"rev", Attr("abcdef0123456789abcdef0123456789abcdef01")},
                },
            .expectedUnpinnedURL = "github:NixOS/nixpkgs",
            .description = "github_strips_rev",
        },
        InputToUnpinnedURLTestCase{
            /* `ref` identifies the specification (which branch to track),
               not the resolution output. Strip `rev`, keep `ref`. */
            .attrs =
                {
                    {"type", Attr("git")},
                    {"url", Attr("https://example.com/foo")},
                    {"ref", Attr("nixos-25.05")},
                    {"rev", Attr("abcdef0123456789abcdef0123456789abcdef01")},
                },
            .expectedUnpinnedURL = "git+https://example.com/foo?ref=nixos-25.05",
            .description = "git_strips_rev_keeps_ref",
        },
        InputToUnpinnedURLTestCase{
            .attrs =
                {
                    {"type", Attr("git")},
                    {"url", Attr("https://example.com/foo")},
                    {"narHash", Attr("sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=")},
                },
            .expectedUnpinnedURL = "git+https://example.com/foo",
            .description = "git_strips_narHash",
        },
        InputToUnpinnedURLTestCase{
            .attrs =
                {
                    {"type", Attr("git")},
                    {"url", Attr("https://example.com/foo")},
                    {"lastModified", Attr(static_cast<uint64_t>(1700000000))},
                },
            .expectedUnpinnedURL = "git+https://example.com/foo",
            .description = "git_strips_lastModified",
        },
        InputToUnpinnedURLTestCase{
            .attrs =
                {
                    {"type", Attr("git")},
                    {"url", Attr("https://example.com/foo")},
                    {"rev", Attr("abcdef0123456789abcdef0123456789abcdef01")},
                    {"revCount", Attr(static_cast<uint64_t>(42))},
                },
            .expectedUnpinnedURL = "git+https://example.com/foo",
            .description = "git_strips_revCount",
        },
        InputToUnpinnedURLTestCase{
            .attrs =
                {
                    {"type", Attr("git")},
                    {"url", Attr("https://example.com/foo")},
                    {"narHash", Attr("sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=")},
                    {"__final", Attr(Explicit<bool>{true})},
                },
            .expectedUnpinnedURL = "git+https://example.com/foo",
            .description = "git_strips_final_marker",
        },
        InputToUnpinnedURLTestCase{
            .attrs =
                {
                    {"type", Attr("path")},
                    {"path", Attr("/some/where")},
                },
            .expectedUnpinnedURL = "path:/some/where",
            .description = "path_identity_no_version_fields",
        }),
    [](const ::testing::TestParamInfo<InputToUnpinnedURLTestCase> & info) { return info.param.description; });

namespace fetchers {

class GitHubInputTest : public ::testing::Test
{};

TEST_F(GitHubInputTest, throwOnInvalidURLParam)
{
    EXPECT_THAT(
        []() { Input::fromURL(fetchers::Settings{}, "github:a/b?tag=foo"); },
        ::testing::ThrowsMessage<BadURL>(testing::HasSubstrIgnoreANSIMatcher("tag")));
}

} // namespace fetchers

} // namespace nix
