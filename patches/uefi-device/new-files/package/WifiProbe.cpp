// SPDX-License-Identifier: BSD-2-Clause-Patent
// WiFi bring-up on the uke board (SM7675 / Snapdragon 7+ Gen 3).
//
// The WiFi chip is expected to be WCN7850 on PCIe (FastConnect 7800),
// the same as sheng (SM8550). The board differences are:
//   - PCIe controller base addresses (SM7675 pcie@1c00000, verify on device)
//   - GCC clock register offsets (gcc-cliffs.c vs gcc-sm8550.c)
//   - GPIO numbers for WLAN_EN, PERST#, BT_EN, SW_CTRL
//   - PMIC rail names and peripheral addresses
//   - SMMU stream ID assignment
//
// Stage 1 (uke-wifi=1): read-only probe of the PCIe0 path left by the
//   boot loaders. Maps GCC, TLMM and the PCIe controller and reports
//   the clock, GPIO and LTSSM state without writing anything.
// Stage 2 (uke-wifi=2): powers WLAN_EN, trains the PCIe0 link and reads
//   the endpoint's vendor/device ID.
// Stage 3 (uke-wifi=3): sizes and assigns BAR0, reads MHI registers.
// Stage 4 (uke-wifi=4): SMMU stream mapping, firmware download via MHI.
// Stage 5 (uke-wifi=5): starts the Ath12kWifi module (nw_wifi_start).
//
// HARDWARE VERIFICATION REQUIRED: the addresses below are preliminary,
// derived from the SM8550 port and gcc-cliffs.c (SM7675's GCC). They
// must be confirmed against the live device tree or register dumps from
// the rooted tablet once hardware arrives.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include "nw_abi.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <stdint.h>

