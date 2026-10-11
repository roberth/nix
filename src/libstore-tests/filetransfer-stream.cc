#include <gtest/gtest.h>

#include "nix/store/filetransfer.hh"
#include "nix/util/file-system.hh"
#include "nix/util/fmt.hh"
#include "nix/util/signals.hh"
#include "nix/util/url.hh"

#include <future>
#include <thread>

namespace nix {

namespace {

/**
 * Read `stream` to its end on a thread of its own, as an unpacker does.
 * The result is the body, or a description of the exception that ended
 * the read.
 */
std::future<std::string> readOnThread(ref<TransferStream> stream)
{
    return std::async(std::launch::async, [stream]() -> std::string {
        std::string body;
        try {
            char buf[4096];
            while (true)
                body.append(buf, stream->read(buf, sizeof(buf)));
        } catch (EndOfFile &) {
            return body;
        } catch (FileTransferError &) {
            return "FileTransferError";
        } catch (Cancelled &) {
            return "Cancelled";
        } catch (std::exception & e) {
            return std::string("other exception: ") + e.what();
        }
    });
}

/**
 * Set the size of the download buffer for the duration of a test, so that
 * the transfer has to pause for the reader.
 */
struct SmallDownloadBuffer
{
    size_t previous = fileTransferSettings.downloadBufferSize;

    SmallDownloadBuffer()
    {
        fileTransferSettings.downloadBufferSize = 4096;
    }

    ~SmallDownloadBuffer()
    {
        fileTransferSettings.downloadBufferSize = previous;
    }
};

} // namespace

TEST(TransferStream, aReaderOnAnotherThreadGetsTheWholeBody)
{
    SmallDownloadBuffer smallBuffer;

    std::string contents;
    for (size_t i = 0; contents.size() < 3 * 1024 * 1024; ++i)
        contents += fmt("line %d\n", i);
    auto dir = createTempDir();
    AutoDelete cleanup(dir);
    auto path = dir / "body";
    writeFile(path, contents);

    auto fileTransfer = makeFileTransfer();
    auto stream = make_ref<TransferStream>();
    auto reader = readOnThread(stream);

    auto result = runBlocking(
        fileTransfer->transferStreaming(FileTransferRequest(VerbatimURL("file://" + path.string())), stream));

    EXPECT_EQ(result.data, "");
    EXPECT_TRUE(reader.get() == contents);
}

TEST(TransferStream, theReaderGetsTheErrorOfTheTransferWithItsType)
{
    auto dir = createTempDir();
    AutoDelete cleanup(dir);

    auto fileTransfer = makeFileTransfer();
    auto stream = make_ref<TransferStream>();
    auto reader = readOnThread(stream);

    EXPECT_THROW(
        runBlocking(fileTransfer->transferStreaming(
            FileTransferRequest(VerbatimURL("file://" + (dir / "does-not-exist").string())), stream)),
        FileTransferError);
    EXPECT_EQ(reader.get(), "FileTransferError");
}

TEST(TransferStream, closeWakesABlockedReader)
{
    auto stream = make_ref<TransferStream>();
    auto reader = readOnThread(stream);

    /* The reader blocks: nothing ever arrives. */
    EXPECT_EQ(reader.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);

    stream->close();

    EXPECT_EQ(reader.get(), "Cancelled");
}

} // namespace nix
