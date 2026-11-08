// uke macOS: shader priming on the Mac, as Snapintosh precompiles its WindowServer shaders on the host. The tablet's own plugin binary
// (NativeAdrenoMetal, which carries the frontend and the metal2vulkan runtime) and its own Turnip run here over the host copy of the
// NativeAdreno kernel stack (libhostiokit.dylib, built by build-shim.sh with the tablet's device identity: chip 0x43030b00, GMEM 1.5 MiB),
// with the environment WindowServer has on the tablet (TU_DEBUG=nocb for the binning image). The plugin's own priming code (PrimeShaders.h,
// compiled in here unchanged) then translates and compiles every Metal library of the macOS 25F71 system volume into a cache directory,
// which make-pack.py turns into NativeAdrenoMetal.m2vpack for the bundle: the tablet imports it at device open and starts with cache hits.
// The software CP executes nothing; only compilation happens here.
// usage: prime_host <plugin binary> <libvulkan_freedreno.dylib> <cache dir> <library list>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

static void *gPlugin;
template <typename F> static F pluginSymbol(const char *name) {
    void *p = gPlugin ? dlsym(gPlugin, name) : nullptr;
    if (!p) {fprintf(stderr, "prime_host: plugin has no %s\n", name); exit(3);}
    return reinterpret_cast<F>(p);
}
// PrimeShaders.h calls these three runtime functions; here they reach the plugin's own copies.
extern "C" unsigned native_bridge_metal_recent_commits(uint64_t windowUs) {
    return pluginSymbol<unsigned (*)(uint64_t)>("native_bridge_metal_recent_commits")(windowUs);
}
extern "C" void native_bridge_metal_quiet_problems(bool quiet) {
    pluginSymbol<void (*)(bool)>("native_bridge_metal_quiet_problems")(quiet);
}
extern "C" const char *nmb_runtime_build(void) {return pluginSymbol<const char *(*)(void)>("nmb_runtime_build")();}

#include "PrimeShaders.h"

int main(int argc, char **argv) {
    if (argc != 5) {fprintf(stderr, "usage: prime_host <plugin> <turnip> <cache dir> <library list>\n"); return 2;}
    @autoreleasepool {
        // What NativeAdrenoMetalPlugin.mm sets in WindowServer before the device opens (binning image: no sheng-fb-bgra).
        setenv("TU_DEBUG", "nocb", 1);
        // The host shim answers the iPadOS class; the class name only picks the service, it keys nothing.
        setenv("TU_DARWIN_SERVICE", "IOAcceleratorES", 1);
        unsetenv("SHENG_DIAG"); unsetenv("SHENG_GPU_TRACE"); unsetenv("SHENG_LOG_GPU"); unsetenv("METAL2VULKAN_LOG_PIPELINES");
        gPlugin = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
        if (!gPlugin) {fprintf(stderr, "prime_host: %s\n", dlerror()); return 3;}
        auto open = pluginSymbol<id (*)(const char *, const char *, uint64_t, void (*)(const char *, const char *))>("native_bridge_metal_open");
        auto lastError = pluginSymbol<const char *(*)(void)>("nmb_last_error");
        mkdir(argv[3], 0700);
        id<MTLDevice> device = open(argv[2], argv[3], 0, nullptr);
        if (!device) {fprintf(stderr, "prime_host: no device: %s\n", lastError()); return 4;}
        printf("device %s, runtime %s\n", [[device name] UTF8String], nmb_runtime_build());
        fflush(stdout);
        prime::run(device, argv[3], 0, argv[4]);
        printf("done: libraries %u/%u, pipelines %u\n", native_prime_libraries_done, native_prime_libraries_total, native_prime_made);
        if (getenv("PRIME_HOST_TABLE")) {   // the frontend's telemetry table (warnings, formats, refusals, pipeline counts)
            static char table[16384];
            const size_t n = pluginSymbol<size_t (*)(char *, size_t)>("native_bridge_metal_telemetry")(table, sizeof table);
            fwrite(table, 1, n, stdout);
        }
    }
    return 0;
}
