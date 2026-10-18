#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace rayneo::core {

// A session's raw HID input reports with their arrival times, to feed the same head motion
// through the whole pipeline (and the renderers) again: before / after comparisons with one
// input. Layout: the 8-byte magic, then per report its host time (u64, little endian, ns), its
// length (u8, <= 65) and its bytes. A truncated last report (recording cut short) is dropped.
inline constexpr uint8_t kImuRecordingMagic[8] = {'N', 'I', 'M', 'B', 'O', 'I', 'M', '1'};

struct ImuRecord {
    uint64_t host_ns = 0;
    uint8_t length = 0;
    uint8_t data[65] = {};
};

void append_imu_recording_header(std::vector<uint8_t>& out);
void append_imu_record(std::vector<uint8_t>& out, uint64_t host_ns, const uint8_t* data, size_t length);
// nullopt: not a recording (magic) or a report longer than 65 bytes.
std::optional<std::vector<ImuRecord>> parse_imu_recording(const uint8_t* bytes, size_t size);

}  // namespace rayneo::core
