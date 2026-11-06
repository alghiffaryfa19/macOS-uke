// SPDX-License-Identifier: BSD-2-Clause-Patent
// UkeGpu: macOS 26.5 entry for the uke Adreno 732 bring-up (base: our iPadOS 27 build). On iPadOS the LegacyDisplayProbe called
// sheng_gpu_power_probe() once the desktop was up; macOS has no such display probe, so this IOService (IOResources,
// like UkeTouchPointer) starts the same staged thread (GpuPowerProbe.cpp, boot-arg sheng-gpu-power=1..9). Every row
// the probe draws is also printed as "UKE GPU<n>: ..." through IOLog, so the result lands in macoslog.
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

extern "C" {
void sheng_gpu_power_probe(void);
extern volatile uint32_t native_gpu_power_stage;
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
int boot_log_export_put(uint32_t, uint32_t, uint32_t, const void *, uint32_t) { return -1; }
// Diagnostic only (which process still maps a BO page being freed); the macOS pmap has no such lookup.
unsigned int native_pmap_user_mappings(ppnum_t, void **, uint64_t *, char *) { return 0; }
void native_adreno_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp");
void native_adreno_accelerator_static_init(void) {}
IOService *native_adreno_accelerator_allocate(void) { return nullptr; }
unsigned native_adreno_accelerator_instance_size(void) { return 0; }
IOReturn native_gop_handoff_call(uint32_t, task_t, const void *, size_t, void *, size_t) { return kIOReturnUnsupported; }
IOMemoryDescriptor *native_gop_handoff_memory(void) { return nullptr; }
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
            IOLog("UkeGpu: DRV firmware %s fails its SHA-256\n", role);
            IOFreeAligned(buffer, rounded);
            return false;
        }
        gBytes[slot] = e.bytes;
        *image = reinterpret_cast<const uint8_t (*)[]>(buffer);
        *size = &gBytes[slot];
        IOLog("UkeGpu: DRV firmware %s %llu bytes ok\n", role, e.bytes);
        return true;
    }
    IOLog("UkeGpu: DRV firmware %s missing or unreadable\n", role);
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
    if (!ok) IOLog("UkeGpu: no DRV firmware table (status %#x)\n", status);
    ok = ok && loadOne(*table, "gmu", &uke_fw_gmu, &uke_fw_gmu_size, 0);
    ok = ok && loadOne(*table, "sqe", &uke_fw_sqe, &uke_fw_sqe_size, 1);
    ok = ok && loadOne(*table, "zap", &uke_fw_zap, &uke_fw_zap_size, 2);
    IOFreeAligned(table, 4096);
    return ok;
}
}
#endif

namespace {
void bringUp(void *, wait_result_t) {
#if UKE_GPU_FIRMWARE_DRV
    // Without firmware the probe still runs the power and clock stages and stops at the GMU cold start (mode 7).
    if (!loadFirmware()) IOLog("UkeGpu: continuing without firmware\n");
#endif
    sheng_gpu_power_probe();
    // Phase 2 (uke-gpu-metal=1): once the probe has published NativeAdrenoGpu (mode 9, after "C10ok"), make it visible
    // to Metal's discovery (UkeGpuAccelerator.cpp).
    unsigned metal = 0;
    PE_parse_boot_argn("uke-gpu-metal", &metal, sizeof metal);
    // Stage word every 30 s for ten minutes, so a hang leaves its step in the log.
    uint32_t last = ~0u;
    for (unsigned i = 0; i < 20; ++i) {
        if (native_gpu_power_stage != last) {
            last = native_gpu_power_stage;
            IOLog("UKE GPU: stage %#x\n", last);
        }
        if (metal == 1 && native_adreno_published_service()) {
            const IOReturn status = uke_gpu_publish_accelerator();
            if (status) IOLog("UkeGpu: AGXAccelerator not published (%#x)\n", status);
            metal = 2;
        }
        IOSleep(30000);
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
            IOLog("UkeGpu: sheng-gpu-power not set, GPU left off\n");
            return true;
        }
        IOLog("UkeGpu: starting the Adreno 732 bring-up, sheng-gpu-power=%u\n", mode);
        thread_t thread = nullptr;
        if (kernel_thread_start(bringUp, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
        registerService();
        return true;
    }
};
OSDefineMetaClassAndStructors(UkeGpu, IOService)
