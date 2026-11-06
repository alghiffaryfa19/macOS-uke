// SPDX-License-Identifier: BSD-2-Clause-Patent
// Forced include (-include) for GpuPowerProbe.cpp and NativeAdrenoService.cpp when the firmware comes from the DRV
// window instead of the kernel (UKE_GPU_FIRMWARE=drv). The iPadOS sources name the firmware as weak const arrays
// (native_adreno_{firmware,sqe_firmware,zap}_image[] and their _size words) that firmware.S defined in the kernel.
// The prelinked kernel copies every byte of the bundle, and vendor_boot has no room for them, so each name becomes the
// object behind a pointer that UkeGpu.cpp sets once the DRV copy is read and its SHA-256 checked:
//   extern "C" const uint8_t native_adreno_firmware_image[] __attribute__((weak));
// declares   extern "C" const uint8_t (*uke_fw_gmu)[] __attribute__((weak));
// and every use (x[i], memcpy(.., x, ..), &x_size, x_size) reads through the pointer. The pointers are never null:
// until a load succeeds they point at an empty image of size 0, which the probe already refuses ("size < 4").
#pragma once
#include <stdint.h>

#define native_adreno_firmware_image (*uke_fw_gmu)
#define native_adreno_firmware_image_size (*uke_fw_gmu_size)
#define native_adreno_sqe_firmware_image (*uke_fw_sqe)
#define native_adreno_sqe_firmware_image_size (*uke_fw_sqe_size)
#define native_adreno_zap_image (*uke_fw_zap)
#define native_adreno_zap_image_size (*uke_fw_zap_size)
