#include "nix/util/error.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/fetchers/git-typed.hh"
#include "nix/util/users.hh"
#include "nix/fetchers/cache.hh"
#include "nix/store/globals.hh"
#include "nix/util/tarfile.hh"
#include "nix/store/store-api.hh"
#include "nix/util/url-parts.hh"
#include "nix/store/pathlocks.hh"
#include "nix/util/processes.hh"
#include "nix/util/git.hh"
#include "nix/fetchers/git-utils.hh"
#include "nix/util/logging.hh"
#include "nix/util/finally.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/util/json-utils.hh"
#include "nix/util/archive.hh"
#include "nix/util/mounted-source-accessor.hh"

#include <regex>
#include <string.h>
#include <sys/time.h>

#ifndef _WIN32
#  include <sys/wait.h>
#endif

using namespace std::string_literals;

namespace nix::fetchers {

namespace {

// Explicit initial branch of our bare repo to suppress warnings from new version of git.
// The value itself does not matter, since we always fetch a specific revision or branch.
// It is set with `-c init.defaultBranch=` instead of `--initial-branch=` to stay compatible with
// old version of git, which will ignore unrecognized `-c` options.
const std::string gitInitialBranch = "__nix_dummy_branch";

bool isCacheFileWithinTtl(time_t now, const struct stat & st)
{
    return st.st_mtime + static_cast<time_t>(settings.tarballTtl) > now;
}

Path getCachePath(std::string_view key, bool shallow)
{
    return getCacheDir() + "/gitv3/" + hashString(HashAlgorithm::SHA256, key).to_string(HashFormat::Nix32, false)
           + (shallow ? "-shallow" : "");
}

// Returns the name of the HEAD branch.
//
// Returns the head branch name as reported by git ls-remote --symref, e.g., if
// ls-remote returns the output below, "main" is returned based on the ref line.
//
//   ref: refs/heads/main       HEAD
//   ...
std::optional<std::string> readHead(const Path & path)
{
    auto [status, output] = runProgram(
        RunOptions{
            .program = "git",
            // FIXME: use 'HEAD' to avoid returning all refs
            .args = {"ls-remote", "--symref", path},
            .isInteractive = true,
        });
    if (status != 0)
        return std::nullopt;

    std::string_view line = output;
    line = line.substr(0, line.find("\n"));
    if (const auto parseResult = git::parseLsRemoteLine(line); parseResult && parseResult->reference == "HEAD") {
        switch (parseResult->kind) {
        case git::LsRemoteRefLine::Kind::Symbolic:
            debug("resolved HEAD ref '%s' for repo '%s'", parseResult->target, path);
            break;
        case git::LsRemoteRefLine::Kind::Object:
            debug("resolved HEAD rev '%s' for repo '%s'", parseResult->target, path);
            break;
        }
        return parseResult->target;
    }
    return std::nullopt;
}

// Persist the HEAD ref from the remote repo in the local cached repo.
bool storeCachedHead(const std::string & actualUrl, bool shallow, const std::string & headRef)
{
    Path cacheDir = getCachePath(actualUrl, shallow);
    try {
        runProgram("git", true, {"-C", cacheDir, "--git-dir", ".", "symbolic-ref", "--", "HEAD", headRef});
    } catch (ExecError & e) {
        if (
#ifndef WIN32 // TODO abstract over exit status handling on Windows
            !WIFEXITED(e.status)
#else
            e.status != 0
#endif
        )
            throw;

        return false;
    }
    /* No need to touch refs/HEAD, because `git symbolic-ref` updates the mtime. */
    return true;
}

std::optional<std::string> readHeadCached(const std::string & actualUrl, bool shallow)
{
    // Create a cache path to store the branch of the HEAD ref. Append something
    // in front of the URL to prevent collision with the repository itself.
    Path cacheDir = getCachePath(actualUrl, shallow);
    Path headRefFile = cacheDir + "/HEAD";

    time_t now = time(0);
    struct stat st;
    std::optional<std::string> cachedRef;
    if (stat(headRefFile.c_str(), &st) == 0) {
        cachedRef = readHead(cacheDir);
        if (cachedRef != std::nullopt && *cachedRef != gitInitialBranch && isCacheFileWithinTtl(now, st)) {
            debug("using cached HEAD ref '%s' for repo '%s'", *cachedRef, actualUrl);
            return cachedRef;
        }
    }

    auto ref = readHead(actualUrl);
    if (ref)
        return ref;

    if (cachedRef) {
        // If the cached git ref is expired in fetch() below, and the 'git fetch'
        // fails, it falls back to continuing with the most recent version.
        // This function must behave the same way, so we return the expired
        // cached ref here.
        warn("could not get HEAD ref for repository '%s'; using expired cached ref '%s'", actualUrl, *cachedRef);
        return *cachedRef;
    }

    return std::nullopt;
}

std::vector<PublicKey> getPublicKeys(const Attrs & attrs)
{
    std::vector<PublicKey> publicKeys;
    if (attrs.contains("publicKeys")) {
        auto pubKeysJson = nlohmann::json::parse(getStrAttr(attrs, "publicKeys"));
        auto & pubKeys = getArray(pubKeysJson);

        for (auto & key : pubKeys) {
            publicKeys.push_back(key);
        }
    }
    if (attrs.contains("publicKey"))
        publicKeys.push_back(
            PublicKey{maybeGetStrAttr(attrs, "keytype").value_or("ssh-ed25519"), getStrAttr(attrs, "publicKey")});
    return publicKeys;
}

} // end namespace

static const Hash nullRev{HashAlgorithm::SHA1};

struct GitInputScheme : InputScheme
{
    std::optional<Input> inputFromURL(const Settings & settings, const ParsedURL & url, bool requireTree) const override
    {
        if (url.scheme != "git" && parseUrlScheme(url.scheme).application != "git")
            return {};

        auto url2(url);
        url2.query.clear();

        Attrs attrs;
        attrs.emplace("type", "git");

        for (auto & [name, value] : url.query) {
            if (name == "rev" || name == "ref" || name == "keytype" || name == "publicKey" || name == "publicKeys")
                attrs.emplace(name, value);
            else if (
                name == "shallow" || name == "submodules" || name == "lfs" || name == "exportIgnore"
                || name == "allRefs" || name == "verifyCommit")
                attrs.emplace(name, Explicit<bool>{value == "1"});
            else
                url2.query.emplace(name, value);
        }

        attrs.emplace("url", url2.to_string());

        return inputFromAttrs(settings, attrs);
    }

