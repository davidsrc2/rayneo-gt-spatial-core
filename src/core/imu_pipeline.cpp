#include "rayneo/core/imu_pipeline.h"

#include <algorithm>
#include <cmath>

#include "rayneo/core/gt_protocol.h"

namespace rayneo::core {
namespace {

constexpr float kDegToRad = 0.017453292519943295f;

// Pure-yaw rotation that takes earth X onto the horizontal projection of `forward`.
Quat heading_of(const Vec3& forward) {
    const float h = std::sqrt(forward.x * forward.x + forward.y * forward.y);
    if (h < 1e-3f) return Quat{};  // looking straight up/down: heading undefined
    return Quat::from_axis_angle({0.0f, 0.0f, 1.0f}, std::atan2(forward.y, forward.x));
}

}  // namespace

ImuPipeline::ImuPipeline(ImuPipelineConfig cfg)
    : cfg_(cfg), filter_(cfg.madgwick_beta), bias_(cfg.bias), stabilizer_(cfg.stabilizer) {}

void ImuPipeline::reset() {
    filter_.reset();
    bias_.reset();
    have_prev_tick_ = false;
    attitude_init_ = false;
    recenter_pending_ = true;
    glide_requested_ = false;
    glide_t_ = 1.0f;
    last_relative_ = Quat{};
    reference_ = Quat{};
    heading_ref_ = Quat{};
    stabilizer_.reset();
}

std::optional<Pose> ImuPipeline::process_report(const uint8_t* data, size_t length) {
    const gt::Report r = gt::decode_report(data, length);
    if (r.kind != gt::ReportKind::Imu) return std::nullopt;
    return process_sample(r.imu);
}

Pose ImuPipeline::process_sample(const ImuSample& s) {
    float dt = 0.0f;
    if (have_prev_tick_) {
        dt = static_cast<float>(gt::tick_delta_seconds(prev_tick_, s.tick_100us));
    }
    prev_tick_ = s.tick_100us;
    have_prev_tick_ = true;

    const Vec3 gyro_corr_degs = bias_.update(s.gyro_degs, s.accel_mps2, dt);

    if (bias_.tracking()) {
        if (!attitude_init_) {
            filter_.initialize_from_accel(s.accel_mps2);
            attitude_init_ = true;
        } else if (dt > 0.0f && dt < cfg_.bias.gap_reset_s) {
            const Vec3 g = gyro_corr_degs * kDegToRad;
            if (cfg_.use_magnetometer) {
                filter_.update_marg(g, s.accel_mps2, s.mag_ut, dt);
            } else {
                filter_.update_imu(g, s.accel_mps2, dt);
            }
        }
    }

    Pose p;
    p.absolute = filter_.quaternion();
    p.tracking = bias_.tracking();
    p.gyro_bias_degs = bias_.bias();
    p.gyro_corrected_degs = gyro_corr_degs;
    p.accel_mps2 = s.accel_mps2;
    p.tick_100us = s.tick_100us;

    if (p.tracking && recenter_pending_) {
        reference_ = p.absolute;
        // Head forward (+X) in sensor coordinates, then in earth coordinates.
        const Vec3 fwd_sensor = sensor_to_head_.conjugate().rotate({1.0f, 0.0f, 0.0f});
        heading_ref_ = heading_of(p.absolute.rotate(fwd_sensor));
        recenter_pending_ = false;
        stabilizer_.reset();  // the stabilizer snaps; the glide below hides the jump
        if (glide_requested_ && cfg_.recenter_glide_s > 0.0f) {
            glide_from_ = last_relative_;  // relative is ~identity now: offset = previous output
            glide_t_ = 0.0f;
        }
        glide_requested_ = false;
    }
    // Earth-frame delta (q_ref^-1 * q would leak yaw into pitch/roll when recentered with a
    // tilted head), conjugated by a pure yaw so its axes read as head forward/left/up.
    const Quat delta = p.absolute * reference_.conjugate();
    p.relative = (heading_ref_.conjugate() * delta * heading_ref_).normalized();
    // d(relative)/dt = 1/2 * w_rel * relative with w_rel = H^-1 * w_earth.
    const Vec3 w_earth = p.absolute.rotate(gyro_corr_degs * kDegToRad);
    p.angular_velocity_rads = p.tracking ? heading_ref_.conjugate().rotate(w_earth) : Vec3{};
    p.relative_raw = p.relative;
    if (cfg_.stabilize && p.tracking) {
        p.relative = stabilizer_.update(p.relative, p.angular_velocity_rads, dt);
        p.angular_velocity_rads = stabilizer_.omega();
    }
    if (glide_t_ < 1.0f && p.tracking) {
        glide_t_ = std::min(1.0f, glide_t_ + dt / cfg_.recenter_glide_s);
        const float k = 1.0f - (1.0f - glide_t_) * (1.0f - glide_t_) * (1.0f - glide_t_);  // ease-out
        p.relative = (slerp(glide_from_, Quat{}, k) * p.relative).normalized();
    }
    last_relative_ = p.relative;
    p.euler = to_euler_deg(p.relative);
    return p;
}

}  // namespace rayneo::core
