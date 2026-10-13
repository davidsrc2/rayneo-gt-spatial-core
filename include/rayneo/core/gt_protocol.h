#pragma once

// RayNeo GT USB HID wire protocol. Pure byte-level logic: no OS, no hidapi.
// Reference: docs/PROTOCOL.md (derived from Rayneo-Spatial-App, Apache-2.0).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "rayneo/core/types.h"

namespace rayneo::core::gt {

inline constexpr uint16_t kVendorId = 0x3941;
inline constexpr uint16_t kProductId = 0xAF50;
inline constexpr uint16_t kProductIdDfu = 0xAF51;
inline constexpr uint16_t kUsagePage = 0xFF00;

inline constexpr size_t kReportSize = 64;

inline constexpr uint8_t kCmdTag = 0x66;
inline constexpr uint8_t kCmdDeviceInfo = 0x00;
inline constexpr uint8_t kCmdStreamOn = 0x01;
inline constexpr uint8_t kCmdStreamOff = 0x02;

inline constexpr uint8_t kBoardGt = 0x40;
inline constexpr uint8_t kBoardGtMax = 0x41;

inline constexpr double kNominalRateHz = 476.0;

// Output report: [0x00 report id][0x66][cmd][0x00 x 62] -> 65 bytes for hid_write.
using CommandFrame = std::array<uint8_t, kReportSize + 1>;
CommandFrame build_command(uint8_t cmd);

enum class ReportKind { Imu, Ack, Unknown };

struct Report {
    ReportKind kind = ReportKind::Unknown;
    uint8_t board_id = 0;
    uint8_t ack_cmd = 0;
    ImuSample imu;
};

// Accepts reports with or without a leading 0x00 report id (backend dependent).
Report decode_report(const uint8_t* data, size_t length);

// Wrap-safe device tick delta in seconds (tick unit = 100 us).
double tick_delta_seconds(uint32_t prev, uint32_t now);

}  // namespace rayneo::core::gt