    std::string_view schemeName() const override
    {
        return "git";
    }

    StringSet allowedAttrs() const override
    {
        return {
            "url",
            "ref",
            "rev",
            "shallow",
            "submodules",
            "lfs",
            "exportIgnore",
            "lastModified",
            "revCount",
            "narHash",
            "allRefs",
            "name",
            "dirtyRev",
            "dirtyShortRev",
            "verifyCommit",
            "keytype",
            "publicKey",
            "publicKeys",
        };
    }

    std::optional<Input> inputFromAttrs(const Settings & settings, const Attrs & attrs) const override
    {
        for (auto & [name, _] : attrs)
            if (name == "verifyCommit" || name == "keytype" || name == "publicKey" || name == "publicKeys")
                experimentalFeatureSettings.require(Xp::VerifiedFetches);

        maybeGetBoolAttr(attrs, "verifyCommit");

        if (auto ref = maybeGetStrAttr(attrs, "ref"); ref && !isLegalRefName(*ref))
            throw BadURL("invalid Git branch/tag name '%s'", *ref);

        Input input{settings};
        input.attrs = attrs;
        input.attrs["url"] = fixGitURL(getStrAttr(attrs, "url")).to_string();
        return input;
    }

    ParsedURL toURL(const Input & input) const override
    {
        auto url = parseURL(getStrAttr(input.attrs, "url"));
        if (url.scheme != "git")
            url.scheme = "git+" + url.scheme;
        if (auto rev = input.getRev())
            url.query.insert_or_assign("rev", rev->gitRev());
        if (auto ref = input.getRef())
            url.query.insert_or_assign("ref", *ref);
        if (getShallowAttr(input))
            url.query.insert_or_assign("shallow", "1");
        if (getLfsAttr(input))
            url.query.insert_or_assign("lfs", "1");
        if (getSubmodulesAttr(input))
            url.query.insert_or_assign("submodules", "1");
        if (getExportIgnoreAttr(input))
            url.query.insert_or_assign("exportIgnore", "1");
        if (maybeGetBoolAttr(input.attrs, "verifyCommit").value_or(false))
            url.query.insert_or_assign("verifyCommit", "1");
        auto publicKeys = getPublicKeys(input.attrs);
        if (publicKeys.size() == 1) {
            url.query.insert_or_assign("keytype", publicKeys.at(0).type);
            url.query.insert_or_assign("publicKey", publicKeys.at(0).key);
        } else if (publicKeys.size() > 1)
            url.query.insert_or_assign("publicKeys", publicKeys_to_string(publicKeys));
        return url;
    }

    Input applyOverrides(const Input & input, std::optional<std::string> ref, std::optional<Hash> rev) const override
    {
        auto res(input);
        if (rev)
            res.attrs.insert_or_assign("rev", rev->gitRev());
        if (ref)
            res.attrs.insert_or_assign("ref", *ref);
        if (!res.getRef() && res.getRev())
            throw Error("Git input '%s' has a commit hash but no branch/tag name", res.to_string());
        return res;
    }

