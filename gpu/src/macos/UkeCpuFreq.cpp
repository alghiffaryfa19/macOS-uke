// SPDX-License-Identifier: BSD-2-Clause-Patent
// uke macOS: CPU clusters at their highest non-turbo level (`sheng-cpufreq=<n>`), the EPSS control the iPadOS kernel used
// (UniXNU-uke-gpu-seed package/CpuFreqProbe.cpp, kernel 21, 2026-10-28: domain 0 went from 1593 to 1900 MHz and held; the two big
// clusters already sat at 2611 / 2803 MHz). Nothing on macOS asks the EPSS block for a level (no cpufreq driver, no CLPC for this
// SoC), so the little cluster stays at the bootloader's level, and every CPU-side part of rendering (WindowServer, the Metal
// translator, Turnip's command recording) runs slower than it could.
//   1  read-only: report each domain's enable bit, current/requested level and its highest non-turbo LUT entry
//   2  additionally request that level in domain 0 (the boot CPU's cluster)
//   3  the same in every enabled domain (including clusters whose cores are not yet started)
// Register layout: qcom-cpufreq-hw "epss" (reg_enable 0x0, reg_domain_state 0x20, reg_dcvs_ctrl 0xb0, reg_freq_lut 0x100 with 4-byte
// rows, reg_perf_state 0x320); the three domain windows as on the iPadOS kernel. LUT row: bits 31:30 source, 18:16 core count
// (1 = turbo), 7:0 L value (multiple of the 19.2 MHz bi_tcxo_div2 clock; source 0 is GPLL0/2).
// Snapintosh (refs/Snapintosh compat/cpu-dvfs.h) runs a full governor, but its X2 Elite has no cpufreq-hw block (SCMI levels through
// the PDP firmware); on uke the EPSS registers take the level directly, as the iPadOS boots showed. uke_cpufreq_apply() is called
// again every 30 s from UkeGpu's log loop: the request is re-asserted (cheap, idempotent) and the row shows what the hardware reports.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <pexpert/pexpert.h>

extern "C" {
char uke_cpufreq_row[160];
volatile uint32_t uke_cpufreq_stage;
}

namespace {
constexpr uint64_t kDomainBase[3] = {0x17d91000, 0x17d92000, 0x17d93000};
constexpr uint64_t kDomainSize = 0x1000;
constexpr uint32_t kRegEnable = 0x0, kRegDomainState = 0x20, kRegDcvsCtrl = 0xb0, kRegFreqLut = 0x100, kRegPerfState = 0x320;
constexpr unsigned kLutEntries = 40;
constexpr uint32_t kXoKHz = 19200;        // bi_tcxo_div2
constexpr uint32_t kGpllHalfKHz = 300000; // GPLL0 600 MHz / CLK_HW_DIV 2

uint32_t rd32(uintptr_t base, uint32_t reg) {return *reinterpret_cast<volatile uint32_t *>(base + reg);}
void wr32(uintptr_t base, uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
uint32_t lutKHz(uint32_t row) {return (row >> 30) ? kXoKHz * (row & 0xff) : kGpllHalfKHz;}
IOMemoryMap *gMaps[3];
}

// Returns the mode (0: off, nothing touched). The maps are made once and kept.
extern "C" unsigned uke_cpufreq_apply(void) {
    unsigned mode = 0;
    PE_parse_boot_argn("sheng-cpufreq", &mode, sizeof(mode));
    if (!mode) return 0;
    uke_cpufreq_stage = 1;
    char *out = uke_cpufreq_row;
    unsigned n = (unsigned)snprintf(out, sizeof(uke_cpufreq_row), "CPUFREQ m%u", mode);
    for (unsigned d = 0; d < 3; ++d) {
        if (!gMaps[d]) {
            IODeviceMemory *memory = IODeviceMemory::withRange(kDomainBase[d], kDomainSize);
            gMaps[d] = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
            if (memory) memory->release();
        }
        if (!gMaps[d]) {n += (unsigned)snprintf(out + n, sizeof(uke_cpufreq_row) - n, " d%u:nomap", d); continue;}
        uintptr_t va = static_cast<uintptr_t>(gMaps[d]->getVirtualAddress());
        uint32_t enable = rd32(va, kRegEnable), state = rd32(va, kRegDomainState) & 0xff, perf = rd32(va, kRegPerfState);
        uint32_t dcvs = rd32(va, kRegDcvsCtrl);
        // Last distinct, non-turbo row before the table ends (two equal frequencies end it).
        int best = -1; uint32_t bestKHz = 0, prev = 0;
        for (unsigned i = 0; i < kLutEntries; ++i) {
            uint32_t row = rd32(va, kRegFreqLut + i * 4);
            uint32_t khz = lutKHz(row);
            bool turbo = ((row >> 16) & 7) == 1;
            if (i > 0 && khz == prev) break;
            if (!turbo && khz >= bestKHz) {best = (int)i; bestKHz = khz;}
            prev = khz;
        }
        int rc = 0;
        if (mode >= 2 && (d == 0 || mode >= 3) && (enable & 1) && best >= 0) {
            if (perf != (uint32_t)best) wr32(va, kRegPerfState, (uint32_t)best);
            if (dcvs & 1) for (unsigned c = 1; c < 4; ++c)
                if (rd32(va, kRegPerfState + c * 4) != (uint32_t)best) wr32(va, kRegPerfState + c * 4, (uint32_t)best);
            rc = 1;
        }
        n += (unsigned)snprintf(out + n, sizeof(uke_cpufreq_row) - n, " d%u:e%u c%u p%u m%d@%u%s",
                                d, enable & 1, state * kXoKHz / 1000, perf, best, bestKHz / 1000, rc ? " SET" : "");
    }
    uke_cpufreq_stage = 2;
    return mode;
}