// ---- SM7675 (cliffs) PCIe0 addresses ----
// Sources: gcc-cliffs.c (MCC45TR/senemos-uke-kernel-mainline 0001-cliffs-clock-handoff.patch),
// SM7675 DTS (0007-uke-independent-dts.patch), uke-wlan-supplies-and-dma.patch.
// Values marked (verify) need confirmation from DT dump on live device.
namespace {
constexpr uint64_t kGcc   = 0x100000;       // clock-controller@100000 (confirmed)
constexpr uint64_t kTlmm  = 0xf000000;      // pinctrl@f000000 (confirmed from DTS; SM8550 was 0xf100000)
constexpr uint64_t kParf  = 0x1c00000;      // pcie@1c00000 (confirmed: reg[0] in DTS)
constexpr uint64_t kDbi   = 0x60000000;     // dm_core (confirmed: reg[2] in DTS)
constexpr uint64_t kPhy   = 0x1c06000;      // QMP PHY (confirmed: reg[1] "phy" in DTS, size 0x2000)

// GCC PCIe0 GDSC and reset (offsets relative to kGcc).
// The PCIe0 clock block confirmed at 0x6b000 range from gcc-cliffs.c.
constexpr uint32_t kGdsc        = 0x6b004;  // PCIe0 GDSC (verify — consistent with clock block)
constexpr uint32_t kPhyGdsc     = 0x6c000;  // PCIe PHY GDSC (verify)
constexpr uint32_t kPcieBcr     = 0x6b000;  // block reset (verify)
constexpr uint32_t kPhyBcr      = 0x6c01c;  // PHY block reset (verify)

// Clock branch halt registers — confirmed from gcc-cliffs.c.
// NOTE: cnoc_sf, aggre_noc, ddrss_sf offsets corrected vs SM8550 pattern.
constexpr uint32_t kBranches[] = {
    0x6b020,  // gcc_pcie_0_slv_axi_clk (confirmed)
    0x6b02c,  // gcc_pcie_0_mstr_axi_clk (confirmed)
    0x6b038,  // gcc_pcie_0_cfg_ahb_clk (confirmed)
    0x6b03c,  // gcc_pcie_0_aux_clk (confirmed)
    0x6b048,  // gcc_pcie_0_pipe_clk (confirmed)
    0x6b01c,  // gcc_pcie_0_slv_q2a_axi_clk (confirmed)
    0x10058,  // gcc_cnoc_pcie_sf_axi_clk (confirmed; was 0x10030)
    0x10064,  // gcc_aggre_noc_pcie_axi_clk (confirmed; was 0x1003c)
    0x10074,  // gcc_ddrss_pcie_sf_qtb_clk (confirmed; was 0x1004c)
    0x6b054,  // gcc_pcie_0_phy_rchng_clk (confirmed)
};
// Additional clocks found in gcc-cliffs.c (not in kBranches[] probe yet):
//   0x6b098 = gcc_pcie_0_pipe_div2_clk
//   0x6b018 = gcc_qmip_pcie_ahb_clk
//   0x10050 = gcc_cfg_noc_pcie_anoc_ahb_clk

// Vote register for PCIe0 clocks (gcc-cliffs.c enable_reg / enable_mask):
//   0x52008: BIT(0) slv_axi, BIT(1) mstr_axi, BIT(2) cfg_ahb, BIT(3) aux,
//            BIT(4) pipe, BIT(5) slv_q2a, BIT(6) cnoc_sf
//   0x52000: BIT(12) aggre_noc, BIT(19) ddrss_sf, BIT(20) cfg_noc_ahb, BIT(22) phy_rchng
constexpr uint32_t kVoteEnable  = 0x52008;
constexpr uint32_t kVoteEnable2 = 0x52000;
constexpr uint32_t kVoteMask    = (1u<<0)|(1u<<1)|(1u<<2)|(1u<<3)|(1u<<4)|(1u<<5)|(1u<<6);
constexpr uint32_t kVoteMask2   = (1u<<12)|(1u<<19)|(1u<<20)|(1u<<22);

// GPIO assignments (TODO(hardware): read from DT dump or downstream pinctrl-cliffs.c)
// SM8550 uses: WLAN_EN=80, BT_EN=81, SW_CTRL=82, PERST#=94, WAKE#=96.
// SM7675 DTS reserves GPIOs 8-11 and 56-60; WLAN pins are outside those ranges.
constexpr unsigned kWlanEn = 80;   // TODO(hardware): verify
constexpr unsigned kBtEn   = 81;   // TODO(hardware): verify
constexpr unsigned kPerst  = 94;   // TODO(hardware): verify
constexpr unsigned kClkReq = 95;   // TODO(hardware): verify

// PCIe memory window (verify against SM7675 DTS ranges property)
constexpr uint64_t kMemBase = 0x60300000;
constexpr uint64_t kMemSize = 0x3d00000;

// Diagnostics exported to the boot log reader
extern "C" {
volatile uint32_t native_wifi_stats[29];
volatile uint32_t native_wifi_dump[6 * 18];
volatile uint32_t native_wifi_pmic[24 + 2 * 2 * 112];
volatile uint32_t native_wifi_ep[16];
volatile uint32_t native_wifi_pon[12];
volatile uint32_t native_wifi_cfg[32];
volatile uint32_t native_wifi_chip[16];
volatile uint32_t native_wifi_ev[48];
volatile uint32_t native_wifi_ldo[24];
volatile uint32_t native_wifi_mhi[16];
volatile uint32_t native_wifi_mhi_ms[8];
char native_wifi_text[512][112];
volatile uint32_t native_wifi_text_n;
char native_wifi_note[128][112];
volatile uint32_t native_wifi_note_n;
}

// Forward declarations for NativeUfs DMA window sharing
extern "C" volatile uint32_t native_ufs_extra_stream __attribute__((weak_import));
extern "C" uint8_t *native_ufs_extra_va __attribute__((weak_import));
extern "C" volatile uint32_t native_ufs_extra_uncached __attribute__((weak_import));
extern "C" int native_ufs_extra_fault(uint32_t *) __attribute__((weak_import));
extern "C" volatile uint32_t native_ufs_extra_atos[8] __attribute__((weak_import));
extern "C" volatile uint32_t native_ufs_extra_iova __attribute__((weak_import)), native_ufs_extra_bytes __attribute__((weak_import));
extern "C" int native_ufs_part_find(const char *, uint32_t *, uint64_t *, uint64_t *) __attribute__((weak_import));
extern "C" int native_ufs_part_read(uint32_t, uint64_t, uint32_t, void *) __attribute__((weak_import));

struct Window { IOMemoryMap *map; volatile uint32_t *va; };

bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    IODeviceMemory *mem = IODeviceMemory::withRange(base, size);
    w.map = mem ? mem->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (mem) mem->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}

void release(Window &w) { if (w.map) w.map->release(); w = {}; }

void pLog(int level, const char *line) {
    const uint32_t i = __atomic_fetch_add(const_cast<uint32_t *>(&native_wifi_text_n), 1u, __ATOMIC_RELAXED) % 512;
    snprintf(native_wifi_text[i], sizeof native_wifi_text[i], "%d %.100s", level, line);
    if (level <= 6) {
        const uint32_t j = __atomic_fetch_add(const_cast<uint32_t *>(&native_wifi_note_n), 1u, __ATOMIC_RELAXED) % 128;
        memcpy(native_wifi_note[j], native_wifi_text[i], sizeof native_wifi_note[j]);
    }
}

