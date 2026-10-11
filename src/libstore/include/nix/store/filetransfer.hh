#pragma once
///@file

#include <optional>
#include <cstdint>
#include <string>
#include <future>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>

#include "nix/util/logging.hh"
#include "nix/util/types.hh"
#include "nix/util/ref.hh"
#include "nix/util/configuration.hh"
#include "nix/util/serialise.hh"
#include "nix/util/task.hh"
#include "nix/util/url.hh"

#include "nix/store/config.hh"
#if NIX_WITH_AWS_AUTH
#  include "nix/store/aws-creds.hh"
#endif
#include "nix/store/s3-url.hh"

namespace nix {

const std::filesystem::path & nixConfDir();

class FileTransferSettings : public Config
{
    static std::optional<std::filesystem::path> getDefaultSSLCertFile();

    void anchor() override;

public:
    FileTransferSettings();

    Setting<bool> enableHttp2{this, true, "http2", "Whether to enable HTTP/2 support."};

    Setting<bool> enableHttp3{
        this,
        false,
        "http3",
        R"(
          Whether to try enabling HTTP/3 (QUIC).
          When enabled, Nix requests HTTP/3 and transparently falls back
          to HTTP/2 or HTTP/1.1 for servers that do not support it.
          This option has no effect unless the `nix` binary is linked
          against a libcurl built with HTTP/3 (QUIC) support.
        )"};

    Setting<std::string> userAgentSuffix{
        this, "", "user-agent-suffix", "String appended to the user agent in HTTP requests."};

    Setting<size_t> httpConnections{
        this,
        25,
        "http-connections",
        R"(
          The maximum number of parallel TCP connections used to fetch
          files from binary caches and by other downloads. It defaults
          to 25. 0 means no limit.
        )",
        {"binary-caches-parallel-connections"}};

    /* Do not set this too low. On glibc, getaddrinfo() contains fallback code
       paths that deal with ill-behaved DNS servers. Setting this too low
       prevents some fallbacks from occurring.

       See description of options timeout, single-request, single-request-reopen
       in resolv.conf(5). Also see https://github.com/NixOS/nix/pull/13985 for
       details on the interaction between getaddrinfo(3) behavior and libcurl
       CURLOPT_CONNECTTIMEOUT. */
    Setting<unsigned long> connectTimeout{
        this,
        15,
        "connect-timeout",
        R"(
          The timeout (in seconds) for establishing connections in the
          binary cache substituter. It corresponds to `curl`’s
          `--connect-timeout` option. A value of 0 means no limit.
        )"};

    Setting<unsigned long> stalledDownloadTimeout{
        this,
        300,
        "stalled-download-timeout",
        R"(
          The timeout (in seconds) for receiving data from servers
          during download. Nix cancels idle downloads after this
          timeout's duration.
        )"};

