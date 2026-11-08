// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// uke (2026-11-07): the full-macOS OS window with its changed pages on the UFS instead of in RAM. The volatile RAM overlay held every
// page macOS wrote (2.1 GB by 480 s in log M46) in the ~4.8 GB the kernel had, and the system then re-read 23 GB of evicted pages in
// two minutes. Here the changes live in the store window at the end of userdata (native_ufs_store_*, sheng-data-persist=2), in
// ForSheng's PersistOverlay format (header, presence bitmap, page i at a fixed block), and survive reboots like a real disk.
// The flashed window stays the immutable seed: a page whose bit is set is read from the store, any other from the seed. Writes reach
// the store with FUA before they complete (PersistOverlay::writeRun: pages first, then the bitmap blocks that gained a bit), so a
// page the system was told is written is on the medium. A seed with another digest (a new flashed image) starts the store over.
// Runs of up to kDataMax bytes go to the media in one command. Caller holds no lock; this takes the window lock (Ops::lock).
#include "../NativeRamBlock/PersistOverlay.h"

extern "C" int native_ufs_store_read (uint64_t rel, uint32_t blocks, void *dst);
extern "C" int native_ufs_store_write (uint64_t rel, uint32_t blocks, const void *src);
extern "C" int native_ufs_store_sync (void);

struct UfsPersistStore {
  bool read (uint64_t b, uint32_t n, void *d) { return native_ufs_store_read (b, n, d); }
  bool write (uint64_t b, uint32_t n, const void *s) { return native_ufs_store_write (b, n, s); }
  bool sync () { return native_ufs_store_sync (); }
};

// Ops: lock(), unlock(), IOReturn read(lba, blocks, out) from the seed (absolute 4096-byte LBA), uint64_t load(done, out, n) from the
// request buffer (a write's data), uint64_t copy(done, data, n) into the request buffer (a read's data).
template <class Ops, class Store>
static UfsWindowResult ufsPersistIO (Ops &ops, persist::Overlay &ov, Store &store, uint64_t absolute, uint64_t windowBytes,
                                     bool write, uint64_t block, uint64_t count, uint8_t *bounce)
{
  if (!bounce || !ov.active || (write && ov.readOnly) || !ufsWindowGeometry (absolute, windowBytes) || (windowBytes & 4095) ||
      !count || block >= windowBytes / 512 || count > windowBytes / 512 - block || windowBytes / 4096 != ov.pages)
    return { kIOReturnBadArgument, 0 };
  UfsWindowGuard<Ops> guard (ops);
  const uint64_t maxPages = kDataMax / 4096, start = block * 512, length = count * 512, lastIndex = (start + length - 1) / 4096;
  const uint64_t seedFirst = absolute / 4096;
  uint64_t done = 0;
  while (done < length) {
    const uint64_t at = start + done, index = at / 4096, skip = at & 4095;
    if (!write) {
      // One run of pages that are all in the store, or all in the seed.
      const bool present = ov.has (index);
      uint64_t n = 1;
      while (n < maxPages && index + n <= lastIndex && ov.has (index + n) == present) ++n;
      const bool ok = present ? store.read (persist::pageBlock (ov.pages, index), static_cast<uint32_t> (n), bounce)
                              : ops.read (seedFirst + index, static_cast<uint32_t> (n), bounce) == kIOReturnSuccess;
      if (!ok) return { kIOReturnIOError, done };
      uint64_t avail = n * 4096 - skip;
      if (avail > length - done) avail = length - done;
      const uint64_t got = ops.copy (done, bounce + skip, avail);
      done += got;
      if (got != avail) return { kIOReturnIOError, done };
      continue;
    }
    if (!skip && length - done >= 4096) {
      // Whole pages: written as they come, no read of what they replace.
      uint64_t n = (length - done) / 4096;
      if (n > maxPages) n = maxPages;
      if (ops.load (done, bounce, n * 4096) != n * 4096) return { kIOReturnIOError, done };
      if (!ov.writeRun (store, index, static_cast<uint32_t> (n), bounce)) return { kIOReturnNotWritable, done };
      done += n * 4096;
      continue;
    }
    // A part of one page: its current content first (store or seed), then the new bytes over it.
    const bool ok = ov.has (index) ? store.read (persist::pageBlock (ov.pages, index), 1, bounce)
                                   : ops.read (seedFirst + index, 1, bounce) == kIOReturnSuccess;
    if (!ok) return { kIOReturnIOError, done };
    uint64_t n = 4096 - skip;
    if (n > length - done) n = length - done;
    if (ops.load (done, bounce + skip, n) != n) return { kIOReturnIOError, done };
    if (!ov.writeRun (store, index, 1, bounce)) return { kIOReturnNotWritable, done };
    done += n;
  }
  return { kIOReturnSuccess, done };
}
