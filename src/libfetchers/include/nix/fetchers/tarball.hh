#pragma once

#include <optional>

#include "nix/util/hash.hh"
#include "nix/store/filetransfer.hh"
#include "nix/store/path.hh"
#include "nix/util/ref.hh"
#include "nix/util/types.hh"
#include "nix/util/url.hh"

namespace nix {
class Store;
struct SourceAccessor;
} // namespace nix

namespace nix::fetchers {

struct Settings;

struct DownloadFileResult
{
    StorePath storePath;
    std::string etag;
    std::string effectiveUrl;
    std::optional<std::string> immutableUrl;
};

DownloadFileResult downloadFile(
    Store & store,
    const Settings & settings,
    const VerbatimURL & url,
    const std::string & name,
    const Headers & headers = {});

struct DownloadTarballResult
{
    Hash treeHash;
    time_t lastModified;
    std::optional<std::string> immutableUrl;
    ref<SourceAccessor> accessor;
};

/**
 * Download and import a tarball into the Git cache. The result is the
 * Git tree hash of the root directory.
 */
ref<SourceAccessor> downloadTarball(Store & store, const Settings & settings, const std::string & url);

struct UnpackedDownload
{
    /**
     * The root of the unpacked tree, as the archive has it: a single
     * top-level directory is not removed.
     */
    Hash treeHash;

    time_t lastModified;

    FileTransferResult transfer;
};

/**
 * Download an archive and unpack it into the tarball cache, streaming:
 * unpacking starts with the first bytes of the download.
 *
 * With the gitoxide tarball cache, this runs a task: the download and the
 * unpacking run concurrently, and a user interrupt cancels both and leaves
 * the cache as it was. See `doc/manual/source/development/tarball-cache.md`.
 *
 * @param isZip Whether the archive is a zip file, which libarchive must
 * read from a file on disk instead of a stream.
 *
 * @param description What the progress message calls the archive.
 */
UnpackedDownload downloadIntoTarballCache(
    const Settings & settings, FileTransferRequest request, bool isZip, const std::string & description);

/**
 * `downloadIntoTarballCache()` with the gitoxide tarball cache, as a task,
 * so that a caller can fetch many archives at once, for example with
 * `whenAll`. Run it with `runOnReactor()` when `fileTransferUsesReactor()`,
 * so that curl and the tasks share a thread.
 *
 * No evaluator path awaits several fetches at once yet; the tarball
 * benchmark (`src/libfetchers-tests/tarball-bench.cc`) uses this.
 */
Task<UnpackedDownload> downloadIntoGixTarballCache(FileTransferRequest request, bool isZip);

} // namespace nix::fetchers
