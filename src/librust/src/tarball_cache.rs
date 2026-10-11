//! The tarball cache on gitoxide.
//!
//! The tarball cache is a bare Git repository that contains only packfiles.
//! Nix writes the files and directories of each unpacked tarball into it, and
//! reads them back through a `SourceAccessor`.
//!
//! The writer differs from the libgit2 implementation in three ways, each
//! of which gitoxide makes possible:
//!
//! - All threads share one writer and one object store. libgit2 repository
//!   handles are not thread-safe, so the libgit2 implementation keeps a pool
//!   of handles, each with its own in-memory backend.
//! - The writer collects all new objects of a session in one map, and
//!   `flush` writes them as one pack. The libgit2 implementation writes one
//!   pack per pool member, because libgit2 cannot merge its in-memory
//!   backends.
//! - A tree can be written before its children are on disk, because
//!   gitoxide does not look up the children of a tree that it writes. The
//!   libgit2 implementation flushes all blobs before it writes the first
//!   tree.
//!
//! The objects are compressed when they are written, on the threads that
//! write them, so that `flush` only copies bytes. The pack contains base
//! objects only: gitoxide has no delta compression.

use std::collections::HashSet;
use std::ffi::CStr;
use std::future::poll_fn;
use std::io::Write as _;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::task::{Poll, Waker};
use std::thread;
use std::time::Duration;

use cxx::UniquePtr;
use gix_hash::ObjectId;
use gix_object::WriteTo as _;

use crate::future::{AbiType, Error, NixException, NixFuture, export_future};

#[cxx::bridge(namespace = "nix::rs")]
mod ffi {
    /// An entry of a Git tree.
    struct GitTreeEntry {
        /// The file name. A tarball can contain names that are not UTF-8, so
        /// this is bytes.
        name: Vec<u8>,
        /// The Git file mode, for example `0o100644`.
        mode: u32,
        /// The SHA-1 of the object that the entry refers to.
        id: [u8; 20],
    }

    /// The tree or blob that an object peels to.
    struct GitPeeledObject {
        id: [u8; 20],
        is_tree: bool,
    }

    /// The result of unpacking a tarball into the cache.
    struct TarballUnpackResult {
        /// The id of the root of the unpacked tree, as the archive has it:
        /// before a single top-level directory is removed.
        tree: [u8; 20],
        /// The latest modification time of the files in the archive.
        last_modified: i64,
    }

    unsafe extern "C++" {
        include!("nix/rust/tarball-cache.hh");

        #[namespace = ""]
        type nix_future = crate::future::NixFuture;

        #[namespace = ""]
        type nix_exception;

        type TarballCacheHooks;

        #[cxx_name = "logDebug"]
        fn log_debug(self: &TarballCacheHooks, message: &str);

        /// Reads an archive and writes its contents with a writer. Runs on
        /// the thread of a `tarball_cache_unpack` future.
        type TarballUnpacker;

        /// Parse the archive and write its files and directories with
        /// `writer`, but do not flush. Stores the root in `out` and
        /// returns null, or returns the exception that stopped it. Checks
        /// `cancel` between entries and while it waits for data.
        unsafe fn run(
            self: &TarballUnpacker,
            writer: &TarballCacheWriter,
            cancel: &CancelToken,
            out: &mut TarballUnpackResult,
        ) -> *mut nix_exception;

        /// Wake `run` from a wait for data, so that it sees that it is
        /// cancelled. Called on the thread that drops the future, while
        /// `run` may be running on another.
        fn cancel(self: &TarballUnpacker);

        /// The hooks for the flush.
        fn hooks(self: &TarballUnpacker) -> &TarballCacheHooks;
    }

