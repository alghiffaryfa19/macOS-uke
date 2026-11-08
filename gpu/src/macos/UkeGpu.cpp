// SPDX-License-Identifier: BSD-2-Clause-Patent
// UkeGpu: macOS 26.5 entry for the uke Adreno 732 bring-up (base: our iPadOS 27 build). On iPadOS the LegacyDisplayProbe called
// sheng_gpu_power_probe() once the desktop was up; macOS has no such display probe, so this IOService (IOResources,
// like UkeTouchPointer) starts the same staged thread (GpuPowerProbe.cpp, boot-arg sheng-gpu-power=1..9). Every row
// the probe draws is also written as "UKE GPU<n>: ..." straight into macoslog (ulog below).
//
// Firmware (UKE_GPU_FIRMWARE_DRV=1): the gen71100 GMU/SQE/ZAP images are read from the raw table in the last MiB of
// the DRV window (boot/macos-gpu/make-drv.py writes it after the HFS+ volume, NativeUfs exports native_ufs_drv_read),
// checked against their SHA-256 and handed to the probe through UkeGpuFirmwareRemap.h. Without it they are linked in
// from firmware.S.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/crypto/sha2.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <sys/vnode.h>
#include <stdarg.h>
#include <sys/mount.h>

// macoslog only receives the console while the video console is the owner: from ~385 s on (WindowServer up) nothing
// printed through printf/IOLog reaches it any more (log M2g 2026-11-07, only the SHENG-SMP rows went on). The retained
// log writer itself is a kernel symbol the prelinker resolves (NativeUfs already imports sheng_* this way), so every UKE
// line is formatted here and written straight into it.
extern "C" void sheng_log_putc(char c);
extern "C" int vsnprintf(char *, size_t, const char *, va_list);
static void ulog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ulog(const char *fmt, ...)
{
    char line[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 1) n = sizeof line - 1;
    for (int i = 0; i < n; i++) sheng_log_putc(line[i]);
}

