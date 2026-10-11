#include <gtest/gtest.h>

#include "nix/store/build/goal.hh"

namespace nix {

namespace {

/**
 * A waker that records which of the waiters woke, in order.
 */
struct RecordingTarget : WakeTarget
{
    std::vector<int> & log;
    int id;

    RecordingTarget(std::vector<int> & log, int id)
        : log(log)
        , id(id)
    {
    }

    void wake() noexcept override
    {
        log.push_back(id);
    }
};

} // namespace

TEST(WaitQueue, wakesInOrderOfWaiting)
{
    std::vector<int> log;
    WaitQueue queue;
    WaitQueue::Entry a, b, c;
    b.wait(queue, makeWakeTarget<RecordingTarget>(log, 2)->waker());
    a.wait(queue, makeWakeTarget<RecordingTarget>(log, 1)->waker());
    c.wait(queue, makeWakeTarget<RecordingTarget>(log, 3)->waker());

    EXPECT_EQ(queue.wakeOne(), true);
    EXPECT_EQ(log, (std::vector<int>{2}));
    EXPECT_EQ(b.isWoken(), true);
    EXPECT_EQ(a.isWoken(), false);

    queue.wakeAll();
    EXPECT_EQ(log, (std::vector<int>{2, 1, 3}));
    EXPECT_EQ(queue.empty(), true);
    EXPECT_EQ(queue.wakeOne(), false);
}

TEST(WaitQueue, destroyedEntryDoesNotTakeATurn)
{
    std::vector<int> log;
    WaitQueue queue;
    WaitQueue::Entry b;
    {
        /* A cancelled task destroys its leaf, and the entry with it. */
        WaitQueue::Entry a;
        a.wait(queue, makeWakeTarget<RecordingTarget>(log, 1)->waker());
        b.wait(queue, makeWakeTarget<RecordingTarget>(log, 2)->waker());
    }

    EXPECT_EQ(queue.wakeOne(), true);
    EXPECT_EQ(log, (std::vector<int>{2}));
    EXPECT_EQ(queue.empty(), true);
}

TEST(WaitQueue, waitingAgainReplacesTheWaker)
{
    std::vector<int> log;
    WaitQueue queue;
    WaitQueue::Entry a;
    a.wait(queue, makeWakeTarget<RecordingTarget>(log, 1)->waker());
    a.wait(queue, makeWakeTarget<RecordingTarget>(log, 2)->waker());

    queue.wakeAll();
    EXPECT_EQ(log, (std::vector<int>{2}));
}

TEST(WaitQueue, entryOutlivesQueue)
{
    std::vector<int> log;
    WaitQueue::Entry a;
    {
        WaitQueue queue;
        a.wait(queue, makeWakeTarget<RecordingTarget>(log, 1)->waker());
    }
    /* Destroying `a` does not touch the queue. */
    EXPECT_EQ(a.isWoken(), false);
}

} // namespace nix