    extern "Rust" {
        /// A flag that the owner of an operation sets to stop it. The
        /// operation checks it at points where stopping is safe.
        type CancelToken;

        fn cancel_token_new() -> Box<CancelToken>;

        /// Set the flag. Thread-safe.
        fn cancel(self: &CancelToken);

        #[cxx_name = "isCancelled"]
        fn is_cancelled(self: &CancelToken) -> bool;

        /// Unpack an archive into the tarball cache at `objects_dir`, as a
        /// future of `TarballUnpackResult`.
        ///
        /// The future owns a writer and a thread. On its first poll it
        /// starts the thread, which runs `unpacker`, and then flushes the
        /// writer: the archive becomes one pack. Dropping the future
        /// before it completes cancels the thread and waits for it; the
        /// cache is then as it was, with no new pack and no temporary
        /// files.
        fn tarball_cache_unpack(objects_dir: &str, unpacker: UniquePtr<TarballUnpacker>) -> nix_future;

        /// Writes objects into the tarball cache. All methods take `&self`
        /// and may be called from several threads at once.
        type TarballCacheWriter;

        /// Open a writer for the object directory `objects_dir` of an
        /// existing repository.
        fn tarball_cache_writer_open(objects_dir: &str) -> Result<Box<TarballCacheWriter>>;

        /// Add a blob, and return its id.
        fn write_blob(self: &TarballCacheWriter, data: &[u8]) -> Result<[u8; 20]>;

        /// Add a tree with `entries`, in any order, and return its id.
        fn write_tree(self: &TarballCacheWriter, entries: &[GitTreeEntry]) -> Result<[u8; 20]>;

        /// Write the objects that were added since the last flush as one
        /// pack with its index. Stops with an error, and leaves no files,
        /// when `cancel` is set.
        fn flush(self: &TarballCacheWriter, cancel: &CancelToken, hooks: &TarballCacheHooks) -> Result<()>;

        /// The number of objects that the next `flush` writes.
        fn pending_objects(self: &TarballCacheWriter) -> usize;

        /// Reads objects from the tarball cache. Not thread-safe: the caller
        /// serialises the calls.
        type TarballCacheReader;

        /// Open a reader for the object directory `objects_dir`.
        fn tarball_cache_reader_open(objects_dir: &str) -> Result<Box<TarballCacheReader>>;

        /// Whether the object exists. Looks for new packs on disk if it does
        /// not find the object in the packs that it knows.
        fn has_object(self: &TarballCacheReader, id: &[u8; 20]) -> bool;

        /// Follow tags and commits from `id` to a tree, or return `id` if it
        /// is a blob or a tree.
        fn peel_to_tree_or_blob(self: &mut TarballCacheReader, id: &[u8; 20]) -> Result<GitPeeledObject>;

        /// The entries of the tree `id`, in Git order.
        fn read_tree(self: &mut TarballCacheReader, id: &[u8; 20]) -> Result<Vec<GitTreeEntry>>;

        /// The contents of the blob `id`.
        fn read_blob(self: &mut TarballCacheReader, id: &[u8; 20]) -> Result<Vec<u8>>;
    }
}

use ffi::{GitPeeledObject, GitTreeEntry, TarballCacheHooks, TarballUnpackResult, TarballUnpacker};

// The C++ unpacker is thread-safe: `run` and `cancel` may be called at the
// same time from different threads (`tarball-cache.hh`).
unsafe impl Send for TarballUnpacker {}
unsafe impl Sync for TarballUnpacker {}

unsafe impl AbiType for TarballUnpackResult {
    const NAME: &'static CStr = c"TarballUnpackResult";
}

pub struct CancelToken(AtomicBool);

fn cancel_token_new() -> Box<CancelToken> {
    Box::new(CancelToken(AtomicBool::new(false)))
}

impl CancelToken {
    fn cancel(&self) {
        self.0.store(true, Ordering::SeqCst);
    }

    fn is_cancelled(&self) -> bool {
        self.0.load(Ordering::SeqCst)
    }

    /// An error if the token is set, for `?` at the points where an
    /// operation may stop.
    fn check(&self) -> Result<(), String> {
        if self.is_cancelled() {
            Err("writing to the tarball cache was cancelled".to_owned())
        } else {
            Ok(())
        }
    }
}

/// Whether a test asked the writer to stop at `phase` until it is
/// cancelled (`_NIX_TEST_TARBALL_CACHE_PAUSE`), so that a test can
/// interrupt Nix in that phase. If so, announce it on stderr and wait.
fn pause_for_test(phase: &str, cancel: &CancelToken) -> Result<(), String> {
    if std::env::var_os("_NIX_TEST_TARBALL_CACHE_PAUSE").is_some_and(|v| v == phase) {
        eprintln!("nix: tarball cache test hook: paused in phase '{phase}'");
        while !cancel.is_cancelled() {
            std::thread::sleep(Duration::from_millis(10));
        }
    }
    cancel.check()
}