    void clone(const Input & input, const Path & destDir) const override
    {
        auto repoInfo = getRepoInfo(input);

        Strings args = {"clone"};

        args.push_back(repoInfo.locationToArg());

        if (auto ref = input.getRef()) {
            args.push_back("--branch");
            args.push_back(*ref);
        }

        if (input.getRev())
            throw UnimplementedError("cloning a specific revision is not implemented");

        args.push_back(destDir);

        runProgram("git", true, args, {}, true);
    }

    std::optional<std::filesystem::path> getSourcePath(const Input & input) const override
    {
        return getRepoInfo(input).getPath();
    }

    void putFile(
        const Input & input,
        const CanonPath & path,
        std::string_view contents,
        std::optional<std::string> commitMsg) const override
    {
        auto repoInfo = getRepoInfo(input);
        auto repoPath = repoInfo.getPath();
        if (!repoPath)
            throw Error(
                "cannot commit '%s' to Git repository '%s' because it's not a working tree", path, input.to_string());

        writeFile(*repoPath / path.rel(), contents);

        auto result = runProgram(
            RunOptions{
                .program = "git",
                .args =
                    {"-C",
                     repoPath->string(),
                     "--git-dir",
                     repoInfo.gitDir,
                     "check-ignore",
                     "--quiet",
                     std::string(path.rel())},
            });
        auto exitCode =
#ifndef WIN32 // TODO abstract over exit status handling on Windows
            WEXITSTATUS(result.first)
#else
            result.first
#endif
            ;

        if (exitCode != 0) {
            // The path is not `.gitignore`d, we can add the file.
            runProgram(
                "git",
                true,
                {"-C",
                 repoPath->string(),
                 "--git-dir",
                 repoInfo.gitDir,
                 "add",
                 "--intent-to-add",
                 "--",
                 std::string(path.rel())});

            if (commitMsg) {
                // Pause the logger to allow for user input (such as a gpg passphrase) in `git commit`
                auto suspension = logger->suspend();
                runProgram(
                    "git",
                    true,
                    {"-C",
                     repoPath->string(),
                     "--git-dir",
                     repoInfo.gitDir,
                     "commit",
                     std::string(path.rel()),
                     "-F",
                     "-"},
                    *commitMsg);
            }
        }
    }

    struct RepoInfo
    {
        /* Either the path of the repo (for local, non-bare repos), or
           the URL (which is never a `file` URL). */
        std::variant<std::filesystem::path, ParsedURL> location;

        /* Working directory info: the complete list of files, and
           whether the working directory is dirty compared to HEAD. */
        GitRepo::WorkdirInfo workdirInfo;

        std::string locationToArg() const
        {
            return std::visit(
                overloaded{
                    [&](const std::filesystem::path & path) { return path.string(); },
                    [&](const ParsedURL & url) { return url.to_string(); }},
                location);
        }

        std::optional<std::filesystem::path> getPath() const
        {
            if (auto path = std::get_if<std::filesystem::path>(&location))
                return *path;
            else
                return std::nullopt;
        }

        void warnDirty(const Settings & settings) const
        {
            if (workdirInfo.isDirty) {
                if (!settings.allowDirty)
                    throw Error("Git tree '%s' is dirty", locationToArg());

                if (settings.warnDirty)
                    warn("Git tree '%s' is dirty", locationToArg());
            }
        }

        std::string gitDir = ".git";
    };

    bool getShallowAttr(const Input & input) const
    {
        return maybeGetBoolAttr(input.attrs, "shallow").value_or(false);
    }

    bool getSubmodulesAttr(const Input & input) const
    {
        return maybeGetBoolAttr(input.attrs, "submodules").value_or(false);
    }

    bool getLfsAttr(const Input & input) const
    {
        return maybeGetBoolAttr(input.attrs, "lfs").value_or(false);
    }

    bool getExportIgnoreAttr(const Input & input) const
    {
        return maybeGetBoolAttr(input.attrs, "exportIgnore").value_or(false);
    }

    bool getAllRefsAttr(const Input & input) const
    {
        return maybeGetBoolAttr(input.attrs, "allRefs").value_or(false);
    }