extern "C" {
void sheng_gpu_power_probe(void);
extern volatile uint32_t native_gpu_power_stage;
extern char native_gpu_coherence_row[120];
extern volatile uint64_t uke_gop_gpu_stats[8];   // UkeGopHandoff.cpp: offers, done, failed, late, refused, waits, late completes   // GpuPowerProbe.cpp coherenceProbe: "H ... -> on/off" or "H skip"
IOService *native_adreno_published_service(void);
IOReturn uke_gpu_publish_accelerator(void);

// iPadOS-only providers that the iPadOS 27 GpuPowerProbe.cpp, NativeAdrenoPublish.cpp and NativeAdrenoMemory.cpp
// name. On iPadOS they came from the boot progress page, the boot latency trace (BootLatencyTrace.c hooks private VM
// paths), the GPU addon loader, the patched pmap, the IOAcceleratorES accelerator (NativeAdrenoAccelerator.cpp) and the
// GOP presenter. The ForSheng prelinker takes no weak or unresolved import, so they are defined here: the trace is
// off, counters stay zero, the iOS accelerator is absent (macos/UkeGpuAccelerator.cpp is ours) and the GOP handoff
// refuses. native_hid_input_events comes from the touch driver in the same bundle (boot/macos-touch).
char boot_progress_ext[8][48];
char boot_progress_ext_wide[2][112];
volatile int native_gpu_addon_stage, native_gpu_addon_status, native_gpu_addon2_stage, native_gpu_addon2_status;
int boot_latency_enabled(void) { return 0; }
void boot_latency_start(void) {}
void boot_latency_record(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {}
// boot_log_export_put: the persistent pipeline cache below (UKE_GPU_FIRMWARE_DRV), else refused.
// Diagnostic only (which process still maps a BO page being freed); the macOS pmap has no such lookup.
unsigned int native_pmap_user_mappings(ppnum_t, void **, uint64_t *, char *) { return 0; }
void native_adreno_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp");
void native_adreno_accelerator_static_init(void) {}
IOService *native_adreno_accelerator_allocate(void) { return nullptr; }
unsigned native_adreno_accelerator_instance_size(void) { return 0; }
// native_gop_handoff_call / native_gop_handoff_memory: macos/UkeGopHandoff.cpp (sheng-fb-gpu=1).
// proc_iterate is not exported by the macOS kernel; the probe only lists processes for its exec observer row.
int proc_iterate(unsigned, int (*)(void *, void *), void *, int (*)(void *, void *), void *) { return 0; }
}

#if UKE_GPU_FIRMWARE_DRV
extern "C" IOReturn native_ufs_drv_read(uint64_t offset, void *out, uint32_t bytes);

namespace {
const uint8_t kNoImage[4] = {};
const uint64_t kNoBytes = 0;
}
extern "C" {
const uint8_t (*uke_fw_gmu)[] = reinterpret_cast<const uint8_t (*)[]>(&kNoImage);
const uint8_t (*uke_fw_sqe)[] = reinterpret_cast<const uint8_t (*)[]>(&kNoImage);
const uint8_t (*uke_fw_zap)[] = reinterpret_cast<const uint8_t (*)[]>(&kNoImage);
const uint64_t *uke_fw_gmu_size = &kNoBytes, *uke_fw_sqe_size = &kNoBytes, *uke_fw_zap_size = &kNoBytes;
}

namespace {
// boot/macos-gpu/drv.json "firmware_offset" (uke_gpu_build.py passes it): the last MiB of the window, after the volume.
#ifndef UKE_DRV_FIRMWARE_OFFSET
#error UKE_DRV_FIRMWARE_OFFSET comes from drv.json
#endif
constexpr uint64_t kTableOffset = UKE_DRV_FIRMWARE_OFFSET;
constexpr uint32_t kMaxImage = 512 * 1024;
struct __attribute__((packed)) DrvEntry { char name[32]; uint64_t offset, bytes; uint8_t sha256[32]; };
struct __attribute__((packed)) DrvTable { char magic[8]; uint32_t count, reserved; DrvEntry entry[8]; };
static_assert(sizeof(DrvTable) <= 4096, "one 4 KiB block");
uint64_t gBytes[3];

bool loadOne(const DrvTable &table, const char *role, const uint8_t (**image)[], const uint64_t **size, unsigned slot) {
    for (uint32_t i = 0; i < table.count && i < 8; ++i) {
        const DrvEntry &e = table.entry[i];
        if (strncmp(e.name, role, sizeof e.name)) continue;
        if (!e.bytes || e.bytes > kMaxImage || (e.offset & 4095)) break;
        const uint32_t rounded = uint32_t((e.bytes + 4095) & ~uint64_t(4095));
        uint8_t *buffer = static_cast<uint8_t *>(IOMallocAligned(rounded, 4096));
        if (!buffer) break;
        if (native_ufs_drv_read(e.offset, buffer, rounded) != kIOReturnSuccess) {
            IOFreeAligned(buffer, rounded);
            break;
        }
        SHA256_CTX context;
        uint8_t digest[32];
        SHA256_Init(&context);
        SHA256_Update(&context, buffer, size_t(e.bytes));
        SHA256_Final(digest, &context);
        if (memcmp(digest, e.sha256, sizeof digest)) {
            ulog("UkeGpu: DRV firmware %s fails its SHA-256\n", role);
            IOFreeAligned(buffer, rounded);
            return false;
        }
        gBytes[slot] = e.bytes;
        *image = reinterpret_cast<const uint8_t (*)[]>(buffer);
        *size = &gBytes[slot];
        ulog("UkeGpu: DRV firmware %s %llu bytes ok\n", role, e.bytes);
        return true;
    }
    ulog("UkeGpu: DRV firmware %s missing or unreadable\n", role);
    return false;
}

bool loadFirmware() {
    // The DRV window is published by NativeUfs's root thread; give it up to two minutes.
    DrvTable *table = static_cast<DrvTable *>(IOMallocAligned(4096, 4096));
    if (!table) return false;
    IOReturn status = kIOReturnNotReady;
    for (unsigned waited = 0; waited < 120 && (status = native_ufs_drv_read(kTableOffset, table, 4096)) == kIOReturnNotReady;
         ++waited)
        IOSleep(1000);
    bool ok = status == kIOReturnSuccess && !memcmp(table->magic, "UKEFW001", 8);
    if (!ok) ulog("UkeGpu: no DRV firmware table (status %#x)\n", status);
    ok = ok && loadOne(*table, "gmu", &uke_fw_gmu, &uke_fw_gmu_size, 0);
    ok = ok && loadOne(*table, "sqe", &uke_fw_sqe, &uke_fw_sqe_size, 1);
    ok = ok && loadOne(*table, "zap", &uke_fw_zap, &uke_fw_zap_size, 2);
    IOFreeAligned(table, 4096);
    return ok;
}
}
#endif

// Persistent pipeline cache (2026-11-07). The render server's translated shaders and Turnip pipelines live in its cache directory on the
// Data volume, a RAM overlay, so every boot compiled WindowServer's pipelines again ("cache hit5 miss290"). The plugin's export
// (CacheExport.h, method 0x7100 -> boot_log_export_put) is kept here in the DRV window, in two slots used in turn; a slot's header is
// cleared before its data is written and written last, so a cut never leaves a header over partial data. The next boot's plugin reads
// the newest slot back (uke_cache_read, method 0x7101) into its cache directory before the device opens.
#if UKE_GPU_FIRMWARE_DRV
extern "C" IOReturn native_ufs_drv_write(uint64_t offset, const void *in, uint32_t bytes);
namespace ukecache {
constexpr uint64_t kSlot[2] = {1ull << 20, 256ull << 20};
constexpr uint64_t kSlotData = (255ull << 20) - 4096;   // a slot: one header block, then the export blob
constexpr uint32_t kStage = 256 * 1024, kWindow = 64 * 1024;
struct __attribute__((packed)) Header {char magic[8]; uint32_t version, generation; uint64_t bytes; uint32_t dataCrc, headerCrc;};
IOLock *gLock;
int gLatest = -1;
uint32_t gLatestGeneration;
uint64_t gLatestBytes;
bool gActive;
int gSlot;
uint32_t gPutGeneration, gCrc, gStaged;
uint64_t gTotal, gNext, gWritten;
uint8_t *gStage, *gWindowBuf;
uint64_t gWindowAt = ~0ull;
}
// [0] pieces stored, [1] pieces refused, [2] exports committed, [3] last committed bytes, [4] slot found at boot (+1, 0 none),
// [5] reads served, [6] read errors, [7] write errors
extern "C" {volatile uint32_t uke_cache_stats[8];}
namespace ukecache {
uint32_t crc32(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    while (n--) {crc ^= *p++; for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));}
    return ~crc;
}
bool readHeader(int slot, Header &h) {
    uint8_t *block = static_cast<uint8_t *>(IOMallocAligned(4096, 4096));
    if (!block) return false;
    const bool ok = native_ufs_drv_read(kSlot[slot], block, 4096) == kIOReturnSuccess;
    memcpy(&h, block, sizeof h);
    IOFreeAligned(block, 4096);
    return ok && !memcmp(h.magic, "UKECACH1", 8) && h.version == 1 && h.bytes && h.bytes <= kSlotData &&
        h.headerCrc == crc32(0, reinterpret_cast<const uint8_t *>(&h), offsetof(Header, headerCrc));
}
bool writeHeader(int slot, const Header *h) {
    uint8_t *block = static_cast<uint8_t *>(IOMallocAligned(4096, 4096));
    if (!block) return false;
    memset(block, 0, 4096);
    if (h) memcpy(block, h, sizeof *h);
    const bool ok = native_ufs_drv_write(kSlot[slot], block, 4096) == kIOReturnSuccess;
    IOFreeAligned(block, 4096);
    return ok;
}
void init() {
    gLock = IOLockAlloc();
    gStage = static_cast<uint8_t *>(IOMallocAligned(kStage, 4096));
    gWindowBuf = static_cast<uint8_t *>(IOMallocAligned(kWindow, 4096));
    Header h[2] = {};
    const bool valid[2] = {readHeader(0, h[0]), readHeader(1, h[1])};
    if (valid[0] || valid[1]) {
        gLatest = valid[0] && (!valid[1] || int32_t(h[0].generation - h[1].generation) > 0) ? 0 : 1;
        gLatestGeneration = h[gLatest].generation;
        gLatestBytes = h[gLatest].bytes;
        uke_cache_stats[4] = uint32_t(gLatest + 1);
    }
    ulog("UKE CACHE: slots %s/%s, using %d (generation %u, %llu bytes)\n", valid[0] ? "ok" : "-", valid[1] ? "ok" : "-", gLatest,
         gLatestGeneration, gLatestBytes);
}
bool flush(bool last) {
    uint32_t bytes = last ? (gStaged + 4095) & ~4095u : gStaged & ~4095u;
    if (!bytes) return true;
    if (last && bytes > gStaged) memset(gStage + gStaged, 0, bytes - gStaged);
    if (native_ufs_drv_write(kSlot[gSlot] + 4096 + gWritten, gStage, bytes) != kIOReturnSuccess) return false;
    gWritten += bytes;
    const uint32_t kept = gStaged > bytes ? gStaged - bytes : 0;
    if (kept) memmove(gStage, gStage + bytes, kept);
    gStaged = kept;
    return true;
}
int put(uint32_t generation, uint32_t total, uint32_t offset, const uint8_t *data, uint32_t size) {
    if (!gLock || !gStage) return -1;
    IOLockLock(gLock);
    int result = -1;
    if (offset == 0) {
        gActive = total && total <= kSlotData;
        gSlot = gLatest == 0 ? 1 : 0;
        gPutGeneration = generation; gTotal = total; gNext = gWritten = 0; gCrc = 0; gStaged = 0;
        // The slot about to be filled loses its header first: an older export there must not survive with half-new data under it.
        if (gActive && !writeHeader(gSlot, nullptr)) {gActive = false; uke_cache_stats[7] = uke_cache_stats[7] + 1;}
    }
    if (gActive && generation == gPutGeneration && offset == gNext && size && size <= gTotal - gNext && size <= kStage - gStaged) {
        memcpy(gStage + gStaged, data, size);
        gStaged += size; gNext += size; gCrc = crc32(gCrc, data, size);
        bool ok = gStaged + 4096 < kStage || flush(false);
        if (ok && gNext == gTotal) {
            ok = flush(true);
            Header h = {};
            memcpy(h.magic, "UKECACH1", 8);
            h.version = 1; h.generation = gLatestGeneration + 1; h.bytes = gTotal; h.dataCrc = gCrc;
            h.headerCrc = crc32(0, reinterpret_cast<const uint8_t *>(&h), offsetof(Header, headerCrc));
            ok = ok && writeHeader(gSlot, &h);
            if (ok) {
                gLatest = gSlot; gLatestGeneration = h.generation; gLatestBytes = gTotal; gWindowAt = ~0ull;
                uke_cache_stats[2] = uke_cache_stats[2] + 1; uke_cache_stats[3] = uint32_t(gTotal);
            }
            gActive = false;
        }
        if (!ok) {gActive = false; uke_cache_stats[7] = uke_cache_stats[7] + 1;}
        else result = 0;
    } else gActive = false;
    uke_cache_stats[result ? 1 : 0] = uke_cache_stats[result ? 1 : 0] + 1;
    IOLockUnlock(gLock);
    return result;
}
}
extern "C" int boot_log_export_put(uint32_t generation, uint32_t total, uint32_t offset, const void *data, uint32_t size) {
    return ukecache::put(generation, total, offset, static_cast<const uint8_t *>(data), size);
}
// The newest committed export: *total is its size; up to `size` bytes from `offset` into out. 0 on success.
extern "C" int uke_cache_read(uint32_t offset, void *out, uint32_t size, uint32_t *total) {
    using namespace ukecache;
    if (!gLock || !gWindowBuf || !total) return -1;
    IOLockLock(gLock);
    int result = -1;
    *total = gLatest < 0 ? 0 : uint32_t(gLatestBytes);
    if (gLatest >= 0 && offset <= gLatestBytes && size <= gLatestBytes - offset) {
        result = 0;
        for (uint32_t done = 0; done < size && !result;) {
            const uint64_t at = offset + done, window = at & ~uint64_t(kWindow - 1);
            if (window != gWindowAt) {
                gWindowAt = ~0ull;
                if (native_ufs_drv_read(kSlot[gLatest] + 4096 + window, gWindowBuf, kWindow) != kIOReturnSuccess) {result = -1; break;}
                gWindowAt = window;
            }
            const uint32_t k = uint32_t(kWindow - (at - window)) < size - done ? uint32_t(kWindow - (at - window)) : size - done;
            memcpy(static_cast<uint8_t *>(out) + done, gWindowBuf + (at - window), k);
            done += k;
        }
    }
    uke_cache_stats[result ? 6 : 5] = uke_cache_stats[result ? 6 : 5] + 1;
    IOLockUnlock(gLock);
    return result;
}
#else
extern "C" {volatile uint32_t uke_cache_stats[8];}
extern "C" int boot_log_export_put(uint32_t, uint32_t, uint32_t, const void *, uint32_t) { return -1; }
extern "C" int uke_cache_read(uint32_t, void *, uint32_t, uint32_t *total) { if (total) *total = 0; return -1; }
#endif