/// The tarball cache uses SHA-1, like the libgit2 implementation.
const HASH: gix_hash::Kind = gix_hash::Kind::Sha1;

fn open_store(objects_dir: &str) -> Result<Arc<gix_odb::Store>, String> {
    gix_odb::Store::at_opts(
        PathBuf::from(objects_dir),
        HASH,
        &mut std::iter::empty(),
        gix_odb::store::init::Options::default(),
    )
    .map(Arc::new)
    .map_err(|e| format!("opening the Git object database '{objects_dir}': {e}"))
}

fn to_oid(id: &[u8; 20]) -> ObjectId {
    ObjectId::from_bytes_or_panic(id)
}

fn to_bytes(id: &gix_hash::oid) -> [u8; 20] {
    id.as_bytes().try_into().expect("the tarball cache uses SHA-1")
}

/// An object that is compressed and waits for `flush`.
struct PackEntry {
    id: ObjectId,
    kind: gix_object::Kind,
    size: u64,
    compressed: Vec<u8>,
}

#[derive(Default)]
struct Pending {
    /// The ids of all objects that this writer has seen, written or found on
    /// disk. The object store does not see the packs that this writer adds,
    /// because it does not refresh (see `write_object`), so this set also
    /// keeps a second flush from writing the objects of the first again.
    known: HashSet<ObjectId>,
    entries: Vec<PackEntry>,
}

pub struct TarballCacheWriter {
    objects_dir: PathBuf,
    store: Arc<gix_odb::Store>,
    pending: Mutex<Pending>,
}

fn tarball_cache_writer_open(objects_dir: &str) -> Result<Box<TarballCacheWriter>, String> {
    Ok(Box::new(TarballCacheWriter {
        objects_dir: PathBuf::from(objects_dir),
        store: open_store(objects_dir)?,
        pending: Mutex::new(Pending::default()),
    }))
}

impl TarballCacheWriter {
    fn lock(&self) -> MutexGuard<'_, Pending> {
        // A panic while the lock is held leaves `Pending` consistent: every
        // update is a single insertion.
        self.pending.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn write_object(&self, kind: gix_object::Kind, data: &[u8]) -> Result<ObjectId, String> {
        let id = gix_object::compute_hash(HASH, kind, data).map_err(|e| e.to_string())?;

        if self.lock().known.contains(&id) {
            return Ok(id);
        }

        // Skip objects that the cache already has, for example the unchanged
        // files of a new revision of a tarball. The handle reads the pack
        // directory once and does not look again when it misses an object:
        // no other writer adds the objects of this session. This is the
        // supported equivalent of the libgit2 implementation, which removes
        // the `refresh` function from the vtable of its pack backend.
        let mut handle = self.store.to_handle_arc();
        handle.refresh_never();
        if gix_pack::Find::contains(&handle, &id) {
            self.lock().known.insert(id);
            return Ok(id);
        }

        let compressed = deflate(data)?;

        let mut pending = self.lock();
        if pending.known.insert(id) {
            pending.entries.push(PackEntry {
                id,
                kind,
                size: data.len() as u64,
                compressed,
            });
        }
        Ok(id)
    }

    fn write_blob(&self, data: &[u8]) -> Result<[u8; 20], String> {
        Ok(to_bytes(&self.write_object(gix_object::Kind::Blob, data)?))
    }

    fn write_tree(&self, entries: &[GitTreeEntry]) -> Result<[u8; 20], String> {
        let mut tree = gix_object::Tree {
            entries: entries
                .iter()
                .map(|entry| {
                    check_entry_name(&entry.name)?;
                    Ok(gix_object::tree::Entry {
                        mode: gix_object::tree::EntryMode::try_from(entry.mode)
                            .map_err(|mode| format!("invalid Git file mode {mode:o}"))?,
                        filename: entry.name.clone().into(),
                        oid: to_oid(&entry.id),
                    })
                })
                .collect::<Result<_, String>>()?,
        };
        tree.entries.sort();
        let mut data = Vec::new();
        tree.write_to(&mut data).map_err(|e| format!("encoding a Git tree: {e}"))?;
        Ok(to_bytes(&self.write_object(gix_object::Kind::Tree, &data)?))
    }

