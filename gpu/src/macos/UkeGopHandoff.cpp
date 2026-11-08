// SPDX-License-Identifier: BSD-2-Clause-Patent
// UkeGopHandoff: the GPU scanout hand-off of iPadOS (UniXNU drivers/GopFramebuffer/NativeGOPPresenter.cpp, boot-arg
// sheng-fb-gpu=1) for macOS 26.5's NativeGOPDisplay. WindowServer's Adreno plugin (GpuScanoutClient.h) waits for a finished
// frame through the accelerator's admitted connection (native_gop_handoff_call: NGP mode, wait, complete), gets the
// compositor surface's pages mapped into its task, lets the GPU write it into the firmware scanout (memory type 0x5343,
// native_gop_handoff_memory) and completes it. Nothing here copies a pixel.
//
// Unlike iPadOS, the offer is synchronous: NativeGOPDisplay::program_hardware_gated offers the selected surface and waits
// (bounded) for the client's completion before it retires the swap itself, so the swap never leaves the display's gate.
// A refused offer (no client waiting, a frame still outstanding, or no sheng-fb-gpu=1), a failed GPU copy or a timeout
// returns non-zero and the display keeps its CPU copy for that frame.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>

namespace {
// NativeGOPWire.h (iPadOS), the records the plugin sends: same layout.
enum : uint32_t {NGP_BGRX8888 = 1, NGP_VERSION = 1, NGP_METHOD_MODE = 0, NGP_METHOD_WAIT_SWAP = 2, NGP_METHOD_COMPLETE_SWAP = 3};
struct NGPMode {uint32_t version, format; uint64_t width, height, stride, bytes;};
struct NGPWait {uint64_t timeout_ms;};
struct NGPSwap {uint64_t sequence, address, bytes, stride; uint32_t width, height, format, reserved;};
struct NGPComplete {uint64_t sequence; uint32_t status, reserved;};
static_assert(sizeof(NGPMode) == 40 && sizeof(NGPWait) == 8 && sizeof(NGPSwap) == 48 && sizeof(NGPComplete) == 16, "GOP hand-off ABI");

IOLock *gLock;
bool gEnabled, gClient;
NGPMode gMode;
IOMemoryDescriptor *gScanout;
struct Swap {
    bool offered, delivered, done, abandoned;
    uint64_t sequence;
    IOMemoryDescriptor *memory;
    IOMemoryMap *map;
    uint64_t stride;
    uint32_t width, height, format;
    int status;
    uint64_t offeredAt;
} gSwap;
uint64_t gSequence;
// The compositor cycles a few surfaces: each one's mapping into the client stays here under an id (NGPSwap.reserved), so the
// client's GPU import of those pages is made once (the plugin keeps 4 imports; 6 mappings here).
struct Kept {IOMemoryDescriptor *memory; IOMemoryMap *map; task_t task; uint32_t id; uint64_t used;};
constexpr unsigned kKept = 6;
Kept gKept[kKept];
uint32_t gKeptId;
uint64_t gKeptUse;

void clearSwapLocked(Swap &out) {out = gSwap; gSwap = {};}
void releaseSwap(Swap &s) {OSSafeReleaseNULL(s.map); OSSafeReleaseNULL(s.memory);}
}

