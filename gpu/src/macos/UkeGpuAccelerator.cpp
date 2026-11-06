// SPDX-License-Identifier: BSD-2-Clause-Patent
// Phase 2 (boot-arg uke-gpu-metal=1): the registry entry macOS Metal discovers. MTLRegisterDevices ->
// -[MTLIOAccelServiceGlobalContext init] matches IOServiceMatching("IOAccelerator") (traced on a Mac), reads MetalPluginName
// and MetalPluginClassName, loads /System/Library/Extensions/<MetalPluginName>.bundle and calls
// -[<MetalPluginClassName> initWithAcceleratorPort:]. On iPadOS the same role was the AGXAccelerator subclass of the original
// IOAcceleratorES (NativeAdrenoAccelerator.cpp, bound to iOS 17 kernelcache addresses); this one replaces it.
//
// IOAccelerator itself lives in IOGraphicsFamily, which ForSheng's boot collection does not carry (about 350 KiB that
// vendor_boot cannot hold). With UKE_GPU_OWN_IOACCELERATOR the bundle registers its own empty IOAccelerator class so the
// matching finds AGXAccelerator; that only holds while IOGraphicsFamily never loads (it would refuse the duplicate class).
// Without it the class comes from IOGraphicsFamily, which must then be added to the prelinked collection.
// The service class keeps the AGX name for the same reason as on iPadOS: sandbox profiles name AGXAccelerator and
// AGXDeviceUserClient, and native_adreno_new_user_client hands out the latter.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <pexpert/pexpert.h>

extern "C" IOReturn native_adreno_new_user_client(IOService *provider, task_t owningTask, void *securityID, UInt32 type,
                                                  OSDictionary *properties, IOUserClient **handler, unsigned limit);
extern "C" void native_adreno_note_property_read(const char *key);

#if UKE_GPU_OWN_IOACCELERATOR
class IOAccelerator : public IOService {
    OSDeclareDefaultStructors(IOAccelerator)
};
OSDefineMetaClassAndStructors(IOAccelerator, IOService)
#else
#include <IOKit/graphics/IOAccelerator.h>
#endif

class AGXAccelerator : public IOAccelerator {
    OSDeclareDefaultStructors(AGXAccelerator)

public:
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
                           IOUserClient **handler) override {
        unsigned limit = 40;   // uke-gpu-metal-opens: connections ever granted to the plugin (0 = unlimited)
        PE_parse_boot_argn("uke-gpu-metal-opens", &limit, sizeof limit);
        return native_adreno_new_user_client(this, owningTask, securityID, type, properties, handler, limit);
    }
    // Which keys the discovery reads (the published service's diagnostic counters), as on iPadOS.
    OSObject *copyProperty(const char *key) const override {
        native_adreno_note_property_read(key);
        return IORegistryEntry::copyProperty(key);
    }
};
OSDefineMetaClassAndStructors(AGXAccelerator, IOAccelerator)

// After the probe published NativeAdrenoGpu (GpuPowerProbe's "P" row): one AGXAccelerator under the platform expert.
extern "C" IOReturn uke_gpu_publish_accelerator(void) {
    static bool published;
    if (published) return kIOReturnSuccess;
    AGXAccelerator *accelerator = OSTypeAlloc(AGXAccelerator);
    if (!accelerator || !accelerator->init()) {
        OSSafeReleaseNULL(accelerator);
        return kIOReturnNoMemory;
    }
    accelerator->setProperty("MetalPluginName", "NativeAdrenoMetal");
    accelerator->setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice");
    accelerator->setProperty("model", "Adreno 732");
    // The processes whose open the iPadOS kernel granted in place of the sandbox verdict; kept for the same grant patch.
    accelerator->setProperty("NativeSandboxGrantProcess", "WindowServer");
    if (!accelerator->attach(IOService::getPlatform())) {
        accelerator->release();
        return kIOReturnNotAttached;
    }
    accelerator->registerService();
    accelerator->release();
    published = true;
    IOLog("UkeGpu: AGXAccelerator published (MetalPluginName NativeAdrenoMetal)\n");
    return kIOReturnSuccess;
}