// The driver's own records of the Metal path (NativeAdrenoPublish.cpp): who read MetalPluginName (the discovery), each
// IOServiceOpen and its gates, per-process opens, and the last failure line a plugin streamed.
extern "C" {
struct NativeAdrenoOpenRecord {char name[20]; uint32_t type, macf, res, attach, foreign;};
struct NativeAdrenoClientRecord {char name[20]; uint32_t opens, refused, closes, slot, peak_mib, last_refusal;};
struct NativeAdrenoClientText {char name[20]; char text[64];};
struct NativeAdrenoAskerRecord {char name[20]; uint32_t reads;};
extern struct NativeAdrenoOpenRecord native_adreno_open_log[8];
extern volatile uint32_t native_adreno_open_log_count;
extern struct NativeAdrenoClientRecord native_adreno_client_log[24];
extern struct NativeAdrenoClientText native_adreno_client_text[24];
extern struct NativeAdrenoAskerRecord native_adreno_metal_askers[12];
extern volatile uint32_t native_adreno_mount_fail[4];
}

namespace {
// Phase 2 (uke-gpu-metal): one "UKE MTL" block every 30 s, so the latest state is always in the retained tail.
// Does the plugin path resolve (the /Library/GPUBundles link into the mounted UKEDRV volume)? 0 = yes, else errno.
int bundleLookup(const char *path) {
    vnode_t root = vfs_rootvnode();   // no lookups before the root file system is mounted
    if (!root) return -1;
    vnode_put(root);
    vnode_t vp = nullptr;
    vfs_context_t ctx = vfs_context_create(nullptr);
    const int error = vnode_lookup(path, 0, &vp, ctx);
    if (!error) vnode_put(vp);
    vfs_context_rele(ctx);
    return error;
}

// The render server's own state (NativeAdrenoPublish.cpp, same layout): its result line, its Turnip-phase line, the processes that
// started the plugin, and the telemetry rows the Metal bridge streams (frames, passes, failures). Printed only when they change.
extern "C" {
struct NativeAdrenoMetalDiag {
    uint32_t opens, busy, bad, closes, reports, property_reads, foreign_opens, foreign_type, name_reads, calls, last_selector, last_status,
        ext_entries, ext_selector, ext_status, ext_in, ext_out, tunnel_calls, probes, sel_bits[8], mb_maps, mb_calls, marks_total, marks[16],
        chk_macf, chk_res, chk_type, type_pass_lo, type_pass_hi, attach_status, inflight, inflight_since_calls, forced_unmaps;
    char opener[3][17];
    char reader[3][17];
    char name_reader[17];
    char starter[3][17];
    char report[64];
    char report2[64];
    uint32_t grants;
};
extern struct NativeAdrenoMetalDiag native_adreno_metal_diag;
extern char native_adreno_metal_rows[48][96];
extern volatile uint32_t native_adreno_metal_rows_seq;
extern volatile uint32_t sheng_recovery_ui_page_mask;   // ForSheng recovery/ui_policy_kernel.h: bit i = UI policy i applied
}
char gLastReport[64], gLastReport2[64], gLastRows[48][96];
uint32_t gLastRowsSeq = ~0u, gLastUiMask = ~0u;

void dumpRenderServer(unsigned t) {
    const NativeAdrenoMetalDiag &d = native_adreno_metal_diag;
    if (sheng_recovery_ui_page_mask != gLastUiMask) {
        gLastUiMask = sheng_recovery_ui_page_mask;
        ulog("UKE RS: t=%us ui policies applied mask %#x\n", t, gLastUiMask);
    }
    if (strncmp(d.report, gLastReport, sizeof gLastReport) || strncmp(d.report2, gLastReport2, sizeof gLastReport2)) {
        memcpy(gLastReport, d.report, sizeof gLastReport);
        memcpy(gLastReport2, d.report2, sizeof gLastReport2);
        ulog("UKE RS: t=%us report \"%.63s\" report2 \"%.63s\" starters %.16s,%.16s,%.16s marks %u grants %u\n", t, d.report,
             d.report2, d.starter[0], d.starter[1], d.starter[2], d.marks_total, d.grants);
    }
    if (native_adreno_metal_rows_seq == gLastRowsSeq) return;
    gLastRowsSeq = native_adreno_metal_rows_seq;
    for (unsigned r = 0; r < 48; ++r) {
        if (!native_adreno_metal_rows[r][0] || !memcmp(native_adreno_metal_rows[r], gLastRows[r], sizeof gLastRows[r])) continue;
        memcpy(gLastRows[r], native_adreno_metal_rows[r], sizeof gLastRows[r]);
        ulog("UKE RS%u: %.95s\n", r, native_adreno_metal_rows[r]);
    }
}

void dumpMetal(unsigned metal) {
    ulog("UKE MTL: path GPUBundles %d UKEDRV %d\n",
          bundleLookup("/Library/GPUBundles/NativeAdrenoMetal.bundle/Contents/Info.plist"),
          bundleLookup("/Volumes/UKEDRV/NativeAdrenoMetal.bundle/Contents/Info.plist"));
    ulog("UKE MTL: accel %s opens %u mountfail %u/%#x/%u\n", metal == 2 ? "published" : "FAILED",
          native_adreno_open_log_count, native_adreno_mount_fail[0], native_adreno_mount_fail[1], native_adreno_mount_fail[3]);
    for (unsigned i = 0; i < 12; ++i)
        if (native_adreno_metal_askers[i].name[0])
            ulog("UKE MTL ask: %.20s x%u\n", native_adreno_metal_askers[i].name, native_adreno_metal_askers[i].reads);
    for (unsigned i = 0; i < 8; ++i) {
        const NativeAdrenoOpenRecord &o = native_adreno_open_log[i];
        if (o.name[0]) ulog("UKE MTL open: %.20s type %#x macf %u res %u attach %u foreign %u\n", o.name, o.type, o.macf, o.res, o.attach, o.foreign);
    }
    for (unsigned i = 0; i < 24; ++i) {
        const NativeAdrenoClientRecord &c = native_adreno_client_log[i];
        if (c.name[0]) ulog("UKE MTL client: %.20s opens %u refused %u closes %u slot %u peak %uMiB last %#x\n", c.name, c.opens, c.refused, c.closes, c.slot, c.peak_mib, c.last_refusal);
    }
    for (unsigned i = 0; i < 24; ++i)
        if (native_adreno_client_text[i].name[0])
            ulog("UKE MTL text: %.20s: %.64s\n", native_adreno_client_text[i].name, native_adreno_client_text[i].text);
}

// UKEDRV never showed up at /Volumes/UKEDRV (log M2h 2026-11-07, 17 min): list every IOMedia (is the DRV window, its
// GPT partition and the APFS volume there at all?) and every mounted file system, straight into macoslog.
int dumpMount(mount_t mp, void *) {
    vfsstatfs *st = vfs_statfs(mp);
    if (st) ulog("UKE MNT: %.40s on %.48s (%.15s)\n", st->f_mntfromname, st->f_mntonname, st->f_fstypename);
    return VFS_RETURNED;
}

void dumpStorage(unsigned t) {
    ulog("UKE DSK: t=%us\n", t);
    OSIterator *it = IOService::getMatchingServices(IOService::serviceMatching("IOMedia"));
    if (it) {
        while (IORegistryEntry *m = OSDynamicCast(IORegistryEntry, it->getNextObject())) {
            OSString *bsd = OSDynamicCast(OSString, m->getProperty("BSD Name"));
            OSString *content = OSDynamicCast(OSString, m->getProperty("Content"));
            OSNumber *size = OSDynamicCast(OSNumber, m->getProperty("Size"));
            IORegistryEntry *parent = m->getParentEntry(gIOServicePlane);
            ulog("UKE DSK: %.12s %.24s %llu MiB whole %d leaf %d content %.40s parent %.28s name %.28s\n",
                 bsd ? bsd->getCStringNoCopy() : "-", m->getMetaClass()->getClassName(),
                 size ? size->unsigned64BitValue() >> 20 : 0, m->getProperty("Whole") == kOSBooleanTrue,
                 m->getProperty("Leaf") == kOSBooleanTrue, content ? content->getCStringNoCopy() : "-",
                 parent ? parent->getMetaClass()->getClassName() : "-", m->getName());
        }
        it->release();
    }
    vfs_iterate(0, dumpMount, nullptr);
}

// A kernel mount of UKEDRV is not possible: apfs_vfsop_mount treats every mount from kernel_task as a root mount
// (container_rootmount), whose qualification fails once two containers (ISC, OS) are loaded unless the disk is virtual
// (apfs 2811.120.14 disassembly, log M2i: "check_rootmount_qualification() returned false", EBUSY). UKEDRV is mounted
// by diskarbitrationd from the Data fstab line, which works once the DRV window is writable (NativeUfs RAM overlay).
// Every client's latest text row (its latest real Metal problem, split in parts 4 s apart) and the render server's report2, sampled
// every second so no part is lost between the 30 s dumps.
char gLastText[24][64], gLastTextName[24][20], gLastRs2[64];
void sampleTexts(unsigned t) {
    for (unsigned i = 0; i < 24; ++i) {
        const NativeAdrenoClientText &c = native_adreno_client_text[i];
        if (!c.name[0] || (!memcmp(c.text, gLastText[i], sizeof gLastText[i]) && !memcmp(c.name, gLastTextName[i], sizeof gLastTextName[i])))
            continue;
        memcpy(gLastText[i], c.text, sizeof gLastText[i]);
        memcpy(gLastTextName[i], c.name, sizeof gLastTextName[i]);
        ulog("UKE TXT: t=%us %.20s: %.64s\n", t, c.name, c.text);
    }
    if (memcmp(native_adreno_metal_diag.report2, gLastRs2, sizeof gLastRs2)) {
        memcpy(gLastRs2, native_adreno_metal_diag.report2, sizeof gLastRs2);
        ulog("UKE TXT: t=%us (row 9+): %.64s\n", t, gLastRs2);
    }
}

// GPU hang snapshot (2026-11-07). Under many Metal clients BR stops in the SQE's opcode-error trap (PC 0x215: it read a zero word as
// a packet header, CP_INTERRUPT_STATUS bit 0) and BV waits in THREAD_SYNC for it. The driver already captures the CP state, the words
// each thread was reading (through the stalled job's address space) and the last submissions; nothing of it reached macoslog. Once per
// new stalled fence (at most three per boot) everything is written here: who submitted the job, what its tables translate the fetch
// addresses to, the ring around the read pointer and the recent history.
extern "C" {
extern volatile uint32_t native_adreno_stall[2 + 22], native_adreno_stall_mem[4 + 3 * 17 + 4];
extern volatile uint32_t native_adreno_stall_job[24], native_adreno_stall_ib2[17];
extern volatile uint32_t native_adreno_fence_wait[8], native_adreno_unstick[15], native_adreno_table_switches[8];
extern volatile uint64_t native_adreno_stall_log[32][8];
extern volatile uint32_t native_adreno_stall_log_count;
extern volatile uint32_t native_adreno_ring_log[4][4 + 112], native_adreno_ring_log_count;
extern volatile uint32_t native_adreno_ib_log[4][4 + 28], native_adreno_ib_log_count;
extern volatile uint32_t native_adreno_recover_log[8][12], native_adreno_recover_log_count;
extern volatile uint64_t (*native_adreno_tl)[8];
extern volatile uint32_t native_adreno_tl_cap, native_adreno_tl_count;
extern volatile uint32_t native_adreno_wc_audit[6];
void proc_name(int pid, char *buf, int size);
extern volatile uint32_t native_adreno_stall_ring[1 + 48], native_adreno_stall_sqe[4], native_adreno_stall_gpuview[1 + 16];
extern volatile uint32_t native_uke_ib_publish[3];
extern volatile uint32_t native_uke_ib_zero[12];
extern volatile uint32_t native_uke_replay_skipped;
extern volatile uint32_t native_uke_wc_zero_fill[2];
extern volatile uint32_t native_gpu_dcvs[15], native_adreno_submit_ibs[4];
extern volatile uint64_t native_adreno_maint_phase[8][2];
extern "C" volatile uint64_t native_uke_record_head_only;
extern "C" volatile uint64_t native_uke_wait_classes[17];
extern "C" volatile uint64_t uke_ufs_io[10];
extern "C" volatile uint64_t uke_ufs_slots[4];
extern "C" volatile uint64_t uke_persist[6];
extern "C" volatile uint64_t native_uke_wait_procs[9][6];
extern "C" volatile char native_uke_wait_names[8][16];
extern "C" volatile uint64_t native_gpu_idle_sleeps, native_gpu_idle_spins, native_gpu_idle_timeouts;
extern volatile uint32_t native_adreno_io_coherent;
extern volatile uint32_t native_adreno_wc_distrusted;
}

void ulogWords(const char *tag, const volatile uint32_t *w, unsigned n) {
    for (unsigned at = 0; at < n; at += 12) {
        char line[160];
        int k = snprintf(line, sizeof line, "%s+%u:", tag, at);
        for (unsigned i = at; i < n && i < at + 12 && k < (int)sizeof line - 10; ++i) k += snprintf(line + k, sizeof line - k, " %08x", w[i]);
        ulog("%s\n", line);
    }
}

bool allDead(const volatile uint32_t *w, unsigned n) {
    for (unsigned i = 0; i < n; ++i) if (w[i] != 0xdeadbeef) return false;
    return true;
}
constexpr unsigned kStallDumps = 10;
uint32_t gStallDumped[kStallDumps], gStallDumps;
void dumpStall(unsigned t) {
    const uint32_t fence = native_adreno_stall[0];
    if (!fence || native_adreno_stall_job[0] != fence || gStallDumps >= kStallDumps) return;
    for (unsigned i = 0; i < gStallDumps; ++i) if (gStallDumped[i] == fence) return;
    gStallDumped[gStallDumps++] = fence;
    const volatile uint32_t *r = &native_adreno_stall[2], *j = native_adreno_stall_job, *m = native_adreno_stall_mem;
    char who[20] = "?";
    if (j[1]) proc_name((int)j[1], who, sizeof who);
    ulog("UKE HANG%u: t=%us fence %u waits %u pid %u (%.19s) ring %u ctx %u ibs %u asid %u loaded %u age %ums\n", gStallDumps, t, fence,
         native_adreno_stall[1], j[1], who, j[2], j[3], j[4], j[5], j[6], j[7]);
    ulog("UKE HANG regs: RBBM %08x %08x %08x %08x 201 %08x CP int %08x 825 %08x prot %08x rptr %x wptr %x\n", r[0], r[1], r[2], r[3], r[4],
         r[5], r[6], r[7], r[8], r[9]);
    ulog("UKE HANG regs: BR IB1 %08x%08x rem %x IB2 %08x%08x rem %x BV rptr %x IB1 %08x rem %x a74 %08x a71 %08x a72 %08x\n", r[11], r[10],
         r[12], r[14], r[13], r[15], r[16], r[17], r[18], r[19], r[20], r[21]);
    ulog("UKE HANG sqe: br pc %x pkt %08x bv pc %x pkt %08x\n", native_adreno_stall_sqe[0], native_adreno_stall_sqe[1],
         native_adreno_stall_sqe[2], native_adreno_stall_sqe[3]);
    ulog("UKE HANG smmu: fsr %08x far %08x%08x fsynr %08x | handles next %u br %u bv %u bv2 %u\n", m[0], m[2], m[1], m[3], m[55], m[56],
         m[57], m[58]);
    for (unsigned f = 0; f < 2; ++f) {
        const volatile uint32_t *o = &j[8 + 7 * f];
        ulog("UKE HANG walk %s: va %08x%08x desc %08x%08x level %u pa %08x%08x\n", f ? "IB2" : "IB1", o[1], o[0], o[3], o[2], o[4], o[6], o[5]);
    }
    ulog("UKE HANG ring table %08x asid %u | table switches cp %u cpu %u made %u freed %u live %u peak %u fail %x/%u\n", j[22], j[23],
         native_adreno_table_switches[0], native_adreno_table_switches[1], native_adreno_table_switches[2], native_adreno_table_switches[3],
         native_adreno_table_switches[4], native_adreno_table_switches[5], native_adreno_table_switches[6], native_adreno_table_switches[7]);
    ulog("UKE HANG BR IB1 from dword %x:\n", m[4]);
    ulogWords("UKE HANG br1", &m[5], 16);
    ulog("UKE HANG BR IB1 as the GPU translates it, from dword %x:\n", native_adreno_stall_gpuview[0]);
    ulogWords("UKE HANG gpu1", &native_adreno_stall_gpuview[1], 16);
    ulog("UKE HANG BV IB1 from dword %x:\n", m[4 + 17]);
    ulogWords("UKE HANG bv1", &m[5 + 17], 16);
    if (!allDead(&m[5 + 34], 16)) {ulog("UKE HANG BV IB2 from %x:\n", m[4 + 34]); ulogWords("UKE HANG bv2", &m[5 + 34], 16);}
    if (!allDead(&native_adreno_stall_ib2[1], 16)) {
        ulog("UKE HANG BR IB2 from %x:\n", native_adreno_stall_ib2[0]);
        ulogWords("UKE HANG br2", &native_adreno_stall_ib2[1], 16);
    }
    // The ring around the read pointer at the moment of the snapshot (a GPU restart empties it soon after).
    ulog("UKE HANG ring from %x (rptr %x):\n", native_adreno_stall_ring[0], r[8]);
    ulogWords("UKE HANG rb", &native_adreno_stall_ring[1], 48);
    ulog("UKE HANG waits: calls %u timeouts %u fence %u seen %u newest %u done %u orphan %u refused %u\n",
         native_adreno_fence_wait[0], native_adreno_fence_wait[1], native_adreno_fence_wait[2], native_adreno_fence_wait[3],
         native_adreno_fence_wait[4], native_adreno_fence_wait[5], native_adreno_fence_wait[6], native_adreno_fence_wait[7]);
    ulog("UKE HANG unstick %u/%u/%u/%u drains %u | wc audits %u dirty %u stale %u reset %u\n", native_adreno_unstick[0],
         native_adreno_unstick[1], native_adreno_unstick[2], native_adreno_unstick[3], native_adreno_unstick[4], native_adreno_wc_audit[0],
         native_adreno_wc_audit[2], native_adreno_wc_audit[3], native_adreno_wc_audit[5]);
    for (unsigned k = 0; k < 4; ++k) {
        const volatile uint32_t *s = native_adreno_ring_log[(native_adreno_ring_log_count - 1 - k) & 3];
        if (!s[0]) continue;
        ulog("UKE HANG sub-%u: fence %u ring %x..%x words %u\n", k, s[0], s[1], s[2], s[3]);
        ulogWords("UKE HANG sw", &s[4], s[3] < 48 ? s[3] : 48);
    }
    for (unsigned k = 0; k < 4; ++k) {
        const volatile uint32_t *s = native_adreno_ib_log[(native_adreno_ib_log_count - 1 - k) & 3];
        if (!s[0]) continue;
        ulog("UKE HANG ib-%u: fence %u iova %08x%08x bytes %u\n", k, s[0], s[2], s[1], s[3]);
        ulogWords("UKE HANG iw", &s[4], 28);
    }
    for (unsigned k = 0; k < 16 && k < native_adreno_stall_log_count; ++k) {
        const volatile uint64_t *s = native_adreno_stall_log[(native_adreno_stall_log_count - 1 - k) & 31];
        ulog("UKE HANG ev-%u: us %llu fence %llu ev %llu waits %llu pend %llu rw %llx queue %llx buf %llx seq %llu\n", k, s[0], s[1],
             s[2] & 0xff, s[2] >> 8, s[3], s[4], s[5], s[6], s[7]);
    }
    for (unsigned k = 0; native_adreno_tl && native_adreno_tl_cap && k < 24 && k < native_adreno_tl_count; ++k) {
        const volatile uint64_t *s = native_adreno_tl[(native_adreno_tl_count - 1 - k) & (native_adreno_tl_cap - 1)];
        char name[20] = "?";
        const int pid = (int)(uint32_t)(s[4] >> 32);
        if (pid) proc_name(pid, name, sizeof name);
        ulog("UKE HANG job-%u: fence %u ibs %u pid %d (%.19s) submit %llu retired %llu gpu %llu ring %llu ctl %llx\n", k, (uint32_t)s[0],
             (uint32_t)(s[0] >> 32) & 0x3ffffff, pid, name, s[1], s[5], s[3] > s[2] ? s[3] - s[2] : 0, s[6] & 15, s[6]);
    }
    for (unsigned k = 0; k < 2 && k < native_adreno_recover_log_count; ++k) {
        const volatile uint32_t *s = native_adreno_recover_log[(native_adreno_recover_log_count - 1 - k) & 7];
        ulog("UKE HANG recover-%u: n %u us %u fence %u status %x replayed %u flags %x dead %u info %x/%x/%x/%x\n", k, s[0], s[1], s[3], s[4],
             s[5], s[6], s[7], s[8], s[9], s[10], s[11]);
    }
    ulog("UKE HANG%u end\n", gStallDumps);
}

// One line every 30 s to compare configurations by numbers (2026-11-07): submissions and IBs per second, WindowServer's scanout frames,
// the GPU's busy share and level residency (DCVS, its last 10 s window) and the KiB of CPU cache maintenance per phase since the last line.
struct PerfMark {uint32_t submits, ibs, waits, timeouts; uint64_t offers, done; uint64_t maint[8][2];} gPerf;
void dumpPerf(unsigned t) {
    PerfMark now{};
    now.submits = native_adreno_submit_ibs[0]; now.ibs = native_adreno_submit_ibs[1];
    now.waits = native_adreno_fence_wait[0]; now.timeouts = native_adreno_fence_wait[1];
    now.offers = uke_gop_gpu_stats[0]; now.done = uke_gop_gpu_stats[1];
    for (unsigned i = 0; i < 8; ++i) {now.maint[i][0] = native_adreno_maint_phase[i][0]; now.maint[i][1] = native_adreno_maint_phase[i][1];}
    const volatile uint32_t *d = native_gpu_dcvs;
    ulog("UKE PERF: t=%us submit/s %u ib/s %u frames %llu/%llu waits %u timeouts %u coherent %u\n", t,
         (now.submits - gPerf.submits) / 30, (now.ibs - gPerf.ibs) / 30, now.done - gPerf.done, now.offers - gPerf.offers,
         now.waits - gPerf.waits, now.timeouts - gPerf.timeouts, native_adreno_io_coherent);
    ulog("UKE PERF: t=%us gpu busy %u%% level %u | %% at MHz 220:%u 295:%u 348:%u 401:%u 475:%u 550:%u 615:%u 680:%u\n", t, d[4], d[1],
         d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12]);
    ulog("UKE PERF: t=%us maint KiB alloc %llu map %llu submit %llu (%llu calls) unmap %llu complete %llu table %llu\n", t,
         now.maint[1][1] - gPerf.maint[1][1], now.maint[2][1] - gPerf.maint[2][1], now.maint[3][1] - gPerf.maint[3][1],
         now.maint[3][0] - gPerf.maint[3][0], now.maint[4][1] - gPerf.maint[4][1], now.maint[5][1] - gPerf.maint[5][1],
         now.maint[6][1] - gPerf.maint[6][1]);
    {
        // Storage since boot (NativeUfs uke_ufs_io): requests the system made and the media reads behind them; ms from 19.2 MHz ticks.
        const volatile uint64_t *u = uke_ufs_io;
        const uint64_t mediaMs = u[8] / 19200;
        ulog("UKE UFS: t=%us rd %llu req %llu MiB %llu ms max %llu ms | wr %llu req %llu MiB %llu ms | media %llu cmd %llu MiB %llu ms (%llu MiB/s)\n",
             t, u[0], u[1] >> 20, u[2] / 19200, u[9] / 19200, u[3], u[4] >> 20, u[5] / 19200, u[6], u[7] >> 20, mediaMs,
             mediaMs ? (u[7] >> 20) * 1000 / mediaMs : 0);
        ulog("UKE UFS: t=%us read slots %llu, reads through them %llu, most in flight %llu, waits for a slot %llu\n", t,
             uke_ufs_slots[0], uke_ufs_slots[1], uke_ufs_slots[2], uke_ufs_slots[3]);
    }
    {
        // OS window changes on the UFS (NativeUfs RootWindowPersist.h): state 0 off 1 refused 2 resumed 3 fresh 4 failed 5 write failed.
        const volatile uint64_t *p = uke_persist;
        ulog("UKE PERSIST: t=%us state %llu pages %llu (%llu MiB) writes %llu header %llu lba %llu gen %llu\n", t, p[0], p[1], p[1] >> 8,
             p[2], p[3], p[4], p[5]);
    }
    ulog("UKE PERF: t=%us preempt record passes cut to their first page %llu (uke-gpu-record-publish=1 restores whole)\n", t,
         native_uke_record_head_only);
    {
        // Fence waits by requested timeout (0 poll, <=1 ms, <=20 ms, longer): completed/timed out and the ms spent, since boot.
        const volatile uint64_t *w = native_uke_wait_classes;
        ulog("UKE WAIT: t=%us ok %llu/%llu/%llu/%llu to %llu/%llu/%llu/%llu | ms ok %llu/%llu/%llu/%llu to %llu/%llu/%llu/%llu max %llu | idle sleep %llu spin %llu tmo %llu\n",
             t, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8] / 1000, w[9] / 1000, w[10] / 1000, w[11] / 1000,
             w[12] / 1000, w[13] / 1000, w[14] / 1000, w[15] / 1000, w[16] / 1000, native_gpu_idle_sleeps, native_gpu_idle_spins,
             native_gpu_idle_timeouts);
        // Per process: polls (timed out) and waits (timed out), ms waited and the longest wait in ms.
        for (unsigned i = 0; i < 9; ++i) {
            const volatile uint64_t *p = native_uke_wait_procs[i];
            if (!p[0] && !p[2]) continue;
            ulog("UKE WAITP: t=%us %.16s poll %llu (%llu) wait %llu (%llu) ms %llu max %llu\n", t,
                 i < 8 ? (const char *)native_uke_wait_names[i] : "other", p[0], p[1], p[2], p[3], p[4] / 1000, p[5] / 1000);
        }
    }
    gPerf = now;
}

