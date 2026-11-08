// SPDX-License-Identifier: BSD-2-Clause-Patent
// Audio bring-up on the uke board (SM7675 / Snapdragon 7+ Gen 3).
//
// The SM7675 audio subsystem consists of:
//   - LPASS (Low Power Audio SubSystem) with hardware codec interface
//   - Audio codec: likely WCD9395 (SoundWire or slim bus connection)
//   - Speaker amplifiers: likely WSA8835 or WSA8840 (4 speakers, stereo)
//   - Digital microphones: DMIC via SoundWire
//
// Audio on iPadOS runs through:
//   CoreAudio → IOAudio2Family → codec driver → hardware
//
// The kernel prelink already includes com.apple.iokit.IOAudio2Family and
// com.apple.driver.AppleAudioClockLibs. What is needed:
//   1. An IOAudioDevice subclass that probes the codec over SoundWire/SlimBus
//   2. An IOAudioEngine that configures the sample rate, format, DMA buffers
//   3. Speaker amplifier initialization (WSA power-up sequence)
//
// HARDWARE VERIFICATION REQUIRED:
//   - Exact codec chip (WCD9390 vs WCD9395 vs other)
//   - Speaker amplifier model and count
//   - SoundWire controller base address and interrupt
//   - LPASS DMA channel assignments
//   - Codec register init sequence (from downstream kernel audio driver)
//
// Stage 1 (uke-audio=1): probe the SoundWire bus, identify connected devices
// Stage 2 (uke-audio=2): initialize codec, configure sample format
// Stage 3 (uke-audio=3): register IOAudioDevice/IOAudioEngine with the system
//
// The audio driver is lower priority than WiFi and display; it can be developed
// after the system boots to userland. The text_fixups in board.toml already
// disable BTAudioHALPlugin to prevent audiomxd from hanging at startup.
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <stdint.h>

namespace {

// ---- SM7675 LPASS / audio subsystem addresses ----
// Source: SM7635 DTS (lpass@6d00000), downstream audio drivers for "cliffs" platform.
// The LPASS on SM7675 ("cliffs") follows the same layout as SM8550 ("kalama").
// TODO(hardware): all addresses need verification from DT dump or register scan.

// LPASS core: the audio DSP's register interface
constexpr uint64_t kLpassCore = 0x06d00000; // TODO(hardware): verify
constexpr uint64_t kLpassCoreSize = 0x50000;

// LPASS HDMI TX: digital audio output (may not be used on Pad 7)
constexpr uint64_t kLpassHdmiTx = 0x06d60000; // TODO(hardware): verify

// SoundWire controllers: master ports for codec/amp/mic connection
// SM7675 has up to 4 SoundWire masters in LPASS. The downstream DTS for cliffs
// lists them at offsets within the LPASS range.
constexpr uint64_t kSwrBase[] = {
    0x06d30000,  // SWR0: primary SoundWire (codec WCD9395)  — TODO(hardware): verify
    0x06d50000,  // SWR1: speaker SoundWire (WSA8835/8840)   — TODO(hardware): verify
    0x06d70000,  // SWR2: RX SoundWire (if separate)         — TODO(hardware): verify
    0x06d90000,  // SWR3: TX SoundWire (digital mic capture)  — TODO(hardware): verify
};
constexpr uint64_t kSwrSize = 0x10000;

// SoundWire register offsets (common to all Qualcomm SoundWire controllers)
constexpr uint32_t kSwrComp    = 0x00; // component status
constexpr uint32_t kSwrMaster  = 0x14; // master ID / version
constexpr uint32_t kSwrDevId0  = 0x40; // connected device IDs start here
constexpr uint32_t kSwrClock   = 0x20; // clock control
constexpr uint32_t kSwrStatus  = 0x30; // bus status
constexpr uint32_t kSwrCmd     = 0x34; // command FIFO
constexpr uint32_t kSwrIrq     = 0x50; // interrupt status

// LPASS DMA channels — for moving PCM data between memory and codecs
// Each channel has a set of registers at kLpassDma + channel * 0x1000
constexpr uint64_t kLpassDma = 0x06d10000; // TODO(hardware): verify DMA engine base
constexpr uint64_t kLpassDmaSize = 0x10000;

// DMA register offsets (per channel, offset = channel * 0x1000)
constexpr uint32_t kDmaCtl     = 0x00;
constexpr uint32_t kDmaBufBase = 0x04;
constexpr uint32_t kDmaBufSize = 0x08;
constexpr uint32_t kDmaPeriod  = 0x0c;
constexpr uint32_t kDmaStatus  = 0x10;

// Audio codec: expected WCD9395 (Qualcomm's main codec for SM7675 tier)
// SoundWire device ID for WCD939x is 0x0217020a0100 (verify from DT dump)
constexpr uint64_t kWcd9395DevId = 0x0217020a0100ull; // TODO(hardware): verify
// Codec LPASS macro register base (the digital codec within LPASS)
constexpr uint64_t kLpassWcdBase = 0x06da0000; // TODO(hardware): verify
constexpr uint64_t kLpassWcdSize = 0x10000;

// Speaker amplifier: expected WSA8835 or WSA8840 (stereo/quad)
// SoundWire device IDs for WSA88xx
constexpr uint64_t kWsa8835DevId = 0x0217020a0200ull; // TODO(hardware): verify
constexpr uint64_t kWsa8840DevId = 0x0217020a0300ull; // TODO(hardware): verify

// GCC audio clocks (from gcc-cliffs.c)
// The audio PLL and branch clocks need to be enabled before LPASS access.
constexpr uint64_t kGcc = 0x100000;
constexpr uint32_t kGccLpassVote = 0x7e00c; // TODO(hardware): verify vote register

// Window helper (same as WifiProbe)
struct Window { IOMemoryMap *map; volatile uint32_t *va; };

bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    IODeviceMemory *mem = IODeviceMemory::withRange(base, size);
    w.map = mem ? mem->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (mem) mem->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}

