// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "../NativeRamBlock/SparseDisk.h"
// Reuse the RAM disk's sparse pages. Missing pages come from the read-only UFS
// seed; writes allocate at most residentLimit bytes and never call a disk writer.
// The caller owns the zeroed page table, bounce page, and serialized lifetime.
template <class Ops>
static UfsWindowResult ufsOverlayIO (Ops &ops, RamSparseDisk &disk, uint64_t absolute,
                                     bool write, uint64_t block, uint64_t count,
                                     uint64_t residentLimit, uint8_t *bounce)
{
  if (!disk.pages || !bounce || !ufsWindowGeometry (absolute, disk.bytes) || (disk.bytes & 4095) ||
      !residentLimit || (residentLimit & 4095) || !count || block >= disk.bytes / 512 ||
      count > disk.bytes / 512 - block)
    return { kIOReturnBadArgument, 0 };
  UfsWindowGuard<Ops> guard (ops);
  struct Adapter {
    Ops &parent;
    RamSparseDisk &disk;
    uint64_t absolute, offset, cursor, limit;
    uint8_t *bounce;
    IOReturn status = kIOReturnSuccess;
    uint8_t *allocate () {
      if (disk.resident >= limit) { status = kIOReturnNoMemory; return nullptr; }
      uint8_t *page = parent.allocate ();
      if (!page) { status = kIOReturnNoMemory; return nullptr; }
      // uke (2026-11-07): a write that covers this whole page replaces every byte the seed holds, so the seed is not read (the slab
      // page is zeroed). M45: 849 MiB of writes in 210 s made 217k single-page seed reads, most of the media time, while every read
      // queued behind them (longest read 618 ms). Partial-page writes still read the seed first.
      if (!(cursor & 4095) && end >= cursor + 4096) return page;
      status = parent.read ((absolute + cursor) / 4096, 1, page);
      if (status != kIOReturnSuccess) { parent.release (page); return nullptr; }
      return page;
    }
    uint64_t read (uint64_t done, void *out, uint64_t length) {
      uint64_t copied = parent.load (done, out, length);
      cursor = offset + done + copied;
      return copied;
    }
    // uke (2026-11-07): a page missing from the overlay used to cost one 4 KiB media command, so a 1 MiB read was 256 commands, each
    // polled to completion. Now the run of missing pages from here to the request's end (at most the bounce buffer, kDataMax) is read
    // in one command and served from the bounce buffer. The seed is read-only and resident pages are always looked up first, so the
    // buffered copy of a page can only be what the media holds. The bounce buffer is valid only within this request (window lock).
    uint64_t end = 0, bufferedFirst = 0, bufferedCount = 0;
    uint64_t copy (uint64_t done, const void *, uint64_t length) {
      uint64_t at = offset + done;
      const uint64_t index = at / 4096;
      const uint8_t *page = disk.find (index);
      if (!page) {
        if (!bufferedCount || index < bufferedFirst || index >= bufferedFirst + bufferedCount) {
          const uint64_t lastPage = (end + 4095) / 4096, maxPages = kDataMax / 4096;
          uint64_t run = 1;
          while (run < maxPages && index + run < lastPage && !disk.find (index + run)) ++run;
          bufferedCount = 0;
          status = parent.read ((absolute / 4096) + index, static_cast<uint32_t> (run), bounce);
          if (status != kIOReturnSuccess) return 0;
          bufferedFirst = index; bufferedCount = run;
        }
        page = bounce + (index - bufferedFirst) * 4096;
      }
      return parent.copy (done, page + (at & 4095), length);
    }
  } adapter = { ops, disk, absolute, block * 512, block * 512, residentLimit, bounce };
  adapter.end = block * 512 + count * 512;
  bool noMemory = false;
  uint64_t length = count * 512;
  uint64_t done = ramSparseIO (disk, write, block * 512, length, adapter, noMemory);
  IOReturn status = adapter.status != kIOReturnSuccess ? adapter.status :
    noMemory ? kIOReturnNoMemory : done == length ? kIOReturnSuccess : kIOReturnIOError;
  return { status, done };
}