    Setting<uint32_t> tries{
        this,
        5,
        "filetransfer-retry-attempts",
        R"(
          The number of times Nix attempts a file transfer (download
          or upload) before giving up. Retries apply to transient
          failures: connection-level errors, HTTP 408, 429, and most
          5xx responses. Authentication failures (401/403/407),
          404/410, and other 4xx responses are not retried.
        )",
        {"download-attempts"}};

    Setting<uint32_t> retryDelayMs{
        this,
        100,
        "filetransfer-retry-delay",
        R"(
          Initial delay in milliseconds before retrying a failed file transfer
          (download or upload). The delay doubles with each subsequent attempt
          (exponential backoff) and is subject to random jitter (see
          `filetransfer-retry-jitter`).
        )"};

    Setting<uint32_t> retryDelayRateLimitedMs{
        this,
        5000,
        "filetransfer-retry-delay-rate-limited",
        R"(
          Initial delay in milliseconds before retrying a file transfer that
          failed with a rate-limit response (HTTP 429 or 503). The delay doubles
          with each subsequent attempt.

          Servers may send a `Retry-After` header specifying a longer delay;
          when present, Nix respects the larger of the two values.
        )"};

    Setting<uint32_t> retryMaxDelayMs{
        this,
        60000,
        "filetransfer-retry-max-delay",
        R"(
          Ceiling on the exponential backoff delay in milliseconds. This does not
          cap server-provided `Retry-After` values, which are honored as-is.
        )"};

    Setting<bool> retryJitter{
        this,
        true,
        "filetransfer-retry-jitter",
        R"(
          Whether to apply random jitter to retry delays. When enabled, each
          retry waits for a random duration between 0 and the computed delay
          ("full jitter"), which spreads out retry storms from many clients.

          Disable for deterministic retry timing (primarily useful for tests).
        )"};

    Setting<size_t> downloadBufferSize{
        this,
        1 * 1024 * 1024,
        "download-buffer-size",
        R"(
          The size of Nix's internal download buffer in bytes during `curl` transfers. If data is
          not processed quickly enough to exceed the size of this buffer, downloads may stall.
          The default is 1048576 (1 MiB).
        )"};

    Setting<unsigned int> downloadSpeed{
        this,
        0,
        "download-speed",
        R"(
          Specify the maximum transfer rate in kilobytes per second you want
          Nix to use for downloads.
        )"};

    Setting<AbsolutePath> netrcFile{
        this,
        nixConfDir() / "netrc",
        "netrc-file",
        R"(
          If set to an absolute path to a `netrc` file, Nix uses the HTTP
          authentication credentials in this file when trying to download from
          a remote host through HTTP or HTTPS. Defaults to
          `$NIX_CONF_DIR/netrc`.

          The `netrc` file consists of a list of accounts in the following
          format:

              machine my-machine
              login my-username
              password my-password

          For the exact syntax, see [the `curl`
          documentation](https://ec.haxx.se/usingcurl-netrc.html).

          > **Note**
          >
          > This must be an absolute path, and `~` is not resolved. For
          > example, `~/.netrc` won't resolve to your home directory's
          > `.netrc`.
        )"};

    Setting<std::optional<AbsolutePath>> caFile{
        this,
        getDefaultSSLCertFile(),
        "ssl-cert-file",
        R"(
          The path of a file containing CA certificates used to
          authenticate `https://` downloads. Nix by default uses
          the first of the following files that exists:

          1. `/etc/ssl/certs/ca-certificates.crt`
          2. `/nix/var/nix/profiles/default/etc/ssl/certs/ca-bundle.crt`

          The path can be overridden by the following environment
          variables, in order of precedence:

          1. `NIX_SSL_CERT_FILE`
          2. `SSL_CERT_FILE`
        )",
        {},
        // Don't document the machine-specific default value
        false};
};

extern FileTransferSettings fileTransferSettings;

/**
 * HTTP methods supported by FileTransfer.
 */
enum struct HttpMethod {
    Get,
    Put,
    Head,
    Post,
    Delete,
};

/**
 * Username and optional password for HTTP basic authentication.
 * These are used with curl's CURLOPT_USERNAME and CURLOPT_PASSWORD options
 * for various protocols including HTTP, FTP, and others.
 */
struct UsernameAuth
{
    std::string username;
    std::optional<std::string> password;
};

enum class PauseTransfer : bool {
    No = false,
    Yes = true,
};

struct FileTransferRequest
{
    VerbatimURL uri;
    Headers headers;
    std::string expectedETag;
    HttpMethod method = HttpMethod::Get;
    ActivityId parentAct;
    bool decompress = true;

    /**
     * Per-request retry overrides. When set, these take precedence over the
     * global `FileTransferSettings`. Typically populated from a store's URL
     * parameters (e.g. `s3://bucket?retry-attempts=8`).
     */
    std::optional<uint32_t> retryDelayMs;
    std::optional<uint32_t> retryDelayRateLimitedMs;
    std::optional<uint32_t> retryMaxDelayMs;
    std::optional<uint32_t> retryAttempts;

    /**
     * Optional path to the client certificate in "PEM" format. Only used for TLS-based protocols.
     */
    std::optional<std::filesystem::path> tlsCert;

    /**
     * Optional path to the client private key in "PEM" format. Only used for TLS-based protocols.
     */
    std::optional<std::filesystem::path> tlsKey;

    struct UploadData
    {
        UploadData(StringSource & s)
            : sizeHint(s.s.length())
            , source(&s)
        {
        }

        UploadData(std::size_t sizeHint, RestartableSource & source)
            : sizeHint(sizeHint)
            , source(&source)
        {
        }

        std::size_t sizeHint = 0;
        RestartableSource * source = nullptr;
    };