void release(Window &w) { if (w.map) w.map->release(); w = {}; }

// Diagnostics exported to the boot log reader
extern "C" {
volatile uint32_t native_audio_stats[16];
char native_audio_text[64][112];
volatile uint32_t native_audio_text_n;
}

void aLog(int level, const char *line) {
    const uint32_t i = __atomic_fetch_add(
        const_cast<uint32_t *>(&native_audio_text_n), 1u, __ATOMIC_RELAXED) % 64;
    snprintf(native_audio_text[i], sizeof native_audio_text[i], "%d %.100s", level, line);
}

// ---- Stage 1: SoundWire bus probe ----
// Maps each SoundWire controller and reads the device ID registers.
// This is read-only: it identifies what codecs and amps are connected.
void probeStage1() {
    native_audio_stats[0] = 1;

    for (unsigned i = 0; i < 4; ++i) {
        Window swr = {};
        if (!mapWindow(swr, kSwrBase[i], kSwrSize)) {
            native_audio_stats[1 + i] = 0xfffffffe;
            continue;
        }
        // Read controller version and bus status
        const uint32_t master = swr.va[kSwrMaster / 4];
        const uint32_t status = swr.va[kSwrStatus / 4];
        const uint32_t devId0 = swr.va[kSwrDevId0 / 4];
        native_audio_stats[1 + i] = master;
        native_audio_stats[5 + i] = status;
        native_audio_stats[9 + i] = devId0;

        char msg[112];
        snprintf(msg, sizeof msg,
                 "uke-audio: SWR%u master=%08x status=%08x dev0=%08x",
                 i, master, status, devId0);
        aLog(6, msg);
        release(swr);
    }

    // Try to map LPASS core to check if audio subsystem is accessible
    Window lpass = {};
    if (mapWindow(lpass, kLpassCore, 0x1000)) {
        native_audio_stats[13] = lpass.va[0]; // LPASS version/ID
        aLog(6, "uke-audio: LPASS core mapped OK");
        release(lpass);
    } else {
        native_audio_stats[13] = 0xfffffffe;
        aLog(3, "uke-audio: LPASS core map failed — clock gate or SMMU?");
    }
}

// ---- Stage 2: codec init (skeleton) ----
void probeStage2() {
    native_audio_stats[0] = 2;
    // TODO(hardware): full codec initialization.
    // Pattern: enable GCC audio clocks, bring LPASS out of reset,
    // enumerate SoundWire devices, match WCD9395 and WSA88xx,
    // configure sample rate (48 kHz), bit depth (24-bit), channels.
    aLog(6, "uke-audio: stage 2 codec init — TODO(hardware)");
}

// ---- Stage 3: IOAudio registration (skeleton) ----
void probeStage3() {
    native_audio_stats[0] = 3;
    // TODO(hardware): register IOAudioDevice and IOAudioEngine with the kernel.
    // Pattern: OSMetaClass::preModLoad/postModLoad for audio classes,
    // create an IOAudioDevice subclass, attach an IOAudioEngine with
    // the configured DMA ring buffers and sample format.
    aLog(6, "uke-audio: stage 3 IOAudio registration — TODO(hardware)");
}

} // namespace

extern "C" void uke_audio_probe() __asm__("_uke_audio_probe");
extern "C" void uke_audio_probe() {
    uint32_t stage = 0;
    if (!PE_parse_boot_argn("uke-audio", &stage, sizeof stage) || !stage) return;

    if (stage >= 1) probeStage1();
    if (stage >= 2) probeStage2();
    if (stage >= 3) probeStage3();
}
