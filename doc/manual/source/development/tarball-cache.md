# The tarball cache on gitoxide

Nix unpacks each tarball that it fetches (`fetchTarball`, `tarball` and GitHub-style flake inputs) into the *tarball cache*, a bare Git repository in `~/.cache/nix/tarball-cache-v2` that contains only packfiles.
The cache deduplicates files between tarballs, and Nix reads the unpacked trees from it.

Two implementations of the cache exist:

- The default, on [gitoxide](https://github.com/GitoxideLabs/gitoxide), in `src/librust/src/tarball_cache.rs` and `src/libfetchers/tarball-cache-gix.cc`.
- The original, on libgit2, in `src/libfetchers/git-utils.cc`.

`_NIX_TARBALL_CACHE_BACKEND=libgit2` selects the libgit2 implementation.
The variable is internal: it lets one Nix binary compare the two implementations, and it gives a way back while the gitoxide implementation is new.
Both implementations read and write the same on-disk format, so they can share one cache.

## Why gitoxide

Writing the cache needs three things from the Git library: writes from many threads, new objects kept in memory until a flush, and a flush that writes them as a pack.
libgit2 supports none of these directly, and the libgit2 implementation works around each:

| What the cache needs | libgit2 implementation | gitoxide implementation |
|---|---|---|
| Writes from many threads | A pool of repository handles, because a libgit2 repository is not thread-safe | One writer and one object store, shared by all threads |
| No rescan of the pack directory on each new object | Sets the `refresh` function in the vtable of the pack backend to null | Opens the store handle in `RefreshMode::Never`, a supported option |
| One pack per unpack | One pack per pool member, because libgit2 cannot merge its in-memory backends | One map of new objects, written as one pack |
| Trees after their blobs | Flushes all blobs to disk before it writes the first tree, because `git_treebuilder_insert` looks up each child | Writes trees at any time, because gitoxide does not look up the children of a tree that it writes |

The gitoxide writer also compresses each object on the thread that writes it, so that a flush only copies bytes.

## How the gitoxide implementation works

`TarballCacheWriter` (Rust) holds the object store of the cache and a map of the new objects.
`write_blob` and `write_tree` compute the object id, skip the object if the store or the map already has it, and otherwise compress it and add it to the map.
`flush` writes the map as one pack:

1. It writes the pack to a temporary file in `objects/pack`, with base objects only.
2. It writes a version 2 index to a second temporary file.
   It writes the index itself, because it already knows the id, offset and CRC of each entry.
   The index writer of gitoxide decompresses and hashes every entry again to find them.
3. It moves the pack, then the index, to their final names, `pack-<checksum>.{pack,idx}`.
   A reader finds a pack through its index, so a reader never sees a partial pack.

A cancelled flush removes the temporary files, and the writer keeps the objects for a later flush.
The owner of a write cancels it with a `CancelToken`, which the flush checks; see [Fetching a tarball as a task](#fetching-a-tarball-as-a-task).
Calls from Rust into Nix, such as logging, go through the abstract classes in `nix/rust/tarball-cache.hh`, as [Rust](./rust.md) describes.

`TarballCacheReader` (Rust) reads trees and blobs.
`TarballCacheAccessor` (C++) is a `SourceAccessor` on top of it.
Each accessor has its own reader, because a gitoxide object handle has caches that are not thread-safe.

The libgit2 repository functions still create the cache, so that the format of a new cache does not depend on the implementation.

## Fetching a tarball as a task

`fetchTarball`, `fetchTree { type = "tarball"; }`, and the tarball and GitHub-style flake inputs unpack through `downloadIntoTarballCache()` (`src/libfetchers/tarball.cc`).
With the gitoxide implementation, the function runs one task with `runOnReactor()`, or with `runBlocking()` when curl runs on a thread of its own (`_NIX_FILE_TRANSFER_BACKEND=thread`).
The task owns the whole chain:

```
runOnReactor
└─ downloadIntoGixTarballCache                     C++ task, on the reactor thread
   └─ whenAll
      ├─ FileTransfer::transferStreaming           C++ task; curl runs on the reactor thread
      │     └─ writes the body into a TransferStream
      └─ unpackIntoTarballCache                    C++ task
         └─ tarball_cache_unpack                   Rust future
            └─ a thread of its own:
               ├─ Unpacker::run                    C++: libarchive reads the TransferStream,
               │                                   the tar adapter writes with the writer
               └─ TarballCacheWriter::flush        Rust: one pack
```

The download and the unpacking run at the same time.
The `TransferStream` connects them: curl appends the data on the reactor thread, and the unpacking thread reads it.
When the buffer exceeds `download-buffer-size`, the transfer pauses until the unpacking has taken the data.

The unpacking runs on a thread of the Rust future, because its work blocks: libarchive pulls its input with blocking reads, and gitoxide writes synchronously.
The reactor thread must not block (see [Asynchronous code](./async.md#the-reactor)), so the future starts the thread on its first poll, and the thread wakes the future when it is done.
libarchive stays, because it reads all the archive formats that Nix accepts.
A Rust reader would have to match them, and the tar adapter, which both implementations share, would have to move to Rust too.

### Cancellation

Destroying the task cancels the whole chain.
`runOnReactor()` destroys it on a user interrupt; a combinator such as `whenAll` or `race` destroys it when another task fails or wins.

- The transfer task removes the transfer from curl.
- Dropping the Rust future sets the `CancelToken` of its thread, closes the `TransferStream` so that a blocked read returns, and waits for the thread.
  On the reactor, the wait is a cleanup of the run (see [Asynchronous drop](./async.md#asynchronous-drop)), so that the drop does not block the reactor thread; under `runBlocking()` the drop joins the thread.
  The thread checks the token at each read of the stream, between the entries of the archive, and in the flush, up to the moment before the pack and its index are renamed into place.

When `runOnReactor()` returns, or after the drop under `runBlocking()`, no thread of the fetch runs, nothing writes to the cache, and the cache has no new pack and no temporary file.
`tarball-cancel.sh` tests this for an interrupt during the download, the unpacking and the flush.
`maintainers/tarball-cancel-stress.sh` interrupts many fetches at random times and checks the cache after each.

The token replaces a global interrupt check in the writer.
A global check stops every write in the process at once, and cannot stop the one write whose owner gave up on it, for example the loser of a `race`.

If one of the two tasks fails, `whenAll` cancels the other: a failed download stops the unpacking, and an archive that libarchive rejects stops the download.
An error keeps its type through both languages.
libarchive turns an exception of its input into a message, so the unpacker remembers the exception that the stream threw, for example the `FileTransferError` of the download, and returns that one.

The limits of the guarantee:

- The wait for the thread lasts until the unpacking threads reach their next check.
  On the reactor it delays the return of `runOnReactor()`, not other work; under `runBlocking()` it blocks the thread that drops the future.
  The tar adapter writes files on a pool of at most 10 threads.
  After a cancellation, a pool thread skips the files that are still queued, and the writer stops the file that it is hashing or compressing before its next piece of 1 MiB (`CHUNK` in `tarball_cache.rs`).
  The wait is therefore bounded by the time to hash and compress 1 MiB on each pool thread, plus 10 ms for the parse to notice while it waits for room in the backlog.
- The backlog of the tar adapter, the files that are parsed but not yet written, holds at most 2 files per pool thread and 64 MiB; the parse waits when it is full.
  The parse is often faster than the writers, for example in a debug build or on a machine with few cores.
  Without the bound, it parsed the whole archive ahead of the writers, with the contents of all queued files in memory, and a cancellation waited until the writers had drained the queue: up to 7.9 s in a debug build.
  A single file that is larger than 64 MiB is admitted when the backlog is empty, so the memory in the backlog is at most 64 MiB or one file.
- After the pack and its index are in place, a cancellation does not remove them.
  The pack is complete and valid, and the next fetch of the same tarball finds its objects.
- The libgit2 implementation (`_NIX_TARBALL_CACHE_BACKEND=libgit2`) does not run as a task.
  It unpacks on the calling thread, and stops only where its code checks for a user interrupt.

## Gaps

- The packs contain no deltas, because gitoxide has no delta compression.
  The libgit2 implementation gets almost none either: its packbuilder sees no path names, and it deltifies each pack only against the objects of the same pack.
- The writer checks tree entry names as libgit2 does, except for the names that NTFS and HFS+ treat as `.git`, such as `git~1`.
- Only the tarball cache uses gitoxide.
  Git working directories and `fetchGit` still use libgit2 and the `git` command.

## Measurements

The measurements compare the two implementations in one Nix binary, selected with `_NIX_TARBALL_CACHE_BACKEND`.

Method:

- Machine: Intel Core i9-14900K, 32 hardware threads, ZFS, load below 3.
- Build: Meson `debugoptimized`, which builds the Rust code with the cargo `release` profile.
- Input: two Nixpkgs revisions a week apart, `e8edbfce` (rev1) and `64ac110d` (rev2), as uncompressed tar files on local disk, so that neither the network nor gzip is part of the time.
- Command: `nix flake prefetch tarball+file://…` with `--store 'dummy://?read-only=false'`, so that no store database or disk writes are part of the time. The command still serialises the tree to a NAR and hashes it, which reads every file from the cache.
- Each row is the median of 7 runs after 1 warm-up run, measured with hyperfine; the range is the minimum and maximum.

| Workload | gitoxide | libgit2 |
|---|---|---|
| Unpack rev1 into an empty cache, then read it | 2.80 s (2.76–2.88), 4.9 s CPU | 5.09 s (5.06–5.29), 18.3 s CPU |
| Unpack rev2 into a cache that has rev1, then read it | 2.21 s (2.19–2.25), 2.5 s CPU | 4.17 s (4.03–4.23), 5.6 s CPU |
| Read rev1 from the cache | 1.37 s (1.35–1.43) | 2.10 s (2.09–2.14) |
| Read rev1 from a cache that the other implementation wrote | 1.46 s (1.42–1.52) | 1.99 s (1.97–2.03) |
| Peak memory, unpack rev1 (3 runs) | 556–640 MB | 954–1017 MB |
| Pack size after rev1 | 71.00 MB in 1 pack | 70.89 MB in 11 packs |
| Pack size after rev1 and rev2 | 80.25 MB in 2 packs | 80.19 MB in 21 packs |

Observations:

- The gitoxide implementation unpacks in about half the time of the libgit2 implementation, and uses about a quarter of the CPU time for a new tarball.
- It reads in about two thirds of the time. The pack count is not the main cause: libgit2 reads the single gitoxide pack only 5 % faster than its own 11 packs.
- The pack sizes differ by less than 0.2 %, although only libgit2 computes deltas.
- The libgit2 implementation writes one pack per writer thread plus one for the trees, so its cache has about ten times as many packs.

Inferences, not measured separately:

- Most of the CPU time of the libgit2 implementation goes to the delta search of its packbuilder, which saves almost nothing (see [Gaps](#gaps)).
- The memory difference is the in-memory objects: libgit2 keeps them uncompressed until the flush, and gitoxide keeps them compressed. The dummy store holds the NAR of the tree in memory in both cases, so the absolute numbers are higher than in a normal run.

Deltas between revisions of the same repository would make the cache smaller: a repack of both revisions with `git pack-objects` and path names gives about 67 MB instead of 80 MB.
Neither implementation does that today.

### The task against the synchronous unpacking

The gitoxide implementation unpacked synchronously on the calling thread before it ran as a task.
This comparison builds three versions of Nix in the same configuration: the synchronous unpacking, the first version of the task, and the task with the bounded backlog and the cancellation checks of the tar adapter.
The method is the one above (mean of 7 runs after 1 warm-up, empty cache; the range is the minimum and maximum).

| Input | Synchronous | Task, first version | Task, bounded backlog |
|---|---|---|---|
| rev1, uncompressed tar | 2.78 s (2.73–2.83) | 2.85 s (2.81–2.88) | 2.93 s (2.88–2.96) |
| rev1, gzip | 3.05 s (2.97–3.12) | 3.05 s (3.01–3.10) | 3.07 s (3.02–3.13) |

With gzip, as Nix fetches tarballs in practice, the three are equal within the noise: decompression, not writing, limits the speed.
The uncompressed tar shows a cost of about 5 % for the task with the bounded backlog.
An experiment with the backlog bound removed, and the cancellation checks kept, gave 2.89 s against 2.84 s for the first version, so the bound accounts for part of the cost; the differences are close to the noise (a standard deviation of about 0.04 s).
The download and the unpacking already overlapped before: a `sinkToSource` coroutine fed libarchive.

### Memory

The peak memory of an unpack of the gzipped rev1 (3 runs each, in MB; the dummy store keeps the NAR of the tree in memory, so the absolute numbers are higher than in a normal run):

| Build | Unbounded backlog | Bounded backlog |
|---|---|---|
| `debugoptimized` | 532, 520, 503 | 534, 521, 521 |
| `debug` (`-O0`) | 485, 483, 484 | 438, 418, 418 |

In the optimized build, the bound changes nothing measurable, which suggests that the writers keep up with the parse there.
In the debug build, the bound saves about 50 to 65 MB, which is the inferred size of the queued file contents.

### Interrupts at random times

`maintainers/tarball-cancel-stress.sh` fetched the gzipped rev1 over HTTP from a local server 50 times (`SEED=42`), each into an empty cache, and sent SIGINT after a random delay of 0.2 to 3.7 s.
The reference fetch took 3.1 s.
In the runs, the pack was in place from a delay of about 1.2 s on; the rest of the time goes to hashing the tree.

- 12 fetches completed before the signal, with the expected hash.
- 38 were interrupted: 17 before the pack was in place, 21 after.
- After each run, the cache had no temporary file, each pack had its index, and `git fsck --full` accepted it.

| Time from SIGINT to exit | Interrupted before the pack was in place (17) | After (21) |
|---|---|---|
| Median | 53 ms | 19 ms |
| Range | 9–478 ms | 13–28 ms |

The slow exits, 66 to 478 ms, all came from signals between 0.55 and 1.0 s, during the unpacking.
These runs used the first version of the task, whose tar adapter neither bounded its backlog nor checked the cancellation in its writers.

A debug build (`-O0`) showed that this version did not bound the time to exit: its parse runs far ahead of the writers, and the drop waited until they had written every queued file.
The same script, `SEED=1234`, 30 runs each, against the gzipped rev1 (a reference fetch takes 22.6 s in the debug build, and 3.1 s in the optimized one):

| Build | Version | Interrupted | Time from SIGINT to exit: min, median, p90, max |
|---|---|---|---|
| `debug` | first version | 26 | 7, 57, 6267, 7944 ms |
| `debug` | bounded backlog | 27 | 7, 30, 85, 125 ms |
| `debugoptimized` | bounded backlog | 26 | 10, 18, 24, 31 ms |

In all runs, the cache kept its invariants.

### The reactor against the curl thread

This comparison measures the swap of the curl thread for the [reactor](./async.md#the-reactor).
It uses release builds (`nix build .#nix-cli`) of the commit before the reactor (*before*), and of the commit with it, once with each value of `_NIX_FILE_TRANSFER_BACKEND`: `reactor`, where the fetch runs with `runOnReactor()`, and `thread`, where it runs with `runBlocking()` as before.

Method:

- Machine: the one above, but shared with other work: the load average was between 7 and 30 during the runs.
  Each round runs the three variants one after the other, so that a change of the load affects all of them.
- Server: `test-http-server` of the functional tests, on 127.0.0.1.
- Tarball: `nix flake prefetch tarball+http://…` of the gzipped rev1 (53 MB), into an empty cache and `--store 'dummy://?read-only=false'`.
- Download: `nix store prefetch-file` of a file of 1 GiB of random data, into an empty local store. Without unpacking, this shows the cost of driving curl.
- 7 rounds after 1 warm-up round. Each cell is the median, and the minimum and maximum, in seconds. CPU is user plus system time.

| Workload | Before | `reactor` | `thread` |
|---|---|---|---|
| Tarball, wall clock | 3.15 (3.03–3.84) | 3.17 (3.07–3.79) | 3.11 (3.04–3.73) |
| Tarball, CPU | 5.91 (5.67–6.77) | 5.93 (5.74–6.82) | 5.82 (5.71–6.47) |
| Download 1 GiB, wall clock | 3.84 (3.68–4.54) | 3.83 (3.75–4.47) | 3.86 (3.69–4.57) |
| Download 1 GiB, CPU | 2.57 (2.54–3.16) | 2.60 (2.54–2.88) | 2.59 (2.51–3.03) |

The differences are smaller than the spread of each variant.

`strace -f -c` of one download of 1 GiB counts the system calls that drive curl:

- `reactor`: 547 `epoll_wait`, 31 `epoll_ctl`, 3280 `poll`, 248267 system calls in total.
  The `poll` calls are the zero-timeout checks with which `FdWatch` emulates a level-triggered poll.
- `thread`: 6393 `poll` (`curl_multi_poll`), 6600 `read`, of which 6399 failed, 286560 system calls in total.
  That the failed reads are curl draining its wakeup pipe is an inference from their count, not a measurement.

`maintainers/tarball-cancel-stress.sh`, 50 runs per variant, `SEED=4242`, signals after 0.2 to 3.8 s:

| Variant | Interrupted | Time from SIGINT to exit: min, median, p90, max |
|---|---|---|
| Before | 48 | 6, 29, 56, 190 ms |
| `reactor` | 48 | 9, 34, 76, 144 ms |
| `thread` | 47 | 9, 31, 50, 70 ms |

All runs kept the invariants of the cache, and the completed runs gave the expected hash.
The load average was 12 at the start of these runs and 30 at the end, so the tails do not compare the variants reliably.

The same script on a later release build, with the goals as tasks and the `runInThread` cleanup, at a load average of 10 to 18 (reference fetch 3.1 s):

| Variant | Interrupted | Time from SIGINT to exit: min, median, p90, max |
|---|---|---|
| `reactor` | 36 | 5, 20, 34, 58 ms |
| `thread` | 38 | 10, 28, 45, 65 ms |

All runs kept the invariants of the cache.