// ---- Stage 1: read-only probe ----
// Maps the GCC, TLMM and PCIe PARF to report the clock, GPIO and
// LTSSM state. Does not write any register.
void probeStage1() {
    Window gcc = {}, tlmm = {}, parf = {};
    if (!mapWindow(gcc, kGcc, 0x200000)) { native_wifi_stats[0] = 0xfffffffe; return; }
    if (!mapWindow(tlmm, kTlmm, 0x400000)) { release(gcc); native_wifi_stats[0] = 0xfffffffd; return; }
    if (!mapWindow(parf, kParf, 0x10000)) { release(tlmm); release(gcc); native_wifi_stats[0] = 0xfffffffc; return; }

    native_wifi_stats[0] = 1;  // stage 1 reached

    // GDSC power state
    native_wifi_stats[3] = gcc.va[kGdsc / 4];

    // Clock branch halt bits
    uint32_t halts = 0;
    for (unsigned i = 0; i < sizeof kBranches / sizeof kBranches[0]; ++i) {
        if (gcc.va[kBranches[i] / 4] & (1u << 31)) halts |= 1u << i;
    }
    native_wifi_stats[4] = halts;

    // GPIO state: WLAN_EN, PERST#
    auto gpioIo = [&](unsigned pin) -> uint32_t {
        return tlmm.va[(pin * 0x1000 + 4) / 4] & 3u;
    };
    native_wifi_stats[5] = gpioIo(kWlanEn) | gpioIo(kPerst) << 16;

    // PARF LTSSM state
    native_wifi_stats[6] = parf.va[0x1b0 / 4];

    release(parf);
    release(tlmm);
    release(gcc);
}

// ---- PMIC / SPMI ----
// SM7675 uses the same SPMI arbiter layout as SM8550 (v7).
// TODO(hardware): confirm kSpmiCore base from DT dump; SM7635 DTS uses 0xc400000.
constexpr uint64_t kSpmiCore = 0xc400000, kSpmiObs = 0xc440000;
constexpr uint64_t kSpmiChn = 0xc500000, kSpmiCnfg = 0xc42d000;
constexpr uint32_t kSleepClkPpid = 0x0ba; // PMK8550 gpio3 (sleep clock for WCN7850)
constexpr uint32_t kMaxRegs = 112;

// TODO(hardware): identify the correct PMIC regulator peripherals for SM7675.
// These are placeholder values from SM8550; the actual SIDs and page addresses
// depend on which PMICs are on the Xiaomi Pad 7 board.
constexpr uint8_t kRegPages[] = {0x9b, 0x9e, 0xa1, 0xa4, 0xa7, 0xaa, 0x4e};

struct Pmic { Window core, cnfg, obs, chn; uint32_t apids; };
Pmic gPmic;

bool pmicOpen(Pmic &p) {
    if (!mapWindow(p.core, kSpmiCore, 0x3000) ||
        !mapWindow(p.cnfg, kSpmiCnfg, 0x4000)) return false;
    p.apids = (p.core.va[0x0c / 4]) & 0x3ff; // TODO(hardware): verify APID count field
    if (!p.apids || p.apids > 1024) return false;
    return mapWindow(p.obs, kSpmiObs,
                     ((uint64_t)0x80 * p.apids + 0xfff) & ~0xfffull) &&
           mapWindow(p.chn, kSpmiChn, (uint64_t)0x200 * p.apids);
}

void pmicClose(Pmic &p) {
    release(p.chn); release(p.obs); release(p.cnfg); release(p.core);
}

int pmicApid(Pmic &p, uint32_t ppid) {
    for (uint32_t i = 0; i < p.apids; ++i) {
        const uint32_t map = p.core.va[(0x900 + 4 * i) / 4]; // TODO(hardware): verify APID_MAP offset
        if (map && ((map >> 8) & 0xfff) == ppid) return (int)i;
    }
    return -1;
}

void pmicPrepare() {
    Pmic &p = gPmic;
    if (!pmicOpen(p)) { native_wifi_pmic[10] = 1; return; }
    // TODO(hardware): read sleep clock pin state, configure if not already an output
    // The pattern is: find the APID for kSleepClkPpid, read its mode/function/enable,
    // and if not already output+func2+enabled, write the sequence:
    //   pull disabled (0x42=5), power source (0x41=0), function (0x44=2),
    //   mode output (0x40=1), master enable (0x46=0x80)
    int apid = pmicApid(p, kSleepClkPpid);
    native_wifi_pmic[3] = apid < 0 ? 0xffff : (uint32_t)apid;
    if (apid < 0) { native_wifi_pmic[10] = 2; return; }
    native_wifi_pmic[10] = 0xff; // TODO(hardware): implement actual write sequence
    pLog(6, "uke-wifi: PMIC sleep clock — TODO(hardware): verify and configure");
}

