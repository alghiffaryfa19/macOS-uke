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
      status = parent.read ((absolute + cursor) / 4096, 1, page);
      if (status != kIOReturnSuccess) { parent.release (page); return nullptr; }
      return page;
    }
    uint64_t read (uint64_t done, void *out, uint64_t length) {
      uint64_t copied = parent.load (done, out, length);
      cursor = offset + done + copied;
      return copied;
    }
    uint64_t copy (uint64_t done, const void *, uint64_t length) {
      uint64_t at = offset + done;
      const uint8_t *page = disk.find (at / 4096);
      if (!page) {
        status = parent.read ((absolute + at) / 4096, 1, bounce);
        if (status != kIOReturnSuccess) return 0;
        page = bounce;
      }
      return parent.copy (done, page + (at & 4095), length);
    }
  } adapter = { ops, disk, absolute, block * 512, block * 512, residentLimit, bounce };
  bool noMemory = false;
  uint64_t length = count * 512;
  uint64_t done = ramSparseIO (disk, write, block * 512, length, adapter, noMemory);
  IOReturn status = adapter.status != kIOReturnSuccess ? adapter.status :
    noMemory ? kIOReturnNoMemory : done == length ? kIOReturnSuccess : kIOReturnIOError;
  return { status, done };
}