    fn pending_objects(&self) -> usize {
        self.lock().entries.len()
    }

    fn flush(&self, cancel: &CancelToken, hooks: &TarballCacheHooks) -> Result<(), String> {
        let entries = std::mem::take(&mut self.lock().entries);
        if entries.is_empty() {
            return Ok(());
        }

        match write_pack(&self.objects_dir.join("pack"), &entries, cancel) {
            Ok(name) => {
                hooks.log_debug(&format!(
                    "wrote {} objects to the tarball cache as {name}.{{pack,idx}}",
                    entries.len()
                ));
                Ok(())
            }
            Err(e) => {
                // Keep the objects, so that a later flush can write them.
                let mut pending = self.lock();
                let mut entries = entries;
                entries.append(&mut pending.entries);
                pending.entries = entries;
                Err(e)
            }
        }
    }
}

/// Reject the tree entry names that the libgit2 implementation rejects, so
/// that both accept the same tarballs: an empty name, `.`, `..`, a name with
/// `/` or NUL, and `.git` in any case. libgit2 also rejects names that NTFS
/// or HFS+ treat as `.git`, such as `git~1`; this check does not.
fn check_entry_name(name: &[u8]) -> Result<(), String> {
    if name.is_empty()
        || name == b"."
        || name == b".."
        || name.contains(&b'/')
        || name.contains(&0)
        || name.eq_ignore_ascii_case(b".git")
    {
        return Err(format!(
            "invalid name '{}' for a Git tree entry",
            String::from_utf8_lossy(name)
        ));
    }
    Ok(())
}

fn deflate(data: &[u8]) -> Result<Vec<u8>, String> {
    // Git and libgit2 compress pack entries at the zlib default level.
    let mut out = gix_zlib::stream::deflate::Write::new(Vec::new(), gix_zlib::Compression::DEFAULT);
    out.write_all(data)
        .and_then(|()| out.flush())
        .map_err(|e| format!("compressing a Git object: {e}"))?;
    Ok(out.into_inner())
}

/// Where an entry is in the pack, for the index.
struct IndexEntry {
    id: ObjectId,
    crc32: u32,
    offset: u64,
}