    /**
     * Typed version: Get repository info from GitUnlockedInput.
     */
    RepoInfo getRepoInfo(const GitUnlockedInput & input) const
    {
        auto checkHashAlgorithm = [&](const std::optional<Hash> & hash) {
            if (hash.has_value() && !(hash->algo == HashAlgorithm::SHA1 || hash->algo == HashAlgorithm::SHA256))
                throw Error(
                    "Hash '%s' is not supported by Git. Supported types are sha1 and sha256.",
                    hash->to_string(HashFormat::Base16, true));
        };

        if (input.rev)
            checkHashAlgorithm(input.rev);

        RepoInfo repoInfo;

        // file:// URIs are normally not cloned (but otherwise treated the
        // same as remote URIs, i.e. we don't use the working tree or
        // HEAD). Exception: If _NIX_FORCE_HTTP is set, or the repo is a bare git
        // repo, treat as a remote URI to force a clone.
        static bool forceHttp = getEnv("_NIX_FORCE_HTTP") == "1"; // for testing
        auto url = input.url;

        // Why are we checking for bare repository?
        // well if it's a bare repository we want to force a git fetch rather than copying the folder
        auto isBareRepository = [](PathView path) { return pathExists(path) && !pathExists(path + "/.git"); };

        // FIXME: here we turn a possibly relative path into an absolute path.
        // This allows relative git flake inputs to be resolved against the
        // **current working directory** (as in POSIX), which tends to work out
        // ok in the context of flakes, but is the wrong behavior,
        // as it should resolve against the flake.nix base directory instead.
        //
        // See: https://discourse.nixos.org/t/57783 and #9708
        //
        if (url.scheme == "file" && !forceHttp && !isBareRepository(renderUrlPathEnsureLegal(url.path))) {
            auto path = renderUrlPathEnsureLegal(url.path);

            if (!isAbsolute(path)) {
                warn(
                    "Fetching Git repository '%s', which uses a path relative to the current directory. "
                    "This is not supported and will stop working in a future release. "
                    "See https://github.com/NixOS/nix/issues/12281 for details.",
                    url);
            }

            // If we don't check here for the path existence, then we can give libgit2 any directory
            // and it will initialize them as git directories.
            if (!pathExists(path)) {
                throw Error("The path '%s' does not exist.", path);
            }
            repoInfo.location = std::filesystem::absolute(path);
        } else {
            if (url.scheme == "file")
                /* Query parameters are meaningless for file://, but
                   Git interprets them as part of the file name. So get
                   rid of them. */
                url.query.clear();
            /* Backward compatibility hack: In old versions of Nix, if you had
               a flake input like

                 inputs.foo.url = "git+https://foo/bar?dir=subdir";

               it would result in a lock file entry like

                 "original": {
                   "dir": "subdir",
                   "type": "git",
                   "url": "https://foo/bar?dir=subdir"
                 }

               New versions of Nix remove `?dir=subdir` from the `url` field,
               since the subdirectory is intended for `FlakeRef`, not the
               fetcher (and specifically the remote server), that is, the
               flakeref is parsed into

                 "original": {
                   "dir": "subdir",
                   "type": "git",
                   "url": "https://foo/bar"
                 }

               However, new versions of nix parsing old flake.lock files would pass the dir=
               query parameter in the "url" attribute to git, which will then complain.

               For this reason, we are filtering the `dir` query parameter from the URL
               before passing it to git. */
            url.query.erase("dir");
            repoInfo.location = url;
        }

        // If this is a local directory and no ref or revision is
        // given, then allow the use of an unclean working tree.
        if (auto repoPath = repoInfo.getPath(); !input.ref && !input.rev && repoPath)
            repoInfo.workdirInfo = GitRepo::getCachedWorkdirInfo(*repoPath);

        return repoInfo;
    }

    /**
     * Legacy version: Get repository info from Input (kept for compatibility).
     * Delegates to typed version.
     */
    RepoInfo getRepoInfo(const Input & input) const
    {
        auto typed = gitInputFromAttrs(*input.settings, input.attrs);
        return getRepoInfo(typed);
    }

    uint64_t getLastModified(
        const Settings & settings,
        const RepoInfo & repoInfo,
        const std::filesystem::path & repoDir,
        const Hash & rev) const
    {
        Cache::Key key{"gitLastModified", {{"rev", rev.gitRev()}}};

        auto cache = settings.getCache();

        if (auto res = cache->lookup(key))
            return getIntAttr(*res, "lastModified");

        auto lastModified = GitRepo::openRepo(repoDir)->getLastModified(rev);

        cache->upsert(key, {{"lastModified", lastModified}});

        return lastModified;
    }

    uint64_t getRevCount(
        const Settings & settings,
        const RepoInfo & repoInfo,
        const std::filesystem::path & repoDir,
        const Hash & rev) const
    {
        Cache::Key key{"gitRevCount", {{"rev", rev.gitRev()}}};

        auto cache = settings.getCache();

        if (auto revCountAttrs = cache->lookup(key))
            return getIntAttr(*revCountAttrs, "revCount");

        Activity act(
            *logger, lvlChatty, actUnknown, fmt("getting Git revision count of '%s'", repoInfo.locationToArg()));

        auto revCount = GitRepo::openRepo(repoDir)->getRevCount(rev);

        cache->upsert(key, Attrs{{"revCount", revCount}});

        return revCount;
    }

    std::string getDefaultRef(const RepoInfo & repoInfo, bool shallow) const
    {
        auto head = std::visit(
            overloaded{
                [&](const std::filesystem::path & path) { return GitRepo::openRepo(path)->getWorkdirRef(); },
                [&](const ParsedURL & url) { return readHeadCached(url.to_string(), shallow); }},
            repoInfo.location);
        if (!head) {
            warn("could not read HEAD ref from repo at '%s', using 'master'", repoInfo.locationToArg());
            return "master";
        }
        return *head;
    }

