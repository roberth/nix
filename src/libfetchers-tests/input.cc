#include "nix/fetchers/fetch-settings.hh"
#include "nix/fetchers/attrs.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/store/tests/libstore.hh"
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
    bool needsFlakes = false;
};

class InputFromAttrsTest : public ::testing::WithParamInterface<InputFromAttrsTestCase>, public ::testing::Test
{};

TEST_P(InputFromAttrsTest, attrsAreCorrectAndRoundTrips)
{
    std::optional<EnableExperimentalFeature> enableFlakesMaybe;
    auto & testCase = GetParam();
    if (testCase.needsFlakes)
        enableFlakesMaybe.emplace("flakes");

    auto input = fetchers::Input::fromAttrs(fetchers::Attrs(testCase.attrs));

    EXPECT_EQ(input.toAttrs(), testCase.expectedAttrs);
    EXPECT_EQ(input.toURLString(), testCase.expectedUrl);

    auto input2 = fetchers::Input::fromAttrs(input.toAttrs());
    EXPECT_EQ(input, input2);
    EXPECT_EQ(input.toAttrs(), input2.toAttrs());

    auto url = input.toURL();
    auto inputFromUrl = fetchers::Input::fromURL(url);

#if 0
    // Roundtripping doesn't hold (and never can) because there are multiple
    // equivalent representations. At least the idempotency holds (next assertions).
    EXPECT_EQ(inputFromUrl.toAttrs(), input.toAttrs());
#endif
    EXPECT_EQ(inputFromUrl.toURL(), url);
    EXPECT_EQ(fetchers::Input::fromAttrs(inputFromUrl.toAttrs()).toAttrs(), inputFromUrl.toAttrs());

    enableFlakesMaybe.reset();
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
        },
        InputFromAttrsTestCase{
            .attrs =
                {
                    {"url", Attr("git://user@github.com/NixOS/nixpkgs")},
                    {"type", Attr("git")},
                    {"rev", Attr("sha1-AAAAAAAAAAAAAAAAAAAAAAAAAAA=")},
                },
            .expectedUrl = "git://user@github.com/NixOS/nixpkgs?rev=0000000000000000000000000000000000000000",
            .description = "git_rev_in_base64_sri_sha1",
            .expectedAttrs =
                {
                    {"url", Attr("git://user@github.com/NixOS/nixpkgs")},
                    {"type", Attr("git")},
                    {"rev", Attr("sha1-AAAAAAAAAAAAAAAAAAAAAAAAAAA=")},
                },
        },
        InputFromAttrsTestCase{
            .attrs =
                {
                    {"url", Attr("git+https://user@github.com/NixOS/nix.git")},
                    {"type", Attr("git")},
                    // Same as above, just unprefixed (defaults to sha1)
                    {"rev", Attr("AAAAAAAAAAAAAAAAAAAAAAAAAAA=")},
                },
            .expectedUrl = "git+https://user@github.com/NixOS/nix.git?rev=0000000000000000000000000000000000000000",
            .description = "git_rev_in_base64_implicitly_sha1",
            .expectedAttrs =
                {
                    {"url", Attr("https://user@github.com/NixOS/nix.git")},
                    {"type", Attr("git")},
                    {"rev", Attr("AAAAAAAAAAAAAAAAAAAAAAAAAAA=")},
                },
        },
        InputFromAttrsTestCase{
            .attrs =
                {
                    {"url", Attr("git+something://user@github.com/NixOS/nix.git")},
                    {"type", Attr("git")},
                    // Plain base16, just prefixed.
                    {"rev", Attr("sha1:0000000000000000000000000000000000000000")},
                },
            .expectedUrl = "git+something://user@github.com/NixOS/nix.git?rev=0000000000000000000000000000000000000000",
            .description = "git_rev_in_base16_explicit_sha1",
            .expectedAttrs =
                {
                    // Remote helpers can be specified this way (a.k.a custom schemes).
                    {"url", Attr("something://user@github.com/NixOS/nix.git")},
                    {"type", Attr("git")},
                    {"rev", Attr("sha1:0000000000000000000000000000000000000000")},
                },
        },
        InputFromAttrsTestCase{
            .attrs =
                {
                    {"url", Attr("git+http://somewhere/some/repo/path%2Fwith%2Fspaces")},
                    {"type", Attr("git")},
                    // Yes, also can in nix32 (inexplicably).
                    {"rev", Attr("89144hj289144hj289144hj289144hj2")},
                },
            .expectedUrl =
                "git+http://somewhere/some/repo/path%2Fwith%2Fspaces?rev=4242424242424242424242424242424242424242",
            .description = "git_rev_in_nix32_implicitly_sha1",
            .expectedAttrs =
                {
                    // Remote helpers can be specified this way (a.k.a custom schemes).
                    {"url", Attr("http://somewhere/some/repo/path%2Fwith%2Fspaces")},
                    {"type", Attr("git")},
                    {"rev", Attr("89144hj289144hj289144hj289144hj2")},
                },
        },
        InputFromAttrsTestCase{
            .attrs =
                {
                    {"url", Attr("http://somewhere?something=abcd#a")},
                    {"type", Attr("git")},
                    // Yes, also can in nix32 (inexplicably).
                    {"rev", Attr("sha1:89144hj289144hj289144hj289144hj2")},
                },
            // Query parameters are merged and fragments are accepted too?
            .expectedUrl = "git+http://somewhere?rev=4242424242424242424242424242424242424242&something=abcd#a",
            .description = "git_rev_in_nix32_explicit_sha1",
            .expectedAttrs =
                {
                    {"url", Attr("http://somewhere?something=abcd#a")},
                    {"type", Attr("git")},
                    {"rev", Attr("sha1:89144hj289144hj289144hj289144hj2")},
                },
        },
        InputFromAttrsTestCase{
            .attrs =
                {
                    {"url", Attr("proprietaryhelper:///something///else///?hello=goodbye&test=%2F%3f")},
                    {"type", Attr("git")},
                    // And again, but now in base64.
                    {"rev", Attr("sha1:QkJCQkJCQkJCQkJCQkJCQkJCQkI=")},
                },
            /// XXX: Is it ok that the query parameter loses pct-encoding there? Seems very sketchy.
            .expectedUrl =
                "git+proprietaryhelper:///something///else///?hello=goodbye&rev=4242424242424242424242424242424242424242&test=/?",
            .description = "git_rev_in_base64_explicit_sha1",
            .expectedAttrs =
                {
                    {"url", Attr("proprietaryhelper:///something///else///?hello=goodbye&test=/?")},
                    {"type", Attr("git")},
                    {"rev", Attr("sha1:QkJCQkJCQkJCQkJCQkJCQkJCQkI=")},
                },
        },
        InputFromAttrsTestCase{
            .attrs =
                {
                    {"type", Attr("indirect")},
                    {"id", Attr("some_Id_-1234567890")},
                    // And again, but now in base64.
                    {"ref", Attr("sOmE/BRANCH%2f/definitely/Not1/HEAD")},
                    {"rev", Attr("sha1-QkJCQkJCQkJCQkJCQkJCQkJCQkI====aaaaaaaaa even more trailing%2Fgarbage")},
                },
            // As we can see, garbage in the hash is ignored.
            .expectedUrl =
                "flake:some_Id_-1234567890/sOmE%2FBRANCH%252f%2Fdefinitely%2FNot1%2FHEAD/4242424242424242424242424242424242424242",
            .description = "indirect_sri_sha1_trailing_garbage",
            .expectedAttrs =
                {
                    {"type", Attr("indirect")},
                    {"id", Attr("some_Id_-1234567890")},
                    {"ref", Attr("sOmE/BRANCH%2f/definitely/Not1/HEAD")},
                    {"rev", Attr("sha1-QkJCQkJCQkJCQkJCQkJCQkJCQkI====aaaaaaaaa even more trailing%2Fgarbage")},
                },
            .needsFlakes = true,
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
    auto input = fetchers::Input::fromAttrs(fetchers::Attrs(tc.attrs));
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
        []() { Input::fromURL("github:a/b?tag=foo"); },
        ::testing::ThrowsMessage<BadURL>(testing::HasSubstrIgnoreANSIMatcher("tag")));
}

} // namespace fetchers

} // namespace nix