// ---- RPMh power rails ----
// The WCN7850 needs 6 supplies via RPMh. The rail names and voltages depend on
// the PMIC topology on the Xiaomi Pad 7.
// TODO(hardware): confirm rail names from the downstream DTS regulator bindings.
// These are sheng's values as placeholders — SM7675 may use different PMICs.
struct Rail { const char *name; uint32_t millivolts; };
constexpr Rail kRails[] = {
    {"smpg5", 850},   // vdd       — TODO(hardware): verify PMIC and rail
    {"ldob15", 1800},  // vddio     — TODO(hardware): verify
    {"smpg2", 850},   // vddaon    — TODO(hardware): verify
    {"smpe4", 950},   // vdddig    — TODO(hardware): verify
    {"smpg4", 1300},  // vddrfa1p2 — TODO(hardware): verify
    {"smpg6", 1860},  // vddrfa1p8 — TODO(hardware): verify
};
// TODO(hardware): confirm RSC DRV base and cmd-db base from DT dump.
constexpr uint64_t kRscDrv = 0x17a20000, kCmdDb = 0x81c60000;
constexpr uint64_t kCmdDbSize = 0x20000;

int powerRails() {
    // TODO(hardware): full implementation once rail names are confirmed.
    // Pattern: map the command database, look up each rail name, then send
    // RPMh active requests (mode HPM=7, voltage, enable) through the RSC's
    // TCS registers. Also vote the PCIe NoC BCM (SN7 or equivalent).
    pLog(6, "uke-wifi: power rails — TODO(hardware): RPMh votes");
    native_wifi_stats[20] = 0; // no rails voted yet
    return 0; // pretend success so the rest of the skeleton runs
}

// ---- GPIO helpers ----
void setPin(volatile uint32_t *tlmm, unsigned pin, uint32_t cfg, int out) {
    tlmm[pin * 0x1000 / 4] = cfg;
    if (out >= 0) {
        uint32_t io = tlmm[(pin * 0x1000 + 4) / 4];
        io = out ? (io | 2u) : (io & ~2u);
        tlmm[(pin * 0x1000 + 4) / 4] = io;
    }
}

uint32_t pinWord(volatile uint32_t *tlmm) {
    auto io = [&](unsigned pin) -> uint32_t { return tlmm[(pin * 0x1000 + 4) / 4] & 3u; };
    return io(kWlanEn) | io(kPerst) << 2 | io(kBtEn) << 4 | io(kClkReq) << 6;
}

bool waitClear(volatile uint32_t *reg, uint32_t mask, unsigned tries, unsigned *count = nullptr) {
    for (unsigned i = 0; i < tries; ++i) {
        if (!(*reg & mask)) { if (count) *count = i; return true; }
        IODelay(10);
    }
    if (count) *count = tries;
    return false;
}

void pulseReset(volatile uint32_t *gcc, uint32_t bcr) {
    gcc[bcr / 4] |= 1u;
    IODelay(1000);
    gcc[bcr / 4] &= ~1u;
    IODelay(1000);
}

// ---- GCC clock/GDSC constants ----
// GDSC collapse register: bit 0 = PCIe0, bit 3 = PCIe PHY
// TODO(hardware): verify collapse register offset from gcc-cliffs.c
constexpr uint32_t kCollapse = 0x62200; // TODO(hardware): SM7675 collapse register
// Clock source RCGs
constexpr uint32_t kAuxRcg   = 0x6b044; // TODO(hardware): verify from gcc-cliffs.c
constexpr uint32_t kRchngRcg = 0x6b050; // TODO(hardware): verify
constexpr uint32_t kPipeMux  = 0x6b04c; // TODO(hardware): verify

// PARF register offsets (from pcie-qcom.c / SM7675 DTS)
constexpr uint32_t kParfDeviceType   = 0x1000;
constexpr uint32_t kParfPhyCtrl      = 0x010;
constexpr uint32_t kParfDbiBase      = 0x168;
constexpr uint32_t kParfDbiBaseHi    = 0x16c;
constexpr uint32_t kParfAtuBase      = 0x634;
constexpr uint32_t kParfAtuBaseHi    = 0x638;
constexpr uint32_t kParfSlvSpace     = 0x16c; // TODO(hardware): verify
constexpr uint32_t kParfSlvSpaceHi   = 0x170; // TODO(hardware): verify
constexpr uint32_t kParfSysCtrl      = 0x00;
constexpr uint32_t kParfMhiClockReset = 0x174;
constexpr uint32_t kParfPmCtrl       = 0x020;
constexpr uint32_t kParfAxiHalt      = 0x178;
constexpr uint32_t kParfLtssm        = 0x1b0;
constexpr uint64_t kAtu              = 0x60001000; // TODO(hardware): verify
constexpr uint64_t kConfig           = 0x60100000; // TODO(hardware): verify
constexpr uint64_t kSlvSpaceBytes    = 0x80000000; // 2 GiB
constexpr uint64_t kTcsrClkref       = 0x1e40000;  // TODO(hardware): verify

// DBI registers
constexpr uint32_t kBusNumbers  = 0x18;
constexpr uint32_t kCommand     = 0x04;
constexpr uint32_t kLinkControl = 0x80;
constexpr uint32_t kPortDebug0  = 0x728;
constexpr uint32_t kPortDebug1  = 0x72c;

// Needed clock branches bitmask (the pipe branch runs only after PHY is up)
constexpr uint32_t kNeeded = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 5);