    static MakeNotAllowedError makeNotAllowedError(std::filesystem::path repoPath)
    {
        return [repoPath{std::move(repoPath)}](const CanonPath & path) -> RestrictedPathError {
            if (pathExists(repoPath / path.rel()))
                return RestrictedPathError(
                    "Path '%1%' in the repository %2% is not tracked by Git.\n"
                    "\n"
                    "To make it visible to Nix, run:\n"
                    "\n"
                    "git -C %2% add \"%1%\"",
                    path.rel(),
                    repoPath);
            else
                return RestrictedPathError("Path '%s' does not exist in Git repository %s.", path.rel(), repoPath);
        };
    }

    /**
     * Typed version: Verify commit from GitUnlockedInput.
     * Takes the resolved rev as a separate parameter since input.rev may be unset.
     */
    void verifyCommit(
        const GitUnlockedInput & input,
        std::shared_ptr<GitRepo> repo,
        const std::optional<Hash> & rev,
        const std::string & displayStr) const
    {
        std::vector<fetchers::PublicKey> publicKeys;
        if (input.publicKey)
            publicKeys.push_back({.type = input.keytype.value_or("ssh-ed25519"), .key = *input.publicKey});
        if (input.publicKeys)
            for (auto & key : *input.publicKeys)
                publicKeys.push_back(key);

        auto verifyCommit = input.verifyCommit.value_or(!publicKeys.empty());

        if (verifyCommit) {
            if (rev && repo)
                repo->verifyCommit(*rev, publicKeys);
            else
                throw Error("commit verification is required for Git repository '%s', but it's dirty", displayStr);
        }
    }

    /**
     * Legacy version: delegates to typed version.
     */
    void verifyCommit(const Input & input, std::shared_ptr<GitRepo> repo) const
    {
        auto typed = gitInputFromAttrs(*input.settings, input.attrs);
        verifyCommit(typed, repo, input.getRev(), input.to_string());
    }

