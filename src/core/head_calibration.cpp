#include "rayneo/core/head_calibration.h"

#include <cmath>
#include <cstdio>
#include <locale>
#include <sstream>

namespace rayneo::core {
namespace {

constexpr float kRadToDeg = 57.29577951308232f;

float angle_deg(const Vec3& a, const Vec3& b) {
    const float c = a.normalized().dot(b.normalized());
    return std::acos(std::fmax(-1.0f, std::fmin(1.0f, c))) * kRadToDeg;
}

// Value of `"key": <number>` inside `obj`, locale-independent.
std::optional<float> number_after(std::string_view obj, std::string_view key) {
    const std::string quoted = "\"" + std::string(key) + "\"";
    const size_t k = obj.find(quoted);
    if (k == std::string_view::npos) return std::nullopt;
    const size_t colon = obj.find(':', k + quoted.size());
    if (colon == std::string_view::npos) return std::nullopt;
    std::istringstream is(std::string(obj.substr(colon + 1)));
    is.imbue(std::locale::classic());
    float v;
    if (!(is >> v) || !std::isfinite(v)) return std::nullopt;
    return v;
}

}  // namespace

HeadCalibrationResult solve_head_calibration(const HeadCalibrationInput& in, const HeadCalibrationLimits& lim) {
    HeadCalibrationResult r;
    if (in.gravity_center.norm() < 5.0f) {
        r.error = "no gravity reading at center";
        return r;
    }
    for (const Vec3* v : {&in.rot_up, &in.rot_down, &in.rot_left, &in.rot_right}) {
        if (v->norm() < lim.min_rotation_deg) {
            r.error = "movement too small: look further in each direction";
            return r;
        }
    }
    if (in.rot_up.dot(in.rot_down) >= 0.0f || in.rot_left.dot(in.rot_right) >= 0.0f) {
        r.error = "opposite directions did not rotate in opposite senses";
        return r;
    }

    const Vec3 up = in.gravity_center.normalized();
    // Looking up rotates about -left, looking down about +left.
    const Vec3 left_raw = in.rot_down - in.rot_up;
    // Turning left rotates about +up.
    const Vec3 yaw_axis = in.rot_left - in.rot_right;

    r.yaw_axis_error_deg = angle_deg(yaw_axis, up);
    r.pitch_axis_tilt_deg = std::fabs(90.0f - angle_deg(left_raw, up));
    if (r.yaw_axis_error_deg > lim.max_yaw_axis_error_deg) {
        r.error = "left/right turn axis is not vertical: keep the head level and turn only";
        return r;
    }
    if (r.pitch_axis_tilt_deg > lim.max_pitch_axis_tilt_deg) {
        r.error = "up/down nod axis is not horizontal: nod without turning or tilting";
        return r;
    }

    const Vec3 left = (left_raw - up * left_raw.dot(up)).normalized();
    const Vec3 forward = left.cross(up);
    r.sensor_to_head = Quat::from_rows(forward, left, up);
    r.ok = true;
    return r;
}

std::string calibration_to_json(const Quat& q) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "{\n  \"version\": 1,\n  \"sensor_to_head\": {\"w\": %.7f, \"x\": %.7f, \"y\": %.7f, \"z\": %.7f}\n}\n",
                  static_cast<double>(q.w), static_cast<double>(q.x), static_cast<double>(q.y),
                  static_cast<double>(q.z));
    return buf;
}

std::optional<Quat> calibration_from_json(std::string_view json) {
    const size_t k = json.find("\"sensor_to_head\"");
    if (k == std::string_view::npos) return std::nullopt;
    const size_t open = json.find('{', k);
    const size_t close = json.find('}', open);
    if (open == std::string_view::npos || close == std::string_view::npos) return std::nullopt;
    const std::string_view obj = json.substr(open, close - open + 1);

    const auto w = number_after(obj, "w"), x = number_after(obj, "x");
    const auto y = number_after(obj, "y"), z = number_after(obj, "z");
    if (!w || !x || !y || !z) return std::nullopt;
    const Quat q{*w, *x, *y, *z};
    const float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (std::fabs(n - 1.0f) > 0.01f) return std::nullopt;
    return q.normalized();
}

}  // namespace rayneo::core