// Vote register fields
constexpr uint32_t kVote8 = kVoteEnable;
constexpr uint32_t kVote0 = kVoteEnable2;
constexpr uint32_t kVote8Bits = kVoteMask;
constexpr uint32_t kVote0Bits = kVoteMask2;

// ---- QMP PHY init tables ----
// TODO(hardware): these tables must come from the downstream SM7675 kernel
// (qmp-pcie-cliffs.c or equivalent). The structure is pairs of (offset, value).
// Placeholder: empty tables, which means the PHY init writes nothing.
namespace wifi_phy {
struct Init { uint32_t offset; uint32_t value; };
constexpr Init kSerdes[] = {
    // TODO(hardware): fill from downstream qcom,qmp-pcie-sm7675 serdes init
};
constexpr Init kTx[] = {
    // TODO(hardware): fill from downstream TX lane init
};
constexpr Init kRx[] = {
    // TODO(hardware): fill from downstream RX lane init
};
constexpr Init kPcs[] = {
    // TODO(hardware): fill from downstream PCS init
};
constexpr Init kPcsMisc[] = {
    // TODO(hardware): fill from downstream PCS-MISC init
};
constexpr uint32_t kTx2 = 0x800;   // TODO(hardware): lane 2 offset
constexpr uint32_t kRx2 = 0x800;   // TODO(hardware): lane 2 offset
constexpr uint32_t kPowerDownControl = 0x804; // TODO(hardware): verify
constexpr uint32_t kSwReset = 0x808;          // TODO(hardware): verify
constexpr uint32_t kStartControl = 0x80c;     // TODO(hardware): verify
constexpr uint32_t kPcsStatus1 = 0x814;       // TODO(hardware): verify
} // namespace wifi_phy

// ---- DMA memory allocator ----
// Carved from the buffer the storage driver mapped for the endpoint's SMMU stream.
struct Dma { uint8_t *va; uint64_t pa; size_t bytes; };
uint32_t gDmaUsed;

bool dmaAlloc(Dma &b, size_t bytes) {
    if (&native_ufs_extra_va == nullptr || !native_ufs_extra_va ||
        gDmaUsed + bytes > native_ufs_extra_bytes) {
        native_wifi_mhi[11] = 1;
        return false;
    }
    b.va = native_ufs_extra_va + gDmaUsed;
    b.pa = native_ufs_extra_iova + gDmaUsed;
    b.bytes = bytes;
    gDmaUsed += (uint32_t)((bytes + 63) & ~63ull);
    return true;
}