    /**
     * Typed version: Get accessor from a specific commit.
     * Returns GitLockedInput with resolved ref and rev.
     */
    std::pair<ref<SourceAccessor>, GitLockedInput>
    getAccessorFromCommit(ref<Store> store, RepoInfo & repoInfo, const GitUnlockedInput & input) const
    {
        assert(!repoInfo.workdirInfo.isDirty);

        auto origRev = input.rev;

        // Determine ref (use provided ref or get default)
        auto ref = input.ref ? *input.ref : getDefaultRef(repoInfo, input.shallow);

        std::filesystem::path repoDir;
        Hash resolvedRev = origRev.value_or(Hash(HashAlgorithm::SHA1)); // Will be set below

        if (auto repoPath = repoInfo.getPath()) {
            repoDir = *repoPath;
            if (!input.rev)
                resolvedRev = GitRepo::openRepo(repoDir)->resolveRef(ref);
            else
                resolvedRev = *input.rev;
        } else {
            auto repoUrl = std::get<ParsedURL>(repoInfo.location);
            std::filesystem::path cacheDir = getCachePath(repoUrl.to_string(), input.shallow);
            repoDir = cacheDir;
            repoInfo.gitDir = ".";

            std::filesystem::create_directories(cacheDir.parent_path());
            PathLocks cacheDirLock({cacheDir.string()});

            auto repo = GitRepo::openRepo(cacheDir, true, true);

            // We need to set the origin so resolving submodule URLs works
            repo->setRemote("origin", repoUrl.to_string());

            auto localRefFile = ref.compare(0, 5, "refs/") == 0 ? cacheDir / ref : cacheDir / "refs/heads" / ref;

            bool doFetch = false;
            time_t now = time(0);

            /* If a rev was specified, we need to fetch if it's not in the repo. */
            if (input.rev) {
                doFetch = !repo->hasObject(*input.rev);
                resolvedRev = *input.rev;
            } else {
                if (input.allRefs) {
                    doFetch = true;
                } else {
                    /* If the local ref is older than 'tarball-ttl' seconds, do a
                       git fetch to update the local ref to the remote ref. */
                    struct stat st;
                    doFetch = stat(localRefFile.string().c_str(), &st) != 0 || !isCacheFileWithinTtl(now, st);
                }
            }

            if (doFetch) {
                try {
                    auto fetchRef = input.allRefs                     ? "refs/*:refs/*"
                                    : input.rev                       ? input.rev->gitRev()
                                    : ref.compare(0, 5, "refs/") == 0 ? fmt("%1%:%1%", ref)
                                    : ref == "HEAD"                   ? ref
                                                                      : fmt("%1%:%1%", "refs/heads/" + ref);

                    repo->fetch(repoUrl.to_string(), fetchRef, input.shallow);
                } catch (Error & e) {
                    if (!std::filesystem::exists(localRefFile))
                        throw;
                    logError(e.info());
                    warn(
                        "could not update local clone of Git repository '%s'; continuing with the most recent version",
                        repoInfo.locationToArg());
                }

                try {
                    if (!input.rev)
                        setWriteTime(localRefFile, now, now);
                } catch (Error & e) {
                    warn("could not update mtime for file %s: %s", localRefFile, e.info().msg);
                }
                if (!input.ref && !storeCachedHead(repoUrl.to_string(), input.shallow, ref))
                    warn("could not update cached head '%s' for '%s'", ref, repoInfo.locationToArg());
            }

            if (input.rev) {
                if (!repo->hasObject(*input.rev))
                    throw Error(
                        "Cannot find Git revision '%s' in ref '%s' of repository '%s'! "
                        "Please make sure that the " ANSI_BOLD "rev" ANSI_NORMAL " exists on the " ANSI_BOLD
                        "ref" ANSI_NORMAL " you've specified or add " ANSI_BOLD "allRefs = true;" ANSI_NORMAL
                        " to " ANSI_BOLD "fetchGit" ANSI_NORMAL ".",
                        input.rev->gitRev(),
                        ref,
                        repoInfo.locationToArg());
            } else {
                resolvedRev = repo->resolveRef(ref);
            }

            // cache dir lock is removed at scope end; we will only use read-only operations on specific revisions in
            // the remainder
        }

        auto repo = GitRepo::openRepo(repoDir);

        auto isShallow = repo->isShallow();

        if (isShallow && !input.shallow)
            throw Error(
                "'%s' is a shallow Git repository, but shallow repositories are only allowed when `shallow = true;` is specified",
                repoInfo.locationToArg());

        // FIXME: check whether rev is an ancestor of ref?

        auto lastModified = getLastModified(*input.settings, repoInfo, repoDir, resolvedRev);
        std::optional<uint64_t> revCount;
        if (!input.shallow)
            revCount = getRevCount(*input.settings, repoInfo, repoDir, resolvedRev);

        printTalkative("using revision %s of repo '%s'", resolvedRev.gitRev(), repoInfo.locationToArg());

        // Build display string with ref and rev parameters
        auto displayUrl = input.url;
        if (input.ref)
            displayUrl.query.insert_or_assign("ref", *input.ref);
        displayUrl.query.insert_or_assign("rev", resolvedRev.gitRev());
        auto displayStr = "git+" + displayUrl.to_string();
        verifyCommit(input, repo, resolvedRev, displayStr);

        auto accessor = repo->getAccessor(resolvedRev, input.exportIgnore, "«" + displayStr + "»", input.lfs);

        /* If the repo has submodules, fetch them and return a mounted
           input accessor consisting of the accessor for the top-level
           repo and the accessors for the submodules. */
        if (input.submodules) {
            std::map<CanonPath, nix::ref<SourceAccessor>> mounts;

            for (auto & [submodule, submoduleRev] : repo->getSubmodules(resolvedRev, input.exportIgnore)) {
                auto resolved = repo->resolveSubmoduleUrl(submodule.url);
                debug(
                    "Git submodule %s: %s %s %s -> %s",
                    submodule.path,
                    submodule.url,
                    submodule.branch,
                    submoduleRev.gitRev(),
                    resolved);

                // Create typed submodule input
                // If the resolved URL doesn't have a scheme, it's a local path - prepend file://
                std::string resolvedUrlStr = resolved;
                if (resolvedUrlStr.find("://") == std::string::npos) {
                    resolvedUrlStr = "file://" + resolved;
                }
                auto resolvedUrl = parseURL(resolvedUrlStr);

                // Determine ref for submodule
                std::optional<std::string> submoduleRef;
                if (submodule.branch != "") {
                    // A special value of . is used to indicate that the name of the branch in the submodule
                    // should be the same name as the current branch in the current repository.
                    // https://git-scm.com/docs/gitmodules
                    if (submodule.branch == ".") {
                        submoduleRef = ref;
                    } else {
                        submoduleRef = submodule.branch;
                    }
                }

                // Create GitUnlockedInput for submodule
                GitUnlockedInput submoduleInput(
                    *input.settings,
                    std::move(resolvedUrl),
                    submoduleRef,
                    submoduleRev,       // rev is known
                    false,              // shallow
                    true,               // submodules (recursive)
                    input.lfs,          // lfs
                    input.exportIgnore, // exportIgnore
                    true                // allRefs
                );

                // Recursively lock the submodule (typed!)
                auto [submoduleAccessor, submoduleLocked] =
                    lockTyped(store, submoduleInput, std::make_shared<GitInputScheme>());

                // Build display string from locked input
                auto displayUrl = submoduleLocked.url;
                if (submoduleLocked.ref)
                    displayUrl.query.insert_or_assign("ref", *submoduleLocked.ref);
                if (submoduleLocked.rev)
                    displayUrl.query.insert_or_assign("rev", submoduleLocked.rev->gitRev());
                submoduleAccessor->setPathDisplay("«git+" + displayUrl.to_string() + "»");

                mounts.insert_or_assign(submodule.path, submoduleAccessor);
            }

            if (!mounts.empty()) {
                mounts.insert_or_assign(CanonPath::root, accessor);
                accessor = makeMountedSourceAccessor(std::move(mounts));
            }
        }

        assert(!origRev || origRev == resolvedRev);

        // Build the locked input
        GitLockedInput locked(
            *input.settings,
            input.url,
            resolvedRev,
            LockingMetadata(lastModified),
            ref,
            revCount,
            input.shallow,
            input.submodules,
            input.lfs,
            input.exportIgnore,
            input.allRefs,
            input.name);

        // Copy verified fetches attributes
        locked.verifyCommit = input.verifyCommit;
        locked.keytype = input.keytype;
        locked.publicKey = input.publicKey;
        locked.publicKeys = input.publicKeys;

        return {accessor, std::move(locked)};
    }

