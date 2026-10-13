#pragma once

// Hardware abstraction: raw HID byte transport. Nothing above this layer knows
// about hidapi, IOKit or Win32.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rayneo::hal {

struct HidDeviceInfo {
    std::string path;
    uint16_t vendor_id = 0;
    uint16_t product_id = 0;
    uint16_t usage_page = 0;
    uint16_t usage = 0;
    int interface_number = -1;
    std::string manufacturer;
    std::string product;
    std::string serial;
};

class HidTransport {
public:
    virtual ~HidTransport() = default;

    virtual bool open(const std::string& path) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    // Writes one output report. data[0] is the report id (0x00 for unnumbered reports).
    // Returns bytes written or -1.
    virtual int write(const uint8_t* data, size_t length) = 0;

    // Blocking read with timeout. Returns bytes read, 0 on timeout, -1 on error/disconnect.
    virtual int read(uint8_t* data, size_t capacity, int timeout_ms) = 0;

    virtual std::string last_error() const = 0;
};

// Library lifecycle (hid_init / hid_exit). Call once per process.
bool hal_init();
void hal_shutdown();

std::vector<HidDeviceInfo> enumerate(uint16_t vendor_id, uint16_t product_id);
std::unique_ptr<HidTransport> create_transport();

std::string describe(const HidDeviceInfo& info);

}  // namespace rayneo::hal