// ---- Stage 2: link training ----
void bringUp(Window &gcc, Window &tlmm) {
    volatile uint32_t *g = gcc.va, *t = tlmm.va;
    pmicPrepare();
    const int rails = powerRails();
    IOSleep(20);
    if (rails) { native_wifi_stats[0] = -9; return; }
    IOSleep(5);

    // PERST# low, CLKREQ# to PCIe function, then WLAN_EN cycle
    setPin(t, kPerst, 1u | 1u << 9, 0);
    setPin(t, kClkReq, 3u | 1u << 2, -1);
    setPin(t, kWlanEn, 1u | 3u << 6 | 1u << 9, 0);
    IOSleep(150);
    setPin(t, kWlanEn, 1u | 3u << 6 | 1u << 9, 1);
    IOSleep(60);
    native_wifi_stats[10] = pinWord(t);
    native_wifi_stats[0] = 10;

    // GDSCs: clear collapse bits for PCIe0 (bit 0) and PHY (bit 3)
    g[kCollapse / 4] &= ~(1u << 0 | 1u << 3);
    unsigned tries = 0;
    while (tries++ < 100 && (!(g[kGdsc / 4] & (1u << 31)) || !(g[kPhyGdsc / 4] & (1u << 31))))
        IODelay(10);
    uint32_t power = (g[kGdsc / 4] >> 31) | (g[kPhyGdsc / 4] >> 31) << 1;
    native_wifi_stats[11] = power;
    if (power != 3) { native_wifi_stats[0] = -11; return; }
    native_wifi_stats[0] = 11;

    // Clock sources: aux and phy-rchng RCGs, pipe mux parked on TCXO
    g[(kAuxRcg + 4) / 4] = 0;
    g[kAuxRcg / 4] |= 1u;
    g[(kRchngRcg + 4) / 4] = 6u << 8 | 5u; // TODO(hardware): verify divider
    g[kRchngRcg / 4] |= 1u;
    waitClear(&g[kAuxRcg / 4], 1u, 100);
    waitClear(&g[kRchngRcg / 4], 1u, 100);
    g[kPipeMux / 4] = (g[kPipeMux / 4] & ~3u) | 2u; // TCXO source
    g[kVote8 / 4] |= kVote8Bits;
    g[kVote0 / 4] |= kVote0Bits;

    // TCSR clock reference (if needed)
    Window clkref = {};
    if (mapWindow(clkref, kTcsrClkref, 0x1000)) {
        clkref.va[0x100 / 4] |= 1u;
        release(clkref);
    }
    IODelay(200);

    uint32_t off = 0;
    for (unsigned i = 0; i < sizeof kBranches / sizeof kBranches[0]; ++i)
        if (g[kBranches[i] / 4] & (1u << 31)) off |= 1u << i;
    native_wifi_stats[11] = power | off << 8;
    if (off & ~(1u << 4)) { native_wifi_stats[0] = -12; return; }
    pulseReset(g, kPcieBcr);
    pulseReset(g, kPhyBcr);
    native_wifi_stats[0] = 12;

    // PARF init
    Window parf = {}, dbi = {};
    if (!mapWindow(parf, kParf, 0x3000) || !mapWindow(dbi, kDbi, 0x1000)) {
        release(parf); native_wifi_stats[0] = -13; return;
    }
    volatile uint32_t *f = parf.va, *d = dbi.va;
    f[kParfDeviceType / 4] = 4u;
    f[kParfPhyCtrl / 4] &= ~1u;
    f[kParfDbiBase / 4] = uint32_t(kDbi);
    f[kParfDbiBaseHi / 4] = uint32_t(kDbi >> 32);
    f[kParfAtuBase / 4] = uint32_t(kAtu);
    f[kParfAtuBaseHi / 4] = uint32_t(kAtu >> 32);
    f[kParfSysCtrl / 4] &= ~(1u << 29);
    f[kParfMhiClockReset / 4] |= 1u << 4;
    f[kParfPmCtrl / 4] &= ~(1u << 5);
    f[kParfAxiHalt / 4] |= 1u << 31;
    native_wifi_stats[0] = 13;

    // QMP PHY init
    Window phy = {};
    if (!mapWindow(phy, kPhy, 0x2000)) {
        release(dbi); release(parf); native_wifi_stats[0] = -14; return;
    }
    volatile uint32_t *p = phy.va;
    using namespace wifi_phy;
    p[kPowerDownControl / 4] |= 3u;
    for (const Init &e : kSerdes) p[e.offset / 4] = e.value;
    for (const Init &e : kTx) { p[e.offset / 4] = e.value; p[(e.offset + kTx2) / 4] = e.value; }
    for (const Init &e : kRx) { p[e.offset / 4] = e.value; p[(e.offset + kRx2) / 4] = e.value; }
    for (const Init &e : kPcs) p[e.offset / 4] = e.value;
    for (const Init &e : kPcsMisc) p[e.offset / 4] = e.value;
    g[kPipeMux / 4] &= ~3u; // switch to PHY pipe clock
    p[kSwReset / 4] &= ~1u;
    p[kStartControl / 4] |= 3u;
    IODelay(1000);
    unsigned polls = 0;
    const bool ready = waitClear(&p[kPcsStatus1 / 4], 1u << 6, 100, &polls);
    native_wifi_stats[12] = (p[kPcsStatus1 / 4] & 0xffffu) | polls << 16;
    release(phy);
    if (!ready) { release(dbi); release(parf); native_wifi_stats[0] = -14; return; }

    // SMMU stream ID hash table (CRC-8)
    // requester 0x100 (bus 1 endpoint) → stream 0x1401
    // TODO(hardware): verify stream ID from iommu-map in DT
    {
        for (unsigned i = 0; i < 256; ++i) f[(0x2000 + 4 * i) / 4] = 0;
        f[0x2c00 / 4] &= ~1u; // bypass off
        auto crc8 = [](uint16_t bdf) {
            uint8_t crc = 0;
            const uint8_t bytes[2] = {(uint8_t)(bdf >> 8), (uint8_t)bdf};
            for (uint8_t byte : bytes) {
                crc ^= byte;
                for (int bit = 0; bit < 8; ++bit)
                    crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 7) : (uint8_t)(crc << 1);
            }
            return crc;
        };
        const uint8_t hash = crc8(0x100);
        f[(0x2000 + 4 * hash) / 4] = 0x100u << 16 | 1u << 8;
    }

    // Root port bus numbers and PERST# release
    d[kBusNumbers / 4] = 0x00010100;
    d[kCommand / 4] |= 0x6u;
    native_wifi_stats[7] = d[0];
    native_wifi_stats[0] = 14;

    IOSleep(100);
    setPin(t, kPerst, 1u | 1u << 9, 1); // release PERST#
    IODelay(1000);
    native_wifi_stats[0] = 15;

    // LTSSM enable and link training
    f[kParfLtssm / 4] |= 1u << 8;
    unsigned ms = 0;
    for (; ms < 1000; ++ms) {
        if ((d[kPortDebug1 / 4] & (1u << 4)) && (d[kPortDebug0 / 4] & 0x3f) == 0x11) break;
        IOSleep(1);
    }
    native_wifi_stats[13] = d[kPortDebug0 / 4];
    native_wifi_stats[14] = d[kPortDebug1 / 4];
    native_wifi_stats[6] = f[kParfLtssm / 4];
    native_wifi_stats[8] = d[kLinkControl / 4];
    if (ms == 1000) { release(dbi); release(parf); native_wifi_stats[0] = -16; return; }
    native_wifi_stats[0] = 16;

    // iATU region 0: config space for bus 1
    Window atu = {}, config = {};
    if (mapWindow(atu, kAtu, 0x1000) && mapWindow(config, kConfig, 0x1000)) {
        volatile uint32_t *a = atu.va;
        a[0x08 / 4] = uint32_t(kConfig);
        a[0x0c / 4] = uint32_t(kConfig >> 32);
        a[0x10 / 4] = uint32_t(kConfig + 0xfffff);
        a[0x20 / 4] = uint32_t((kConfig + 0xfffff) >> 32);
        a[0x14 / 4] = 1u << 24;
        a[0x18 / 4] = 0;
        a[0x00 / 4] = 0x4;
        a[0x04 / 4] = 1u << 31;
        for (unsigned i = 0; i < 5 && !(a[0x04 / 4] & (1u << 31)); ++i) IODelay(10);
        native_wifi_stats[15] = config.va[0]; // vendor:device
        native_wifi_stats[0] = 17;
        if ((config.va[0] & 0xffff) == 0x17cb) // Qualcomm vendor ID
            endpointProbe(a, config.va, d);
    } else native_wifi_stats[0] = -17;
    release(config); release(atu); release(dbi); release(parf);
}