    /**
     * Typed version: Get accessor from working directory.
     * Returns GitLockedInput, possibly with dirty state.
     */
    std::pair<ref<SourceAccessor>, GitLockedInput>
    getAccessorFromWorkdir(ref<Store> store, RepoInfo & repoInfo, const GitUnlockedInput & input) const
    {
        auto repoPath = repoInfo.getPath().value();

        if (input.submodules)
            /* Create mountpoints for the submodules. */
            for (auto & submodule : repoInfo.workdirInfo.submodules)
                repoInfo.workdirInfo.files.insert(submodule.path);

        auto repo = GitRepo::openRepo(repoPath, false, false);

        ref<SourceAccessor> accessor =
            repo->getAccessor(repoInfo.workdirInfo, input.exportIgnore, makeNotAllowedError(repoPath));

        /* If the repo has submodules, return a mounted input accessor
           consisting of the accessor for the top-level repo and the
           accessors for the submodule workdirs. */
        if (input.submodules && !repoInfo.workdirInfo.submodules.empty()) {
            std::map<CanonPath, nix::ref<SourceAccessor>> mounts;

            for (auto & submodule : repoInfo.workdirInfo.submodules) {
                auto submodulePath = repoPath / submodule.path.rel();
                fetchers::Attrs attrs;
                attrs.insert_or_assign("type", "git");
                attrs.insert_or_assign("url", submodulePath.string());
                attrs.insert_or_assign("exportIgnore", Explicit<bool>{input.exportIgnore});
                attrs.insert_or_assign("submodules", Explicit<bool>{true});
                // TODO: fall back to getAccessorFromCommit-like fetch when submodules aren't checked out
                // attrs.insert_or_assign("allRefs", Explicit<bool>{ true });

                auto submoduleInput = fetchers::Input::fromAttrs(*input.settings, std::move(attrs));
                auto [submoduleAccessor, submoduleInput2] = submoduleInput.getAccessor(store);
                submoduleAccessor->setPathDisplay("«" + submoduleInput.to_string() + "»");

                /* If the submodule is dirty, mark this repo dirty as well. */
                if (!submoduleInput2.getRev())
                    repoInfo.workdirInfo.isDirty = true;

                mounts.insert_or_assign(submodule.path, submoduleAccessor);
            }

            mounts.insert_or_assign(CanonPath::root, accessor);
            accessor = makeMountedSourceAccessor(std::move(mounts));
        }

        // Build locked input fields
        std::optional<std::string> ref;
        std::optional<Hash> rev;
        std::optional<uint64_t> revCount;
        std::optional<std::string> dirtyRev;
        std::optional<std::string> dirtyShortRev;
        uint64_t lastModified;

        if (!repoInfo.workdirInfo.isDirty) {
            auto repo = GitRepo::openRepo(repoPath);

            if (auto workdirRef = repo->getWorkdirRef())
                ref = *workdirRef;

            /* Return a rev of 000... if there are no commits yet. */
            auto headRev = repoInfo.workdirInfo.headRev.value_or(nullRev);
            rev = headRev;

            if (!input.shallow) {
                revCount = headRev == nullRev ? 0 : getRevCount(*input.settings, repoInfo, repoPath, headRev);
            }

            auto displayStr = "git+" + input.url.to_string();
            verifyCommit(input, repo, headRev, displayStr);

            lastModified = repoInfo.workdirInfo.headRev
                               ? getLastModified(*input.settings, repoInfo, repoPath, *repoInfo.workdirInfo.headRev)
                               : 0;
        } else {
            repoInfo.warnDirty(*input.settings);

            if (repoInfo.workdirInfo.headRev) {
                dirtyRev = repoInfo.workdirInfo.headRev->gitRev() + "-dirty";
                dirtyShortRev = repoInfo.workdirInfo.headRev->gitShortRev() + "-dirty";
            }

            auto displayStr = "git+" + input.url.to_string();
            verifyCommit(input, nullptr, repoInfo.workdirInfo.headRev, displayStr);

            lastModified = repoInfo.workdirInfo.headRev
                               ? getLastModified(*input.settings, repoInfo, repoPath, *repoInfo.workdirInfo.headRev)
                               : 0;
        }

        // Build the locked input
        GitLockedInput locked(
            *input.settings,
            input.url,
            rev, // May be absent for dirty workdirs
            LockingMetadata(lastModified),
            ref,
            revCount,
            input.shallow,
            input.submodules,
            input.lfs,
            input.exportIgnore,
            input.allRefs,
            input.name);

        // Set dirty state if present
        locked.dirtyRev = dirtyRev;
        locked.dirtyShortRev = dirtyShortRev;

        // Copy verified fetches attributes
        locked.verifyCommit = input.verifyCommit;
        locked.keytype = input.keytype;
        locked.publicKey = input.publicKey;
        locked.publicKeys = input.publicKeys;

        return {accessor, std::move(locked)};
    }