/// Write `entries` to a pack and its version 2 index in `pack_dir`, and
/// return the name of the pack, `pack-<checksum>`.
///
/// The files appear under their final names only when they are complete:
/// first the pack, then the index. A reader finds a pack through its index,
/// so an interrupted flush leaves no pack that a reader can see.
fn write_pack(pack_dir: &Path, entries: &[PackEntry], cancel: &CancelToken) -> Result<String, String> {
    let io_error = |what: &str| {
        let what = what.to_owned();
        move |e: std::io::Error| format!("{what} in '{}': {e}", pack_dir.display())
    };

    let num_entries = u32::try_from(entries.len()).map_err(|_| "too many objects for one pack".to_owned())?;

    let mut pack_file = tempfile::Builder::new()
        .prefix("tmp_pack_")
        .tempfile_in(pack_dir)
        .map_err(io_error("creating a temporary pack"))?;
    let mut index = Vec::with_capacity(entries.len());
    let pack_checksum = {
        let mut out = gix_hash::io::Write::new(std::io::BufWriter::new(pack_file.as_file_mut()), HASH);

        out.write_all(&gix_pack::data::header::encode(gix_pack::data::Version::V2, num_entries))
            .map_err(io_error("writing a pack"))?;
        let mut offset = gix_pack::data::header::SIZE as u64;

        let mut header = Vec::with_capacity(16);
        for (n, entry) in entries.iter().enumerate() {
            if n % 1024 == 0 {
                cancel.check()?;
            }

            header.clear();
            pack_entry_header(entry.kind)
                .write_to(entry.size, &mut header)
                .map_err(io_error("encoding a pack entry header"))?;
            let mut crc = crc32fast::Hasher::new();
            crc.update(&header);
            crc.update(&entry.compressed);

            out.write_all(&header).map_err(io_error("writing a pack"))?;
            out.write_all(&entry.compressed).map_err(io_error("writing a pack"))?;

            index.push(IndexEntry {
                id: entry.id,
                crc32: crc.finalize(),
                offset,
            });
            offset += (header.len() + entry.compressed.len()) as u64;
        }

        let gix_hash::io::Write { hash, inner: mut buffered } = out;
        let pack_checksum = hash.try_finalize().map_err(|e| e.to_string())?;
        buffered.write_all(pack_checksum.as_slice()).map_err(io_error("writing a pack"))?;
        buffered.flush().map_err(io_error("writing a pack"))?;
        pack_checksum
    };

    pause_for_test("flush", cancel)?;

    let mut index_file = tempfile::Builder::new()
        .prefix("tmp_idx_")
        .tempfile_in(pack_dir)
        .map_err(io_error("creating a temporary pack index"))?;
    index.sort_by(|a, b| a.id.cmp(&b.id));
    write_index_v2(
        std::io::BufWriter::new(index_file.as_file_mut()),
        &index,
        &pack_checksum,
    )
    .map_err(io_error("writing a pack index"))?;

    let name = format!("pack-{}", pack_checksum.to_hex());
    for file in [&pack_file, &index_file] {
        make_read_only(file.path()).map_err(io_error("changing the permissions of a pack"))?;
        file.as_file().sync_all().map_err(io_error("syncing a pack"))?;
    }
    // The last point where the flush can stop and leave nothing behind:
    // from here on, the pack appears in the cache.
    cancel.check()?;
    // A pack with the same checksum has the same contents, so replacing an
    // existing one is harmless.
    pack_file
        .persist(pack_dir.join(format!("{name}.pack")))
        .map_err(|e| io_error("moving a pack into place")(e.error))?;
    index_file
        .persist(pack_dir.join(format!("{name}.idx")))
        .map_err(|e| io_error("moving a pack index into place")(e.error))?;

    Ok(name)
}

fn pack_entry_header(kind: gix_object::Kind) -> gix_pack::data::entry::Header {
    use gix_pack::data::entry::Header;
    match kind {
        gix_object::Kind::Tree => Header::Tree,
        gix_object::Kind::Blob => Header::Blob,
        gix_object::Kind::Commit => Header::Commit,
        gix_object::Kind::Tag => Header::Tag,
    }
}

/// Write a version 2 pack index for `entries`, which are sorted by id.
///
/// gitoxide's index writer reads the pack again to compute the ids and the
/// CRCs. The writer already has them, so it writes the index itself. The
/// format is in Git's `Documentation/gitformat-pack.adoc`.
fn write_index_v2(
    out: impl std::io::Write,
    entries: &[IndexEntry],
    pack_checksum: &ObjectId,
) -> std::io::Result<()> {
    let mut out = gix_hash::io::Write::new(out, HASH);

    out.write_all(b"\xfftOc")?;
    out.write_all(&2u32.to_be_bytes())?;

    let mut fanout = [0u32; 256];
    for entry in entries {
        fanout[entry.id.as_slice()[0] as usize] += 1;
    }
    let mut total = 0;
    for count in fanout {
        total += count;
        out.write_all(&total.to_be_bytes())?;
    }

    for entry in entries {
        out.write_all(entry.id.as_slice())?;
    }
    for entry in entries {
        out.write_all(&entry.crc32.to_be_bytes())?;
    }

    // Offsets that do not fit in 31 bits go into a table of 64-bit offsets,
    // and the 32-bit slot holds their position in it, with the high bit set.
    let mut large = Vec::new();
    for entry in entries {
        let slot = match u32::try_from(entry.offset) {
            Ok(offset) if offset < 0x8000_0000 => offset,
            _ => {
                large.push(entry.offset);
                0x8000_0000 | u32::try_from(large.len() - 1).expect("fewer than 2^31 large offsets")
            }
        };
        out.write_all(&slot.to_be_bytes())?;
    }
    for offset in large {
        out.write_all(&offset.to_be_bytes())?;
    }

    out.write_all(pack_checksum.as_slice())?;
    let gix_hash::io::Write { hash, mut inner } = out;
    let index_checksum = hash.try_finalize().map_err(std::io::Error::other)?;
    inner.write_all(index_checksum.as_slice())?;
    inner.flush()
}

