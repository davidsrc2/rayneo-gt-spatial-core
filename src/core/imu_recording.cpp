#include "rayneo/core/imu_recording.h"

#include <cstring>

namespace rayneo::core {

void append_imu_recording_header(std::vector<uint8_t>& out) {
    out.insert(out.end(), kImuRecordingMagic, kImuRecordingMagic + sizeof(kImuRecordingMagic));
}

void append_imu_record(std::vector<uint8_t>& out, uint64_t host_ns, const uint8_t* data, size_t length) {
    if (length > sizeof(ImuRecord::data)) length = sizeof(ImuRecord::data);
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(host_ns >> (8 * i)));
    out.push_back(static_cast<uint8_t>(length));
    if (length > 0) out.insert(out.end(), data, data + length);
}

std::optional<std::vector<ImuRecord>> parse_imu_recording(const uint8_t* bytes, size_t size) {
    const size_t magic = sizeof(kImuRecordingMagic);
    if (size < magic || std::memcmp(bytes, kImuRecordingMagic, magic) != 0) return std::nullopt;
    std::vector<ImuRecord> out;
    size_t i = magic;
    while (i + 9 <= size) {
        ImuRecord r;
        for (int k = 0; k < 8; ++k) r.host_ns |= static_cast<uint64_t>(bytes[i + k]) << (8 * k);
        r.length = bytes[i + 8];
        if (r.length > sizeof(r.data)) return std::nullopt;
        if (i + 9 + r.length > size) break;  // cut short
        std::memcpy(r.data, bytes + i + 9, r.length);
        out.push_back(r);
        i += 9 + r.length;
    }
    return out;
}

}  // namespace rayneo::core
