#!/bin/zsh
# Build the macOS 26.5 (25F71) arm64e XNU the way macOS-ForSheng's build_macos26_xnu.py does,
# without running its scripts: stage the kernel SDK, install headers, archive, then link.
# Usage: macos-kbuild.sh [stage|headers|archive|all]   (default all)
set -e
D=${UKE_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
FS=$D/refs/macOS-ForSheng
KDK=${KDK:-$D/ipsw/macos26/kdk/expanded}
OUT=$FS/out/macos26-xnu-arm64e
SRC=$OUT/source
SDK=$OUT/kernel-sdk
STEP=${1:-all}
mkdir -p $OUT/logs $SDK
KDKROOT=$KDK/KDK.pkg/Payload
export XNU_AVAILABILITY_PL=$SDK/usr/local/libexec/availability.pl

if [[ $STEP == stage || $STEP == all ]]; then
  rsync -a $KDK/KDK_SDK.pkg/Payload/ $SDK/
  make -C $FS/sources/macos26/AvailabilityVersions install DSTROOT=$SDK OBJROOT=$OUT/availability \
    SYMROOT=$OUT/availability CMAKE=$(which cmake) NINJA=$(which ninja) > $OUT/logs/availability.log 2>&1
  rsync -a $FS/sources/macos26/libplatform/include/ $FS/sources/macos26/libplatform/private/ $SDK/usr/local/include/
  PH=$SDK/System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/platform
  mkdir -p $PH && rsync -a $FS/sources/macos26/libplatform/private/platform/ $PH/
  mkdir -p $SDK/usr/local/include/os
  for h in $FS/sources/macos26/libdispatch/os/*.h; do
    unifdef -DKERNEL=1 -DOS_FIREHOSE_SPI=1 -DOS_VOUCHER_ACTIVITY_SPI_TYPES=1 -UOS_VOUCHER_ACTIVITY_SPI \
      $h > $SDK/usr/local/include/os/${h:t} || [[ $? == 1 ]]
  done
  echo "stage ok"
fi

W="-Wno-error=unused-but-set-variable -Wno-error=gnu-folding-constant -Wno-error=implicit-int-float-conversion -Wno-error=pointer-to-int-cast -Wno-error=void-pointer-to-int-cast -Wno-error=void-pointer-to-enum-cast"
COMMON=(make -C $SRC SDKROOT=macosx KDKROOT=$KDKROOT "TARGET_CONFIGS=DEVELOPMENT ARM64 SHENG"
  ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64e ARM64_LARGE_MEMORY=1 ARM64_LINK_ADDR=0xfffffe0007004000
  RC_DARWIN_KERNEL_VERSION=25.5.0 RC_ProjectSourceVersion=12377.121.6
  "DEPLOYMENT_TARGET_FLAGS=-mmacosx-version-min=26.5 -DXNU_TARGET_OS_OSX"
  BUILD_LTO=0 BOUND_CHECKS=0 BUILD_WERROR=1 MAKEJOBS=-j$(sysctl -n hw.ncpu)
  KERNEL_SDKROOT=$SDK OBJROOT=$OUT/obj SYMROOT=$OUT/sym DSTROOT=$SDK)

if [[ $STEP == headers || $STEP == all ]]; then
  ${COMMON[@]} installhdrs > $OUT/logs/install-headers.log 2>&1 && echo "headers ok"
fi
if [[ $STEP == archive || $STEP == all ]]; then
  ${COMMON[@]} -k build BUILD_STATIC_LINK=1 BUILD_XNU_LIBRARY=1 CFLAGS_KERNEL=-mkernel \
    CXXFLAGS_KERNEL=-fapple-kext "CFLAGS_EXTRA=-DXNU_NATIVE_ARCH_TIMER=1 -DXNU_DT_PANIC_REGION=1 $W" \
    > $OUT/logs/source-archive.log 2>&1 && echo "archive ok" || { grep -m5 -B2 -A8 "error:" $OUT/logs/source-archive.log; exit 1; }
  ls -la $OUT/obj/DEVELOPMENT_ARM64_SHENG/libkernel.development.sheng.a
fi
if [[ $STEP == link ]]; then
  RT=$(xcrun clang -print-resource-dir)/lib/darwin
  RTLIB=$RT/libclang_rt.cc_kext_osx.a; [[ -f $RTLIB ]] || RTLIB=$RT/libclang_rt.cc_kext.a
  ARCHIVES="-Wl,-force_load,$OUT/corecrypto/libcorecrypto-native.a $OUT/ArmDTPlatform.o $OUT/ArmCpu.o $SDK/usr/local/lib/kernel/libfirehose_kernel.a"
  ${COMMON[@]} build BUILD_STATIC_LINK=1 BUILD_XNU_LIBRARY=0 CFLAGS_KERNEL=-mkernel CXXFLAGS_KERNEL=-fapple-kext \
    "CFLAGS_EXTRA=-DXNU_NATIVE_ARCH_TIMER=1 -DXNU_DT_PANIC_REGION=1 $W" LD_KERNEL_LIBS=$RTLIB \
    "LD_KERNEL_ARCHIVES=$ARCHIVES" "NATIVE_KERNEL_LINK_FLAGS=-Wl,-headerpad,0x2000 -Wl,-map,$OUT/kernel.development.sheng.map" \
    DO_CTFMERGE=0 > $OUT/logs/kernel-link.log 2>&1 && echo "link ok" || { grep -m10 -B2 -A12 "error\|Undefined" $OUT/logs/kernel-link.log | tail -40; exit 1; }
  ls -la $OUT/obj/DEVELOPMENT_ARM64_SHENG/kernel.development.sheng.unstripped.noctf
fi