#[cfg(unix)]
fn make_read_only(path: &Path) -> std::io::Result<()> {
    use std::os::unix::fs::PermissionsExt;
    std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o444))
}

#[cfg(not(unix))]
fn make_read_only(path: &Path) -> std::io::Result<()> {
    let mut permissions = std::fs::metadata(path)?.permissions();
    permissions.set_readonly(true);
    std::fs::set_permissions(path, permissions)
}

// Unpacking as a future.
//
// The future runs the blocking work, parsing and writing, on a thread of
// its own, because no Rust executor exists in Nix: C++ polls the future
// (`runBlocking`). The thread wakes the future when it is done.

/// What the thread of an unpack future shares with the future.
struct UnpackShared {
    cancel: CancelToken,
    /// The result of the thread, when it is done.
    result: Mutex<Option<Result<TarballUnpackResult, Error>>>,
    /// The waker of the last poll. The thread wakes it when it is done.
    waker: Mutex<Option<Waker>>,
}

/// The thread of an unpack future. Dropping it before the thread is done
/// cancels the thread and waits for it.
struct UnpackJob {
    shared: Arc<UnpackShared>,
    unpacker: Arc<UniquePtr<TarballUnpacker>>,
    thread: Option<thread::JoinHandle<()>>,
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(|e| e.into_inner())
}

impl Drop for UnpackJob {
    fn drop(&mut self) {
        let Some(thread) = self.thread.take() else {
            return;
        };
        self.shared.cancel.cancel();
        self.unpacker.cancel();
        // Wait for the thread. After the drop, nothing writes to the cache
        // and no temporary file is left. The thread checks the token while
        // it waits for data, between the entries of the archive, and
        // during the flush, so the wait is short.
        let _ = thread.join();
    }
}

fn tarball_cache_unpack(objects_dir: &str, unpacker: UniquePtr<TarballUnpacker>) -> NixFuture {
    let objects_dir = objects_dir.to_owned();
    let mut unpacker = Some(unpacker);
    let mut job: Option<UnpackJob> = None;
    export_future(poll_fn(move |cx| {
        // Lazy, like any future: the thread starts on the first poll.
        let job = job.get_or_insert_with(|| {
            start_unpack(
                objects_dir.clone(),
                unpacker.take().expect("the unpacker is used once"),
            )
        });
        // Store the waker before looking for the result. The thread stores
        // the result before it takes the waker, so either this poll sees
        // the result, or the thread wakes this waker.
        *lock(&job.shared.waker) = Some(cx.waker().clone());
        let result = lock(&job.shared.result).take();
        match result {
            Some(result) => {
                if let Some(thread) = job.thread.take() {
                    let _ = thread.join();
                }
                Poll::Ready(result)
            }
            None => Poll::Pending,
        }
    }))
}

fn start_unpack(objects_dir: String, unpacker: UniquePtr<TarballUnpacker>) -> UnpackJob {
    let shared = Arc::new(UnpackShared {
        cancel: CancelToken(AtomicBool::new(false)),
        result: Mutex::new(None),
        waker: Mutex::new(None),
    });
    let unpacker = Arc::new(unpacker);

    let spawned = {
        let shared = shared.clone();
        let unpacker = unpacker.clone();
        thread::Builder::new()
            .name("nix-unpack".to_owned())
            .spawn(move || {
                let result = catch_unwind(AssertUnwindSafe(|| {
                    unpack(&objects_dir, &unpacker, &shared.cancel)
                }))
                .unwrap_or_else(|payload| Err(Error::from_panic(payload)));
                *lock(&shared.result) = Some(result);
                let waker = lock(&shared.waker).take();
                if let Some(waker) = waker {
                    waker.wake();
                }
            })
    };

    let thread = match spawned {
        Ok(thread) => Some(thread),
        Err(e) => {
            *lock(&shared.result) = Some(Err(Error::msg(format!(
                "starting a thread to unpack into the tarball cache: {e}"
            ))));
            None
        }
    };

    UnpackJob {
        shared,
        unpacker,
        thread,
    }
}

