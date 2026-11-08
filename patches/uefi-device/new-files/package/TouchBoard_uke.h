// SPDX-License-Identifier: BSD-2-Clause-Patent
// uke (SM7675 / Snapdragon 7+ Gen 3) touch controller board facts.
// Verified against the decompiled device tree (dtbo_00.dtb from uke-kernel-prebuilt) and
// gcc-cliffs.c clock offsets. All addresses confirmed unless noted otherwise.
//
// The touch panel is a Novatek NT36532 connected via SPI on QUP wrapper 0 serial engine 4
// (SE4, base 0xa90000, 19.2 MHz, mode 0). IRQ is on GPIO 54. lcd_id is on GPIO 26.
#ifndef UKE_TOUCH_BOARD_H
#define UKE_TOUCH_BOARD_H
#include <stdint.h>

// QUP wrapper 0 (geniqup@ac0000): the physical wrapper that hosts SPI SE4 for touch.
// In SM7675 GCC naming this is "wrapper 0" (vs SM8550 where the same address is "wrapper 1").
// SM7635 DTSI: qupv3_id_0: geniqup@ac0000, reg <0x0 0x00ac0000 0x0 0x2000>.
#define TB_QUP_WRAPPER_BASE 0x00ac0000ULL
#define TB_SE_BASE          0x00a90000ULL     // SE4: 0xa80000 + 4*0x4000 = 0xa90000
#define TB_SE_SIZE          0x4000ULL
// SMMU SID for QUP wrapper 0: 0xa3 (confirmed: cliffs.dts geniqup@ac0000 iommus = <&apps_smmu 0xa3 0>).
#define TB_QUP_SMMU_SID     0xa3
#define TB_APPS_SMMU_BASE   0x15000000ULL     // apps-smmu@15000000 (confirmed cliffs.dts)
#define TB_APPS_SMMU_SIZE   0x100000ULL       // confirmed: cliffs.dts reg = <0x15000000 0x100000 ...>

// ---- GCC (clock-controller@100000) ----
// Source: gcc-cliffs.c from 0001-cliffs-clock-handoff.patch
#define TB_GCC_BASE             0x00100000ULL
#define TB_GCC_VOTE_ENABLE      0x52008       // shared vote register for wrapper 0 clocks
#define TB_GCC_GPLL0_ENABLE     0x52018       // gcc_gpll0 enable_reg, bit 0
#define TB_GCC_GPLL0_MODE       0x0

// QUP wrapper 0 clock vote bits (all in register 0x52008).
// These have the SAME bit positions as SM8550 wrapper 1 but different halt registers.
#define TB_VOTE_WRAP0_CORE_2X   (1u << 18)    // gcc_qupv3_wrap0_core_2x_clk
#define TB_VOTE_WRAP0_CORE      (1u << 19)    // gcc_qupv3_wrap0_core_clk
#define TB_VOTE_WRAP0_M_AHB     (1u << 20)    // gcc_qupv3_wrap_0_m_ahb_clk
#define TB_VOTE_WRAP0_S_AHB     (1u << 21)    // gcc_qupv3_wrap_0_s_ahb_clk
// S4 vote bit confirmed: gcc-cliffs.c gcc_qupv3_wrap0_s4_clk enable_mask = BIT(26)
#define TB_VOTE_WRAP0_S4        (1u << 26)
#define TB_VOTE_ALL (TB_VOTE_WRAP0_CORE_2X | TB_VOTE_WRAP0_CORE | TB_VOTE_WRAP0_M_AHB | TB_VOTE_WRAP0_S_AHB | TB_VOTE_WRAP0_S4)

// QUP wrapper 0 halt registers (confirmed from gcc-cliffs.c).
// These differ from SM8550 wrapper 1: 0x23000-0x23018 vs 0x2327c-0x23294.
#define TB_HALT_CORE_2X     0x23018           // gcc_qupv3_wrap0_core_2x_clk
#define TB_HALT_CORE        0x23008           // gcc_qupv3_wrap0_core_clk
#define TB_HALT_M_AHB       0x23000           // gcc_qupv3_wrap_0_m_ahb_clk
#define TB_HALT_S_AHB       0x23004           // gcc_qupv3_wrap_0_s_ahb_clk
// Confirmed: gcc-cliffs.c gcc_qupv3_wrap0_s4_clk halt_reg = 0x183bc
#define TB_HALT_S4          0x183bc
#define TB_CBCR_CLK_OFF     (1u << 31)