    std::optional<UploadData> data;
    std::string mimeType;

    /**
     * Callbacked invoked with a chunk of received data.
     * Can pause the transfer by returning PauseTransfer::Yes. No data must be consumed
     * if transfer is paused.
     */
    std::function<PauseTransfer(std::string_view data)> dataCallback;

    /**
     * Optional username and password for HTTP basic authentication.
     * When provided, these credentials will be used with curl's CURLOPT_USERNAME/PASSWORD option.
     */
    std::optional<UsernameAuth> usernameAuth;
#if NIX_WITH_AWS_AUTH
    /**
     * Pre-resolved AWS session token for S3 requests.
     * When provided along with usernameAuth, this will be used instead of fetching fresh credentials.
     */
    std::optional<std::string> preResolvedAwsSessionToken;
#endif

    FileTransferRequest(VerbatimURL uri)
        : uri(std::move(uri))
        , parentAct(getCurActivity())
    {
    }

    /**
     * `uri` with any userinfo (`user:password@`) stripped, for use in
     * progress, warning and error messages so credentials embedded in
     * the URL don't leak into logs. Returns `uri` verbatim if it can't
     * be parsed.
     */
    std::string displayUri() const;

    /**
     * Returns the method description for logging purposes.
     */
    std::string verb(bool continuous = false) const
    {
        switch (method) {
        case HttpMethod::Head:
        case HttpMethod::Get:
            return continuous ? "downloading" : "download";
        case HttpMethod::Put:
        case HttpMethod::Post:
            assert(data);
            return continuous ? "uploading" : "upload";
        case HttpMethod::Delete:
            return continuous ? "deleting" : "delete";
        }
        unreachable();
    }

    std::string noun() const
    {
        switch (method) {
        case HttpMethod::Head:
        case HttpMethod::Get:
            return "download";
        case HttpMethod::Put:
        case HttpMethod::Post:
            assert(data);
            return "upload";
        case HttpMethod::Delete:
            return "deletion";
        }
        unreachable();
    }

    void setupForS3();

private:
    friend struct curlFileTransfer;
#if NIX_WITH_AWS_AUTH
    std::optional<std::string> awsSigV4Provider;
#endif
};

struct FileTransferResult
{
    /**
     * Whether this is a cache hit (i.e. the ETag supplied in the
     * request is still valid). If so, `data` is empty.
     */
    bool cached = false;

    /**
     * The ETag of the object.
     */
    std::string etag;

    /**
     * All URLs visited in the redirect chain.
     *
     * @note Intentionally strings and not `ParsedURL`s so we faithfully
     * return what cURL gave us.
     */
    std::vector<std::string> urls;

    /**
     * The response body.
     */
    std::string data;

    uint64_t bodySize = 0;

    /**
     * An "immutable" URL for this resource (i.e. one whose contents
     * will never change), as returned by the `Link: <url>;
     * rel="immutable"` header.
     */
    std::optional<std::string> immutableUrl;
};

class Store;

/**
 * The body of a transfer that is in progress, as a `Source` for a thread
 * other than the one that drives the transfer. See
 * `FileTransfer::transferStreaming()`.
 *
 * A reader blocks until data arrives. When the buffer is full, the
 * transfer pauses until the reader has taken the data, so that a slow
 * reader does not make the buffer grow without limit.
 */
class TransferStream final : public Source
{
public:
    /**
     * Read the next bytes of the body. Blocks until at least one byte is
     * available. At the end of the body, throws `EndOfFile`. If the
     * transfer fails, throws its error, with its dynamic type. After
     * `close()`, throws `Cancelled`.
     */
    size_t read(char * data, size_t len) override;

    /**
     * Make the current and all later reads throw `Cancelled`. A reader
     * that the owner of the transfer stops uses this to wake up from a
     * blocking read. Thread-safe.
     */
    void close();

private:
    friend struct FileTransfer;

    struct State
    {
        std::string data;

        /**
         * The number of bytes at the start of `data` that a reader has
         * taken already.
         */
        size_t taken = 0;

        bool finished = false;
        std::exception_ptr error;
        bool closed = false;
        bool paused = false;

        /**
         * Unpause the transfer. Set when the transfer is enqueued.
         */
        std::function<void()> unpause;
    };

