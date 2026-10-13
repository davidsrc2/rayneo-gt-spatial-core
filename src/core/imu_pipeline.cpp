#include "rayneo/core/imu_pipeline.h"

#include "rayneo/core/gt_protocol.h"

namespace rayneo::core {
namespace {

constexpr float kDegToRad = 0.017453292519943295f;

}  // namespace

ImuPipeline::ImuPipeline(ImuPipelineConfig cfg)
    : cfg_(cfg), filter_(cfg.madgwick_beta), bias_(cfg.bias) {}

void ImuPipeline::reset() {
    filter_.reset();
    bias_.reset();
    have_prev_tick_ = false;
    attitude_init_ = false;
    recenter_pending_ = true;
    reference_ = Quat{};
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
    p.tick_100us = s.tick_100us;

    if (p.tracking && recenter_pending_) {
        reference_ = p.absolute;
        recenter_pending_ = false;
    }
    // Earth-frame relative rotation. q_ref^-1 * q would leak yaw into pitch/roll when
    // recentered with a tilted head.
    p.relative = (p.absolute * reference_.conjugate()).normalized();
    p.euler = to_euler_deg(p.relative);
    return p;
}

}  // namespace rayneo::core