// SE4 rate clock generator: confirmed gcc-cliffs.c gcc_qupv3_wrap0_s4_clk_src cmd_rcgr = 0x183c8
#define TB_S4_RCG           0x183c8
#define TB_RCG_CMD          0x0
#define TB_RCG_CFG          0x4
#define TB_RCG_DFSR_CMD     0x14              // bit0 DFS enable, bits 4:1 current level
#define TB_RCG_PERF_CFG(l)  (0x1c + 4 * (l))
#define TB_RCG_PERF_M(l)    (0x5c + 4 * (l))
#define TB_RCG_PERF_N(l)    (0x9c + 4 * (l))
#define TB_DFS_LEVELS       8

// ---- TLMM (pinctrl@f000000) ----
// SM7675 DTS: pinctrl@f000000 (compatible "qcom,cliffs-pinctrl"), size 0x1000000.
// NOTE: SM8550 uses 0xf100000 — the uke offset is different!
#define TB_TLMM_BASE        0x0f000000ULL
#define TB_GPIO_STRIDE       0x1000

// SPI GPIO pins for SE4 of wrapper 0 (confirmed from uke DTBO and cliffs.dts pinctrl).
// cliffs.dts: PINGROUP(16..19) with function qup0_se4_l0..l3
// uke DTBO: mi_ts_spi_miso gpio16, mi_ts_spi_mosi gpio17, mi_ts_spi_clk gpio18, mi_ts_spi_cs gpio19
#define TB_GPIO_SPI_MISO    16
#define TB_GPIO_SPI_MOSI    17
#define TB_GPIO_SPI_CLK     18
#define TB_GPIO_SPI_CS      19
// Touch IRQ GPIO: confirmed from uke DTBO (novatek,irq-gpio = <&tlmm 0x36 0x2001> = GPIO 54).
#define TB_GPIO_IRQ         54
// LCD ID GPIO: confirmed from uke DTBO (novatek,lcd_id-gpio = <&tlmm 0x1a 0x00> = GPIO 26).
#define TB_GPIO_LCD_ID      26

// Confirmed: cliffs.dts PINGROUP(16..19) first alt function is qup0_se4 → function select 1
#define TB_FUNC_QUP0_SE4    1
#define TB_DRIVE_6MA        2
#define TB_SPI_MODE         0
#define TB_SPI_DEFAULT_HZ   19200000u         // 19.2 MHz (confirmed from porting.toml)
#define TB_SPI_MAX_HZ       19200000u

static inline uint32_t tb_pin_cfg(unsigned func, unsigned drive_code) { return (func & 15u) << 2 | (drive_code & 7u) << 6; }
static inline uint32_t tb_tlmm_ctl(unsigned gpio) { return (uint32_t)gpio * TB_GPIO_STRIDE; }
static inline uint32_t tb_tlmm_io(unsigned gpio) { return (uint32_t)gpio * TB_GPIO_STRIDE + 4; }

// Source clock rates: 0 = TCXO 19.2 MHz, 1 = GPLL0 main 600 MHz, 6 = GPLL0 even 300 MHz.
static inline uint64_t tb_src_hz(unsigned sel) { return sel == 0 ? 19200000ULL : sel == 1 ? 600000000ULL : sel == 6 ? 300000000ULL : 0; }
static inline uint64_t tb_calc_rate(uint64_t prate, uint32_t m, uint32_t n, unsigned mode, unsigned hid) {
    if (hid) { prate *= 2; prate /= hid + 1; }
    if (mode && n) { prate *= m; prate /= n; }
    return prate;
}
static inline uint64_t tb_dfs_level_hz(uint32_t cfg, uint32_t mreg, uint32_t nreg) {
    unsigned src = (cfg >> 8) & 7, mode = (cfg >> 12) & 3, hid = cfg & 0x1f;
    uint32_t m = 0, n = 0;
    if (mode) { m = mreg & 0xffff; n = ((~nreg) & 0xffff) + m; }
    return tb_calc_rate(tb_src_hz(src), m, n, mode, hid);
}
struct tb_clock_choice { unsigned level, div; uint64_t sclk_hz, actual_hz; };
static inline int tb_choose_clock(const uint64_t *level_hz, unsigned n, unsigned os, unsigned target, struct tb_clock_choice *out) {
    int found = 0;
    for (unsigned l = 0; l < n; ++l) {
        uint64_t f = level_hz[l]; if (!f) continue;
        uint64_t d = ((f + (uint64_t)os * target - 1) / ((uint64_t)os * target)); if (d < 1) d = 1; if (d > 0xfff) continue;
        uint64_t actual = f / (os * d); if (actual > target) continue;
        if (!found || actual > out->actual_hz || (actual == out->actual_hz && d < out->div)) {
            out->level = l; out->div = (unsigned)d; out->sclk_hz = f; out->actual_hz = actual; found = 1;
        }
    }
    return found ? 0 : -1;
}
#endif