/// The work of the thread of an unpack future.
fn unpack(
    objects_dir: &str,
    unpacker: &TarballUnpacker,
    cancel: &CancelToken,
) -> Result<TarballUnpackResult, Error> {
    let writer = tarball_cache_writer_open(objects_dir).map_err(Error::msg)?;

    let mut out = TarballUnpackResult {
        tree: [0; 20],
        last_modified: 0,
    };
    let exception = unsafe { unpacker.run(&writer, cancel, &mut out) };
    if !exception.is_null() {
        // A C++ exception, for example the error of the download, keeps
        // its dynamic type on the way back to C++.
        return Err(unsafe { Error::from_exception(exception as *mut NixException) });
    }

    pause_for_test("before-flush", cancel).map_err(Error::msg)?;
    writer.flush(cancel, unpacker.hooks()).map_err(Error::msg)?;
    Ok(out)
}

pub struct TarballCacheReader {
    objects: gix_odb::Cache<gix_odb::store::Handle<Arc<gix_odb::Store>>>,
    buffer: Vec<u8>,
}

fn tarball_cache_reader_open(objects_dir: &str) -> Result<Box<TarballCacheReader>, String> {
    let mut objects = open_store(objects_dir)?.to_cache_arc();
    // Packs written by libgit2 contain deltas. A small cache of decoded
    // bases avoids decoding the same base again for each object built on it.
    objects.set_pack_cache(|| Box::new(gix_pack::cache::lru::StaticLinkedList::<64>::default()));
    Ok(Box::new(TarballCacheReader {
        objects,
        buffer: Vec::new(),
    }))
}

impl TarballCacheReader {
    fn has_object(&self, id: &[u8; 20]) -> bool {
        gix_object::Exists::exists(&self.objects, &to_oid(id))
    }

    fn find(&mut self, id: &ObjectId) -> Result<gix_object::Kind, String> {
        match gix_object::Find::try_find(&self.objects, id, &mut self.buffer) {
            Ok(Some(data)) => Ok(data.kind),
            Ok(None) => Err(format!("Git object '{id}' does not exist in the tarball cache")),
            Err(e) => Err(format!("reading Git object '{id}': {e}")),
        }
    }

    fn peel_to_tree_or_blob(&mut self, id: &[u8; 20]) -> Result<GitPeeledObject, String> {
        let mut id = to_oid(id);
        // A chain longer than this is a cycle or an attack.
        for _ in 0..64 {
            match self.find(&id)? {
                gix_object::Kind::Tree => return Ok(GitPeeledObject { id: to_bytes(&id), is_tree: true }),
                gix_object::Kind::Blob => return Ok(GitPeeledObject { id: to_bytes(&id), is_tree: false }),
                gix_object::Kind::Commit => {
                    id = gix_object::CommitRef::from_bytes(&self.buffer, HASH)
                        .map_err(|e| format!("parsing Git commit '{id}': {e}"))?
                        .tree();
                }
                gix_object::Kind::Tag => {
                    id = gix_object::TagRef::from_bytes(&self.buffer, HASH)
                        .map_err(|e| format!("parsing Git tag '{id}': {e}"))?
                        .target();
                }
            }
        }
        Err(format!("peeling Git object '{id}': too many tags or commits"))
    }

    fn read_tree(&mut self, id: &[u8; 20]) -> Result<Vec<GitTreeEntry>, String> {
        let id = to_oid(id);
        if self.find(&id)? != gix_object::Kind::Tree {
            return Err(format!("Git object '{id}' is not a tree"));
        }
        gix_object::TreeRefIter::from_bytes(&self.buffer, HASH)
            .map(|entry| {
                let entry = entry.map_err(|e| format!("parsing Git tree '{id}': {e}"))?;
                Ok(GitTreeEntry {
                    name: entry.filename.to_vec(),
                    mode: u32::from(entry.mode.value()),
                    id: to_bytes(entry.oid),
                })
            })
            .collect()
    }

    fn read_blob(&mut self, id: &[u8; 20]) -> Result<Vec<u8>, String> {
        let id = to_oid(id);
        if self.find(&id)? != gix_object::Kind::Blob {
            return Err(format!("Git object '{id}' is not a blob"));
        }
        Ok(std::mem::take(&mut self.buffer))
    }
}