// ---- Stage 3: endpoint BAR and MHI registers ----
volatile uint32_t *gCfg, *gDbi;
uint32_t gBar0;

void endpointProbe(volatile uint32_t *a, volatile uint32_t *c, volatile uint32_t *d) {
    gCfg = c; gDbi = d;
    volatile uint32_t *ep = native_wifi_ep;

    // iATU region 1: memory window
    volatile uint32_t *r1 = a + 0x200 / 4;
    r1[0x08 / 4] = uint32_t(kMemBase);
    r1[0x0c / 4] = uint32_t(kMemBase >> 32);
    r1[0x10 / 4] = uint32_t(kMemBase + kMemSize - 1);
    r1[0x20 / 4] = uint32_t((kMemBase + kMemSize - 1) >> 32);
    r1[0x14 / 4] = uint32_t(kMemBase);
    r1[0x18 / 4] = uint32_t(kMemBase >> 32);
    r1[0x00 / 4] = 0x0;
    r1[0x04 / 4] = 1u << 31;
    for (unsigned i = 0; i < 5 && !(r1[0x04 / 4] & (1u << 31)); ++i) IODelay(10);

    // Type 1 header memory base/limit
    d[0x20 / 4] = uint32_t(((kMemBase + kMemSize - 1) & 0xfff00000u) |
                            ((kMemBase >> 16) & 0xfff0u));
    ep[0] = 1;

    // BAR0 sizing
    const uint32_t saved = c[0x10 / 4];
    c[0x10 / 4] = 0xffffffffu;
    const uint32_t mask = c[0x10 / 4];
    c[0x10 / 4] = saved;
    ep[1] = mask;
    const uint32_t size = ~(mask & ~0xfu) + 1;
    if (!(mask & ~0xfu) || !size || size > kMemSize) return;

    const uint32_t address = uint32_t((kMemBase + size - 1) & ~uint64_t(size - 1));
    c[0x10 / 4] = address | (mask & 0xfu);
    if ((mask & 6u) == 4u) c[0x14 / 4] = 0;
    c[0x04 / 4] = (c[0x04 / 4] & 0xffffu) | 0x6u; // memory space + bus master
    gBar0 = address | (mask & 0xfu);
    ep[2] = c[0x10 / 4];
    ep[3] = c[0x04 / 4];
    ep[0] = 2;

    Window bar = {};
    if (!mapWindow(bar, address, size < 0x200000 ? size : 0x200000)) return;
    volatile uint32_t *m = bar.va;
    // Read MHI registers
    static const uint32_t regs[7] = {0x00, 0x08, 0x10, 0x28, 0x2c, 0x38, 0x48};
    for (unsigned i = 0; i < 7; ++i) ep[5 + i] = m[regs[i] / 4];
    ep[0] = 3;

    uint32_t level = 0;
    PE_parse_boot_argn("uke-wifi", &level, sizeof level);
    if (level >= 5) {
        ep[0] = 4;
        wifiStack(m, size < 0x200000 ? size : 0x200000);
        release(bar);
        return;
    }
    if (level >= 4) {
        // Stage 4: MHI boot (firmware download)
        ep[0] = 4;
        mhiBring(m, size < 0x20000 ? size : 0x20000);
    }
    release(bar);
}