extern "C" {
// Counters for the UKE log line (UkeGpu.cpp): offers taken by the GPU, GPU frames done, failed, timed out, refused (CPU copy).
volatile uint64_t uke_gop_gpu_stats[8];
enum {kOffers, kDone, kFailed, kTimeouts, kRefused, kWaits, kLateCompletes};

// Called once by NativeGOPDisplay::start with the boot framebuffer it scans out.
void uke_gop_gpu_init(uint64_t physical, uint64_t width, uint64_t height, uint64_t rowBytes)
{
    unsigned wanted = 0;
    PE_parse_boot_argn("sheng-fb-gpu", &wanted, sizeof wanted);
    if (wanted != 1 || gLock || !physical || !width || !height || rowBytes < width * 4) return;
    gScanout = IOMemoryDescriptor::withPhysicalAddress(physical, rowBytes * height, kIODirectionInOut);
    if (!gScanout) return;
    gLock = IOLockAlloc();
    if (!gLock) {OSSafeReleaseNULL(gScanout); return;}
    gMode = {NGP_VERSION, NGP_BGRX8888, width, height, rowBytes, rowBytes * height};
    gEnabled = true;
    printf("UkeGopHandoff: GPU scanout hand-off on, %llux%llu row %llu\n", width, height, rowBytes);
}

// The display offers a finished frame and waits for the GPU's copy. 0: on screen; otherwise the display copies it itself.
int uke_gop_gpu_present(IOMemoryDescriptor *memory, uint64_t stride, uint32_t width, uint32_t height, uint32_t format, uint32_t timeoutMs)
{
    if (!gEnabled || !memory || format != NGP_BGRX8888) return -1;
    uint64_t now = mach_absolute_time(), deadline;
    clock_interval_to_deadline(timeoutMs, kMillisecondScale, &deadline);
    Swap stale = {};
    IOLockLock(gLock);
    // A copy the client never completed (it crashed or exited): after a second its frame is forgotten.
    if (gSwap.offered && gSwap.abandoned) {
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(now - gSwap.offeredAt, &ns);
        if (ns > 1000000000ull) {clearSwapLocked(stale); gClient = false;}
    }
    if (!gClient || gSwap.offered) {
        IOLockUnlock(gLock);
        releaseSwap(stale);
        uke_gop_gpu_stats[kRefused] = uke_gop_gpu_stats[kRefused] + 1;
        return -2;
    }
    memory->retain();
    const uint64_t sequence = ++gSequence;
    gSwap = {true, false, false, false, sequence, memory, nullptr, stride, width, height, format, 0, now};
    uke_gop_gpu_stats[kOffers] = uke_gop_gpu_stats[kOffers] + 1;
    IOLockWakeup(gLock, &gSwap, false);
    while (gSwap.sequence == sequence && !gSwap.done) {
        if (IOLockSleepDeadline(gLock, &gSwap.done, deadline, THREAD_UNINT) != THREAD_AWAKENED) break;
    }
    Swap done = {};
    int result;
    if (gSwap.sequence == sequence && gSwap.done) {
        result = gSwap.status;
        clearSwapLocked(done);
    } else if (gSwap.sequence == sequence && !gSwap.delivered) {
        result = -3;   // nobody took it in time: withdrawn
        clearSwapLocked(done);
        gClient = false;
    } else {
        result = -4;   // the client holds it: it completes (or is forgotten) later; this frame is copied by the CPU
        if (gSwap.sequence == sequence) gSwap.abandoned = true;
    }
    IOLockUnlock(gLock);
    releaseSwap(done);
    releaseSwap(stale);
    if (result == 0) uke_gop_gpu_stats[kDone] = uke_gop_gpu_stats[kDone] + 1;
    else if (result == -3 || result == -4) uke_gop_gpu_stats[kTimeouts] = uke_gop_gpu_stats[kTimeouts] + 1;
    else uke_gop_gpu_stats[kFailed] = uke_gop_gpu_stats[kFailed] + 1;
    return result;
}
}

