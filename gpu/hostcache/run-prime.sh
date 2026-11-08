#!/bin/bash
# Prime every metallib of the mounted 25F71 system volume into hostcache/cache-25f71 with the tablet's plugin and Turnip. A run that dies
# (a function that crashes the translator) is started again: PrimeShaders.h moves the function it was compiling to prime-bad and resumes.
# usage: run-prime.sh <system volume mount> [list] [cache dir]
set -uo pipefail
D=${UKE_ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}; W=$D/boot/macos-gpu/hostcache; B=$D/boot/out/drv-stage/NativeAdrenoMetal.bundle/Contents/MacOS
M=${1:?mount}; L=${2:-$W/out/metallibs.txt}; C=${3:-$W/cache-25f71}
[ -f "$L" ] || find "$M/System/Library" -name '*.metallib' -type f | sort > "$L"
for attempt in $(seq 1 40); do
  env -i HOME=$HOME PATH=/usr/bin:/bin DYLD_INSERT_LIBRARIES=$W/out/libhostiokit.dylib $W/out/prime_host $B/NativeAdrenoMetal $B/libvulkan_freedreno.dylib $C "$L" >> ${PRIME_LOG:-$W/out/prime.log} 2>&1
  rc=$?
  echo "attempt $attempt exit $rc $(date +%H:%M:%S)" >> ${PRIME_LOG:-$W/out/prime.log}
  [ $rc = 0 ] && break
done
tail -1 $C/prime.log
