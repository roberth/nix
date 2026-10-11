#pragma once

#include "nix/util/file-descriptor.hh"

namespace nix {

struct FdSink;

/**
 * Try to copy @p nbytes bytes from @p from fd to @p to fd, starting at @p offset.
 * The intent is to try to use optimized file copies like copy_file_range (on Linux) first
 * and only fall back to the slower copying when the fast path fails or is unsupported.
 *
 * @throws SystemError in case the copy was started, but failed halfway.
 *
 * @return true if the copy succeeded, false if it's unsupported. Partial copies
 *         should be reported as errors.
 *
 * @pre written should be set to 0 before calling this function.
 */
bool tryCopyFdRangeFast(Descriptor from, Descriptor to, off_t offset, size_t nbytes, size_t & written);

#ifndef _WIN32
/**
 * On a thread that `runInThread` runs, wait until a read (`POLLIN`) or a
 * write (`POLLOUT`) of `size` bytes on `fd` can start, and throw
 * `Interrupted` if the task of the thread is dropped first. Return the
 * number of bytes that the call can transfer without blocking again:
 * `size`, except for a write to a pipe or a socket, where `poll` only
 * promises room for `PIPE_BUF` bytes.
 *
 * On other threads, return `size` at once, and let the blocking call of
 * the caller wait.
 *
 * Without it, a drop waits until a blocking read or write returns, which
 * on a stalled connection is never.
 */
size_t waitForFdOrThreadCancellation(Descriptor fd, short events, size_t size);
#endif

} // namespace nix