extern "C" unsigned uke_cpufreq_apply(void);
extern "C" char uke_cpufreq_row[160];
void bringUp(void *, wait_result_t) {
    // CPU clusters to their top non-turbo level before the GPU bring-up (UkeCpuFreq.cpp, sheng-cpufreq=3).
    if (uke_cpufreq_apply()) ulog("UKE CPU: t=0s %.159s\n", uke_cpufreq_row);
#if UKE_GPU_FIRMWARE_DRV
    // Without firmware the probe still runs the power and clock stages and stops at the GMU cold start (mode 7).
    if (!loadFirmware()) ulog("UkeGpu: continuing without firmware\n");
    ukecache::init();
#endif
    sheng_gpu_power_probe();
    // Phase 2 (uke-gpu-metal=1): once the probe has published NativeAdrenoGpu (mode 9, after "C10ok"), make it visible
    // to Metal's discovery (UkeGpuAccelerator.cpp).
    unsigned metal = 0;
    PE_parse_boot_argn("uke-gpu-metal", &metal, sizeof metal);
    // The iPadOS 27 probe writes its result rows only into the boot progress page (boot_progress_ext), which nothing
    // draws on macOS: print the stage word and every row every 30 s for twenty minutes, so the latest ones are always
    // in the retained tail of macoslog (the boot head is overwritten).
    for (unsigned i = 0; i < 40; ++i) {
        ulog("UKE GPU: t=%us stage %#x\n", i * 30, native_gpu_power_stage);
        // The rows settle within the first minutes; after that repeat them only every 2 minutes (the tail is ~80 KB).
        for (unsigned r = 0; r < 8 && (i < 8 || i % 4 == 0); ++r)
            if (boot_progress_ext[r][0] && boot_progress_ext[r][0] != ' ') ulog("UKE GPU%u: %.47s\n", r, boot_progress_ext[r]);
        for (unsigned r = 0; r < 2; ++r)
            if (boot_progress_ext_wide[r][0]) ulog("UKE GPUW%u: %.111s\n", r, boot_progress_ext_wide[r]);
        if (native_gpu_coherence_row[0] && (i < 8 || i % 4 == 0)) ulog("UKE GPUH: %.119s\n", native_gpu_coherence_row);
        // Publish the accelerator only once the plugin path resolves (UKEDRV mounted by diskarbitrationd, which starts
        // with WindowServer): Metal's discovery runs when the device appears, and a missing bundle there is final. After
        // 15 minutes publish anyway, so a broken mount still shows WindowServer's open in the log.
        const int path = metal == 1 ? bundleLookup("/Library/GPUBundles/NativeAdrenoMetal.bundle/Contents/Info.plist") : 0;
        if (metal == 1) ulog("UKE MTL: t=%us waiting, path GPUBundles %d UKEDRV %d\n", i * 30, path,
                             bundleLookup("/Volumes/UKEDRV"));
        if (metal == 1 && native_adreno_published_service() && (path == 0 || i >= 30)) {
            const IOReturn status = uke_gpu_publish_accelerator();
            if (status) ulog("UkeGpu: AGXAccelerator not published (%#x)\n", status);
            metal = status ? 3 : 2;
        }
        if (i == 1 || i == 14 || i == 20 || i == 31) dumpStorage(i * 30);
        if (metal >= 2) dumpMetal(metal);
        dumpRenderServer(i * 30);
        dumpPerf(i * 30);
        if (uke_cpufreq_apply() && (i < 8 || i % 4 == 0)) ulog("UKE CPU: t=%us %.159s\n", i * 30, uke_cpufreq_row);
        ulog("UKE IBP: t=%us ib ranges published %u bytes %u last error %#x | wc audits %u dirty %u distrusted %u | replay skipped %u | wc zero-fill bos %u pages %u\n",
             i * 30, native_uke_ib_publish[0], native_uke_ib_publish[1], native_uke_ib_publish[2], native_adreno_wc_audit[0],
             native_adreno_wc_audit[2], native_adreno_wc_distrusted, native_uke_replay_skipped,
             native_uke_wc_zero_fill[0], native_uke_wc_zero_fill[1]);
        {
            const volatile uint32_t *z = native_uke_ib_zero;
            ulog("UKE IBZ: t=%us ibs %u zero-before %u cached %u (kind %x pid %u iova %x) absent %u (kind %x pid %u iova %x bytes %u) unread %u\n",
                 i * 30, z[0], z[1], z[2], z[4], z[5], z[6], z[3], z[7], z[8], z[9], z[10], z[11]);
        }
        ulog("UKE GOP: t=%us gpu offers %llu done %llu failed %llu late %llu refused %llu waits %llu latecomplete %llu\n", i * 30,
             uke_gop_gpu_stats[0], uke_gop_gpu_stats[1], uke_gop_gpu_stats[2], uke_gop_gpu_stats[3], uke_gop_gpu_stats[4],
             uke_gop_gpu_stats[5], uke_gop_gpu_stats[6]);
        for (unsigned s = 0; s < 30; ++s) {IOSleep(1000); sampleTexts(i * 30 + s + 1); dumpStall(i * 30 + s + 1);}
    }
}
}

class UkeGpu : public IOService {
    OSDeclareDefaultStructors(UkeGpu)

public:
    bool start(IOService *provider) override {
        if (!IOService::start(provider)) return false;
        unsigned mode = 0;
        if (!PE_parse_boot_argn("sheng-gpu-power", &mode, sizeof mode) || !mode) {
            ulog("UkeGpu: sheng-gpu-power not set, GPU left off\n");
            return true;
        }
        ulog("UkeGpu: starting the Adreno 732 bring-up, sheng-gpu-power=%u\n", mode);
        thread_t thread = nullptr;
        if (kernel_thread_start(bringUp, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
        registerService();
        return true;
    }
};
OSDefineMetaClassAndStructors(UkeGpu, IOService)