    /**
     * Typed method: Lock a GitUnlockedInput to a GitLockedInput.
     * This is the primary implementation using typed inputs.
     */
    std::pair<ref<SourceAccessor>, GitLockedInput>
    lockTyped(ref<Store> store, const GitUnlockedInput & input, std::shared_ptr<InputScheme> scheme) const
    {
        // Use typed getRepoInfo (no conversion needed!)
        auto repoInfo = getRepoInfo(input);

        if (input.exportIgnore && input.submodules) {
            throw UnimplementedError("exportIgnore and submodules are not supported together yet");
        }

        // Decide which path to take: commit or workdir
        bool useCommitPath = input.ref || input.rev || !repoInfo.getPath();

        if (useCommitPath) {
            // Use typed commit path - no conversions!
            return getAccessorFromCommit(store, repoInfo, input);
        } else {
            // Use typed workdir path - no conversions!
            return getAccessorFromWorkdir(store, repoInfo, input);
        }
    }

    /**
     * Wrapper method for backward compatibility with Input/Attrs API.
     * Delegates to typed lockTyped() method.
     */
    std::pair<ref<SourceAccessor>, Input> getAccessor(ref<Store> store, const Input & input) const override
    {
        // Boundary conversion: Input (Attrs) → GitUnlockedInput (typed)
        auto unlocked = gitInputFromAttrs(*input.settings, input.attrs);

        // Delegate to typed method (pure typed logic, no Attrs!)
        auto [accessor, locked] = lockTyped(store, unlocked, input.scheme);

        // Boundary conversion: GitLockedInput (typed) → Input (Attrs)
        Input result(input); // Copy to preserve scheme and other fields
        result.attrs = gitInputToAttrs(locked);

        // Preserve final-state attrs if they were in the original input
        if (auto narHash = input.getNarHash())
            result.attrs.insert_or_assign("narHash", narHash->to_string(HashFormat::SRI, true));
        if (input.isFinal())
            result.attrs.insert_or_assign("__final", Explicit<bool>(true));

        return {accessor, std::move(result)};
    }

    std::optional<std::string> getFingerprint(ref<Store> store, const Input & input) const override
    {
        auto makeFingerprint = [&](const Hash & rev) {
            return rev.gitRev() + (getSubmodulesAttr(input) ? ";s" : "") + (getExportIgnoreAttr(input) ? ";e" : "")
                   + (getLfsAttr(input) ? ";l" : "");
        };

        if (auto rev = input.getRev())
            return makeFingerprint(*rev);
        else {
            auto repoInfo = getRepoInfo(input);
            if (auto repoPath = repoInfo.getPath(); repoPath && repoInfo.workdirInfo.submodules.empty()) {
                /* Calculate a fingerprint that takes into account the
                   deleted and modified/added files. */
                HashSink hashSink{HashAlgorithm::SHA512};
                for (auto & file : repoInfo.workdirInfo.dirtyFiles) {
                    writeString("modified:", hashSink);
                    writeString(file.abs(), hashSink);
                    dumpPath((*repoPath / file.rel()).string(), hashSink);
                }
                for (auto & file : repoInfo.workdirInfo.deletedFiles) {
                    writeString("deleted:", hashSink);
                    writeString(file.abs(), hashSink);
                }
                return makeFingerprint(repoInfo.workdirInfo.headRev.value_or(nullRev))
                       + ";d=" + hashSink.finish().hash.to_string(HashFormat::Base16, false);
            }
            return std::nullopt;
        }
    }

    bool isLocked(const Input & input) const override
    {
        auto rev = input.getRev();
        return rev && rev != nullRev;
    }
};

static auto rGitInputScheme = OnStartup([] { registerInputScheme(std::make_unique<GitInputScheme>()); });

} // namespace nix::fetchers