    std::mutex mutex;
    std::condition_variable available;
    State state;

    /**
     * Append data that arrived, and say whether the transfer must pause
     * because the buffer is full. Called on the transfer thread.
     */
    PauseTransfer push(std::string_view data);

    /**
     * Mark the end of the body, with the error of the transfer if it
     * failed.
     */
    void finish(std::exception_ptr error);

    void setUnpause(std::function<void()> unpause);
};

struct FileTransfer
{
protected:
    class Item
    {};

public:
    /**
     * An opaque handle to the file transfer. Can be used to reference an in-flight transfer operations.
     */
    struct ItemHandle
    {
        std::weak_ptr<Item> item;
        friend struct FileTransfer;

        explicit ItemHandle(std::weak_ptr<Item> item)
            : item(item)
        {
        }
    };

    virtual ~FileTransfer();

    /**
     * Enqueue a data transfer request, returning a future to the result of
     * the download. The future may throw a FileTransferError
     * exception.
     */
    virtual ItemHandle
    enqueueFileTransfer(const FileTransferRequest & request, Callback<FileTransferResult> callback) = 0;

    /**
     * Unpause a transfer that has been previously paused by a dataCallback.
     */
    virtual void unpauseTransfer(ItemHandle handle) = 0;

    /**
     * Cancel a transfer. The transfer stops and its connection is
     * released. If the transfer has not completed yet, its callback
     * receives a `Cancelled` error. Thread-safe; does nothing if the
     * transfer has already completed.
     */
    virtual void cancelTransfer(ItemHandle handle) = 0;

    /**
     * A data transfer as a task. The transfer starts when the task is
     * first polled. Destroying the task before it completes cancels the
     * transfer (`cancelTransfer`).
     *
     * This `FileTransfer` must outlive the task.
     */
    Task<FileTransferResult> transfer(FileTransferRequest request);

    /**
     * A data transfer as a task, whose body goes to `stream` while the
     * transfer runs, for a reader on another thread. The task completes
     * when the transfer does. The reader sees the end of the body, or the
     * error of the transfer, after the last data.
     *
     * Destroying the task before it completes cancels the transfer, like
     * `transfer()`. It does not close `stream`: a reader that the owner of
     * the task stops at the same time must be woken with
     * `TransferStream::close()`.
     *
     * `request.dataCallback` must be empty: the task sets it.
     */
    Task<FileTransferResult> transferStreaming(FileTransferRequest request, ref<TransferStream> stream);

    std::future<FileTransferResult> enqueueFileTransfer(const FileTransferRequest & request);

    /**
     * Synchronously download a file.
     */
    FileTransferResult download(const FileTransferRequest & request);

    /**
     * Synchronously upload a file.
     */
    FileTransferResult upload(const FileTransferRequest & request);

    /**
     * Synchronously delete a resource.
     */
    FileTransferResult deleteResource(const FileTransferRequest & request);

    /**
     * Download a file, writing its data to a sink. The sink will be
     * invoked on the thread of the caller.
     */
    void
    download(FileTransferRequest && request, Sink & sink, std::function<void(FileTransferResult)> resultCallback = {});

    enum Error { NotFound, Unauthorized, Forbidden, Misc, Transient };
};

/**
 * @return a shared FileTransfer object.
 *
 * Using this object is preferred because it enables connection reuse
 * and HTTP/2 multiplexing.
 */
ref<FileTransfer> getFileTransfer();

/**
 * @return a new FileTransfer object
 *
 * Prefer getFileTransfer() to this; see its docs for why.
 */
ref<FileTransfer> makeFileTransfer(const FileTransferSettings & settings = fileTransferSettings);

/**
 * Whether curl runs on the reactor (`nix/util/reactor.hh`), or on a thread
 * of its own that polls. The environment variable
 * `_NIX_FILE_TRANSFER_BACKEND` selects it: `reactor` (the default, except
 * on Windows) or `thread`.
 */
bool fileTransferUsesReactor();

class FileTransferError final : public CloneableError<FileTransferError, Error>
{
private:
    void anchor() override;

public:
    FileTransfer::Error error;
    /// intentionally optional
    std::optional<std::string> response;

    template<typename... Args>
    FileTransferError(FileTransfer::Error error, std::optional<std::string> response, Args &&... args);
};

} // namespace nix
