#include <hidapi.h>
#if defined(__APPLE__)
#include <hidapi_darwin.h>
#endif

#include <cstdio>
#include <cwchar>
#include <string>

#include "rayneo/hal/hid_transport.h"

namespace rayneo::hal {
namespace {

std::string narrow(const wchar_t* w) {
    if (w == nullptr) return {};
    std::string out;
    for (; *w != L'\0'; ++w) {
        const wchar_t c = *w;
        out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return out;
}

class HidapiTransport final : public HidTransport {
public:
    ~HidapiTransport() override { close(); }

    bool open(const std::string& path) override {
        close();
        dev_ = hid_open_path(path.c_str());
        return dev_ != nullptr;
    }

    void close() override {
        if (dev_ != nullptr) {
            hid_close(dev_);
            dev_ = nullptr;
        }
    }

    bool is_open() const override { return dev_ != nullptr; }

    int write(const uint8_t* data, size_t length) override {
        return dev_ != nullptr ? hid_write(dev_, data, length) : -1;
    }

    int read(uint8_t* data, size_t capacity, int timeout_ms) override {
        return dev_ != nullptr ? hid_read_timeout(dev_, data, capacity, timeout_ms) : -1;
    }

    std::string last_error() const override { return narrow(hid_error(dev_)); }

private:
    hid_device* dev_ = nullptr;
};

}  // namespace

bool hal_init() {
    if (hid_init() != 0) return false;
#if defined(__APPLE__)
    // Shared access: do not seize the device from macOS / other clients.
    hid_darwin_set_open_exclusive(0);
#endif
    return true;
}

void hal_shutdown() { hid_exit(); }

std::vector<HidDeviceInfo> enumerate(uint16_t vendor_id, uint16_t product_id) {
    std::vector<HidDeviceInfo> out;
    hid_device_info* head = hid_enumerate(vendor_id, product_id);
    for (const hid_device_info* d = head; d != nullptr; d = d->next) {
        HidDeviceInfo info;
        info.path = d->path != nullptr ? d->path : "";
        info.vendor_id = d->vendor_id;
        info.product_id = d->product_id;
        info.usage_page = d->usage_page;
        info.usage = d->usage;
        info.interface_number = d->interface_number;
        info.manufacturer = narrow(d->manufacturer_string);
        info.product = narrow(d->product_string);
        info.serial = narrow(d->serial_number);
        out.push_back(std::move(info));
    }
    hid_free_enumeration(head);
    return out;
}

std::unique_ptr<HidTransport> create_transport() { return std::make_unique<HidapiTransport>(); }

std::string describe(const HidDeviceInfo& i) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%04x:%04x if=%d usage=%04x:%04x \"%s\"", i.vendor_id, i.product_id,
                  i.interface_number, i.usage_page, i.usage, i.product.c_str());
    return std::string(buf) + " path=" + i.path;
}

}  // namespace rayneo::hal