namespace {
IOReturn waitSwap(task_t task, uint64_t timeoutMs, NGPSwap *out)
{
    if (timeoutMs > 1000) timeoutMs = 1000;
    uint64_t deadline;
    clock_interval_to_deadline((uint32_t)timeoutMs, kMillisecondScale, &deadline);
    uke_gop_gpu_stats[kWaits] = uke_gop_gpu_stats[kWaits] + 1;
    IOLockLock(gLock);
    gClient = true;
    while (!gSwap.offered || gSwap.delivered) {
        if (IOLockSleepDeadline(gLock, &gSwap, deadline, THREAD_ABORTSAFE) != THREAD_AWAKENED) {IOLockUnlock(gLock); return kIOReturnTimeout;}
    }
    gSwap.delivered = true;
    IOMemoryDescriptor *memory = gSwap.memory;
    memory->retain();
    const uint64_t sequence = gSwap.sequence, stride = gSwap.stride;
    const uint32_t width = gSwap.width, height = gSwap.height, format = gSwap.format;
    IOMemoryMap *map = nullptr;
    uint32_t id = 0;
    for (Kept &k : gKept)
        if (k.map && k.memory == memory && k.task == task && k.map->getLength() == memory->getLength()) {
            map = k.map; map->retain(); id = k.id; k.used = ++gKeptUse; break;
        }
    IOLockUnlock(gLock);
    // The frame's pages go into the client's task (the GPU imports them for reading; the kernel driver pins and maintains them).
    if (!map) {
        map = memory->createMappingInTask(task, 0, kIOMapAnywhere);
        if (map && map->getVirtualAddress()) {
            IOLockLock(gLock);
            Kept *victim = &gKept[0];
            for (Kept &k : gKept) {if (!k.map) {victim = &k; break;} if (k.used < victim->used) victim = &k;}
            Kept old = *victim;
            memory->retain(); map->retain(); task_reference(task);
            id = ++gKeptId; if (!id) id = ++gKeptId;
            *victim = {memory, map, task, id, ++gKeptUse};
            IOLockUnlock(gLock);
            OSSafeReleaseNULL(old.map); OSSafeReleaseNULL(old.memory); if (old.task) task_deallocate(old.task);
        }
    }
    memory->release();
    IOLockLock(gLock);
    if (!map || !map->getVirtualAddress() || !gSwap.offered || gSwap.sequence != sequence) {
        IOLockUnlock(gLock);
        OSSafeReleaseNULL(map);
        return kIOReturnNoMemory;
    }
    gSwap.map = map;
    *out = {sequence, map->getVirtualAddress(), map->getLength(), stride, width, height, format, id};
    IOLockUnlock(gLock);
    return kIOReturnSuccess;
}

IOReturn completeSwap(uint64_t sequence, uint32_t status)
{
    Swap forgotten = {};
    IOLockLock(gLock);
    // A completion for a frame the display no longer waits for (it timed out and copied that frame itself, or forgot it after a
    // second) is accepted: answering an error made the plugin stop serving for good (log M2q 2026-11-07: "PX 34/1 e e00002c2").
    if (!gSwap.offered || !gSwap.delivered || gSwap.sequence != sequence) {
        IOLockUnlock(gLock);
        uke_gop_gpu_stats[kLateCompletes] = uke_gop_gpu_stats[kLateCompletes] + 1;
        return kIOReturnSuccess;
    }
    if (gSwap.abandoned) {
        clearSwapLocked(forgotten);   // the display already copied that frame itself
    } else {
        gSwap.done = true;
        gSwap.status = status ? -5 : 0;
        IOLockWakeup(gLock, &gSwap.done, false);
    }
    IOLockUnlock(gLock);
    releaseSwap(forgotten);
    return kIOReturnSuccess;
}
}

extern "C" IOReturn native_gop_handoff_call(uint32_t op, task_t task, const void *in, size_t inSize, void *out, size_t outSize)
{
    if (!gEnabled) return kIOReturnNotReady;
    switch (op) {
    case NGP_METHOD_MODE:
        if (inSize || !out || outSize != sizeof(NGPMode)) return kIOReturnBadArgument;
        memcpy(out, &gMode, sizeof gMode);
        return kIOReturnSuccess;
    case NGP_METHOD_WAIT_SWAP: {
        if (!in || inSize != sizeof(NGPWait) || !out || outSize != sizeof(NGPSwap)) return kIOReturnBadArgument;
        NGPWait q = {};
        NGPSwap swap = {};
        memcpy(&q, in, sizeof q);
        const IOReturn status = waitSwap(task, q.timeout_ms, &swap);
        if (status == kIOReturnSuccess) memcpy(out, &swap, sizeof swap);
        return status;
    }
    case NGP_METHOD_COMPLETE_SWAP: {
        if (!in || inSize != sizeof(NGPComplete) || outSize) return kIOReturnBadArgument;
        NGPComplete q = {};
        memcpy(&q, in, sizeof q);
        return completeSwap(q.sequence, q.status);
    }
    default:
        return kIOReturnUnsupported;
    }
}

// The scanout as a writable, retained descriptor; the accelerator's user client maps it into the plugin's task (type 0x5343).
extern "C" IOMemoryDescriptor *native_gop_handoff_memory(void)
{
    if (!gEnabled || !gScanout) return nullptr;
    gScanout->retain();
    return gScanout;
}
