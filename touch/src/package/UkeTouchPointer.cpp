// SPDX-License-Identifier: BSD-2-Clause-Patent
// UkeTouchPointer: the uke NT36532 touch panel as a macOS absolute pointer (one button, X/Y over the whole screen).
// macOS has no touch UI, so the first contact moves the pointer and holds button 1; lifting releases it. The finger
// set comes from TouchProbe.cpp (native_touch_* seqlock + sink), which this service also starts (boot-arg sheng-touch).
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>

extern "C" {
extern volatile uint32_t native_touch_seq, native_touch_count;
extern volatile int native_touch_xs[], native_touch_ys[];
void native_touch_set_sink(void (*sink)(void *), void *context);
void native_touch_probe(void);
// TouchProbe's heartbeat prints the iOS HID gate state; there is no such gate here.
volatile uint32_t native_hid_input_events[8], native_hid_input_results[8], native_touch_edge_stats[4];
volatile uint32_t native_hid_stage, native_hid_resource_found;
volatile int native_hid_load_status, native_hid_matching_load_status, native_hid_input_meta_status;
volatile uint32_t native_hid_class_sizes[5];
volatile uint64_t native_hid_input_object;
volatile uint32_t uke_touch_pointer_reports;
}

// NT36532 report units (NvtTouchCore.h): long axis 0..30479, short axis 0..20319.
static const int kTouchMaxX = 30479, kTouchMaxY = 20319;

static const uint8_t kDescriptor[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01,                     // Generic Desktop, Mouse, Application
    0x09, 0x01, 0xa1, 0x00,                                 //   Pointer, Physical
    0x05, 0x09, 0x19, 0x01, 0x29, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,   // button 1
    0x75, 0x07, 0x95, 0x01, 0x81, 0x03,                     //   padding
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff, 0x7f, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02,   // X, Y absolute
    0xc0, 0xc0,
};
struct __attribute__((packed)) Report { uint8_t buttons; uint16_t x, y; };

class UkeTouchPointer : public IOHIDDevice {
    OSDeclareDefaultStructors(UkeTouchPointer)
    uint32_t lastSeq = 0;
    bool down = false;
    uint16_t lastX = 0, lastY = 0;

    static void sink(void *context) { static_cast<UkeTouchPointer *>(context)->deliver(); }
    void deliver() {
        uint32_t seq, count; int x = -1, y = -1;
        do {
            seq = __atomic_load_n(&native_touch_seq, __ATOMIC_ACQUIRE);
            if (seq & 1) continue;
            count = native_touch_count;
            if (count) { x = native_touch_xs[0]; y = native_touch_ys[0]; }
        } while ((seq & 1) || seq != __atomic_load_n(&native_touch_seq, __ATOMIC_ACQUIRE));
        if (seq == lastSeq) return;
        lastSeq = seq;
        bool touching = count && x >= 0 && y >= 0;
        if (touching) {
            lastX = static_cast<uint16_t>((x > kTouchMaxX ? kTouchMaxX : x) * 32767LL / kTouchMaxX);
            lastY = static_cast<uint16_t>((y > kTouchMaxY ? kTouchMaxY : y) * 32767LL / kTouchMaxY);
        } else if (!down) {
            return;
        }
        down = touching;
        Report r = { static_cast<uint8_t>(touching ? 1 : 0), lastX, lastY };
        IOBufferMemoryDescriptor *m = IOBufferMemoryDescriptor::withBytes(&r, sizeof r, kIODirectionNone);
        if (m) {
            handleReport(m);
            m->release();
            uke_touch_pointer_reports = uke_touch_pointer_reports + 1;
        }
    }

public:
    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override {
        IOBufferMemoryDescriptor *m = IOBufferMemoryDescriptor::withBytes(kDescriptor, sizeof kDescriptor, kIODirectionNone);
        if (!m) return kIOReturnNoMemory;
        *out = m;
        return kIOReturnSuccess;
    }
    OSString *newTransportString() const override { return OSString::withCString("Virtual"); }
    OSString *newProductString() const override { return OSString::withCString("uke Touch Pointer"); }
    OSString *newManufacturerString() const override { return OSString::withCString("UniXNU"); }
    OSNumber *newVendorIDNumber() const override { return OSNumber::withNumber(0x5555, 32); }
    OSNumber *newProductIDNumber() const override { return OSNumber::withNumber(0x7a01, 32); }
    OSNumber *newPrimaryUsagePageNumber() const override { return OSNumber::withNumber(0x01, 32); }
    OSNumber *newPrimaryUsageNumber() const override { return OSNumber::withNumber(0x02, 32); }

    bool start(IOService *provider) override {
        if (!IOHIDDevice::start(provider)) return false;
        native_touch_set_sink(sink, this);
        native_touch_probe();
        IOLog("UkeTouchPointer: started\n");
        return true;
    }
    void stop(IOService *provider) override {
        native_touch_set_sink(nullptr, nullptr);
        IOHIDDevice::stop(provider);
    }
};
OSDefineMetaClassAndStructors(UkeTouchPointer, IOHIDDevice)
