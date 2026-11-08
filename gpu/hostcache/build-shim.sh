#!/bin/bash
# uke macOS: the host IOKit shim + NativeAdreno kernel half (UniXNU-a27 tests/hostsim, scripts/check-turnip-darwin-host.py recipe)
# with the tablet's device identity (GpuPowerProbe.cpp gDeviceInfo: chip 0x43030b00, GMEM 1.5 MiB), so the tablet's own Turnip
# and plugin compile on this Mac exactly what they compile on the tablet. Output: hostcache/out/libhostiokit.dylib.
set -euo pipefail
D=/Users/daniel/Desktop/Deliver/Domotica; R=$D/UniXNU-a27; W=$D/boot/macos-gpu/hostcache; O=$W/out
kit=$D/boot/macos-gpu/src/drivers/Adreno740; sim=$R/drivers/Adreno740/tests/hostsim; smmu=$D/boot/macos-gpu/src/kits/ArmSmmu4K
mkdir -p $O/host-include/sys $O/host-include/IOKit $O/host-include/kern $O/src
for n in IOKit/IOTypes.h IOKit/IOReturn.h IOKit/IOBufferMemoryDescriptor.h IOKit/IOLib.h kern/task.h; do echo '#include "XnuMemoryFixture.h"' > $O/host-include/$n; done
sed -e 's/const NativeAdrenoAbiDeviceInfo info={0x43050a01u,3u<<20,/const NativeAdrenoAbiDeviceInfo info={0x43030b00u,0x180000u,/' -e 's/0x1fffffffff000ull,0x20000000ull,0x60000000ull,/0x1fffffffff000ull,0x100000000ull,0x100000000ull,/' $sim/host_gpu_service.cpp > $O/src/host_gpu_service.cpp
grep -q '0x43030b00u,0x180000u' $O/src/host_gpu_service.cpp
inc=(-I $O/host-include -I $sim -I $sim/.. -I $kit -I $smmu -I $D/boot/macos-gpu/src/kits/AdrenoPM4)
objs=()
for s in ArmSmmu4KContext ArmSmmu4KProgram ArmSmmu4KLive ArmSmmu4KStreams ArmSmmu4KMmio; do
  xcrun clang -std=c11 -O1 -fPIC -c $smmu/$s.c -o $O/$s.o; objs+=($O/$s.o); done
xcrun clang -std=c11 -O1 -fPIC -c $D/boot/macos-gpu/src/kits/AdrenoPM4/AdrenoPM4.c -o $O/AdrenoPM4.o; objs+=($O/AdrenoPM4.o)
xcrun clang++ -std=c++23 -O1 -fPIC "${inc[@]}" -c $O/src/host_gpu_service.cpp -o $O/host_gpu_service.o; objs+=($O/host_gpu_service.o)
for n in NativeAdrenoMemory NativeAdrenoSmmuMemory NativeAdrenoVm NativeAdrenoRing NativeAdrenoSubmit NativeAdrenoUserClient NativeAdrenoTimer NativeAdrenoSmmuContext NativeAdrenoGpuContext NativeAdrenoPreempt NativeAdrenoProfile; do
  xcrun clang++ -std=c++23 -O1 -fPIC "${inc[@]}" -c $kit/$n.cpp -o $O/$n.o; objs+=($O/$n.o); done
xcrun clang++ -std=c++17 -O1 -fPIC -c $O/src/host_stubs.cpp -o $O/host_stubs.o; objs+=($O/host_stubs.o)
xcrun clang++ -std=c++17 -O1 -fPIC -c $sim/host_iokit_shim.cpp -o $O/host_iokit_shim.o; objs+=($O/host_iokit_shim.o)
xcrun clang++ -dynamiclib -o $O/libhostiokit.dylib "${objs[@]}" -framework IOKit -framework CoreFoundation
xcrun clang -std=c11 -O1 -I /opt/homebrew/include $sim/vulkan_smoke.c -o $O/vulkan_smoke
echo SHIM-OK