// ---- Stage 4: MHI firmware download (skeleton) ----
void mhiBring(volatile uint32_t *m, uint64_t barBytes) {
    volatile uint32_t *st = native_wifi_mhi;
    // TODO(hardware): full MHI boot implementation.
    // Pattern: allocate DMA segments, find firmware in the oops partition,
    // load SBL into DMA buffers, build BHIe scatter-gather table,
    // program MHI channel contexts, write BHI doorbell, wait for boot phases.
    pLog(6, "uke-wifi: stage 4 MHI boot — TODO(hardware): firmware download");
    st[0] = 1;

    // Check that the SMMU pass-through stream was set up by the storage driver
    st[1] = native_ufs_extra_stream;
    if (&native_ufs_extra_stream == nullptr || !(st[1] & 0x80000000u)) {
        st[0] = (uint32_t)-4;
        pLog(3, "uke-wifi: SMMU stream not ready (storage driver did not set it up)");
        return;
    }
    st[0] = 4;
    // TODO(hardware): the rest of MHI boot (SBL load, BHIe image, context programming)
    // requires the firmware container on the oops partition and confirmed DMA addresses.
}

// ---- Stage 5: WiFi module start (skeleton) ----
extern "C" void uke_wifi_net_start(const uint8_t mac[6]) __asm__("_uke_wifi_net_start");

void wifiStack(volatile uint32_t *m, uint64_t barBytes) {
    volatile uint32_t *st = native_wifi_mhi;
    // TODO(hardware): full implementation once MHI boot works.
    // Pattern: init DMA arena, find MHI capabilities in BAR, set up
    // lx_platform + nw_board, call nw_wifi_start(), attach MSI interrupt,
    // then enter the poll/sleep loop calling nw_wifi_step().
    pLog(6, "uke-wifi: stage 5 wifi module — TODO(hardware): nw_wifi_start");
    st[0] = 20;

    // The network classes and interface are registered once the module reports
    // its MAC address:
    //   OSMetaClass::preModLoad / postModLoad for the IOKit classes
    //   uke_wifi_net_start(mac) to create the ifnet
    // Then the main loop runs nw_wifi_step() on MSI interrupts or 1ms polling.
}

// ---- Entry: WiFi thread ----
void wifiThread(void *arg, wait_result_t) {
    thread_set_thread_name(current_thread(), "uke-wifi");
    Window gcc = {}, tlmm = {};
    if (!mapWindow(gcc, kGcc, 0x80000) || !mapWindow(tlmm, kTlmm, 0x60000)) {
        native_wifi_stats[0] = -1; release(gcc); release(tlmm); return;
    }
    native_wifi_stats[0] = 1;
    native_wifi_stats[1] = gcc.va[kVote8 / 4];
    native_wifi_stats[2] = gcc.va[kVote0 / 4];
    native_wifi_stats[3] = gcc.va[kGdsc / 4];
    uint32_t off = 0;
    for (unsigned i = 0; i < sizeof kBranches / sizeof kBranches[0]; ++i)
        if (gcc.va[kBranches[i] / 4] & (1u << 31)) off |= 1u << i;
    native_wifi_stats[4] = off;
    native_wifi_stats[5] = pinWord(tlmm.va);
    native_wifi_stats[0] = 2;

    if (reinterpret_cast<uintptr_t>(arg) >= 2) {
        bringUp(gcc, tlmm);
        release(tlmm); release(gcc);
        return;
    }
    // Stage 1 only: read PARF/DBI if link is already up
    Window parf = {}, dbi = {};
    const uint32_t gdsc = gcc.va[kGdsc / 4];
    if ((gdsc & (1u << 31)) && !(off & kNeeded) &&
        mapWindow(parf, kParf, 0x3000) && mapWindow(dbi, kDbi, 0x1000)) {
        native_wifi_stats[6] = parf.va[kParfLtssm / 4];
        native_wifi_stats[9] = parf.va[kParfPhyCtrl / 4];
        native_wifi_stats[7] = dbi.va[0];
        native_wifi_stats[8] = dbi.va[kLinkControl / 4];
        native_wifi_stats[0] = 3;
    }
    release(dbi); release(parf); release(tlmm); release(gcc);
}

} // namespace

extern "C" int uke_wifi_file(const char *name, const uint8_t **data, size_t *size);
extern "C" int uke_wifi_file(const char *, const uint8_t **, size_t *) { return -1; /* TODO(hardware) */ }

extern "C" void uke_wifi_probe() __asm__("_uke_wifi_probe");
extern "C" void uke_wifi_probe() {
    unsigned stage = 0;
    if (!PE_parse_boot_argn("uke-wifi", &stage, sizeof stage) || stage < 1 || native_wifi_stats[0]) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(wifiThread, reinterpret_cast<void *>(uintptr_t(stage)), &thread) == KERN_SUCCESS)
        thread_deallocate(thread);
    else native_wifi_stats[0] = -7;
}
