#include "rayneo/core/imu_pipeline.h"

#include <algorithm>
#include <cmath>

#include "rayneo/core/gt_protocol.h"
#include "rayneo/core/render_math.h"

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
    : cfg_(cfg), filter_(cfg.madgwick_beta), bias_(cfg.bias), stabilizer_(cfg.stabilizer), vqf_(cfg.vqf),
      convergence_(cfg.bias_convergence), noise_(cfg.gyro_noise), wear_(cfg.wear) {}

void ImuPipeline::reset() {
    filter_.reset();
    vqf_.reset();
    bias_.reset();
    convergence_.reset();
    noise_.reset();
    wear_.reset();
    bias_seeded_ = false;
    bias_settled_ = false;
    have_prev_tick_ = false;
    attitude_init_ = false;
    recenter_pending_ = true;
    glide_requested_ = false;
    glide_t_ = 1.0f;
    pitch_from_ = pitch_to_ = pitch_now_ = 0.0f;
    pitch_pending_ = false;
    last_relative_ = Quat{};
    reference_ = Quat{};
    heading_ref_ = Quat{};
    roll_fix_ = Quat{};
    stabilizer_.reset();
}

ImuPipelineConfig real_size_pipeline_config(ImuPipelineConfig c) {
    c.stabilizer = stationary_lock_stabilizer();
    c.stabilizer.lock_on_rest = true;
    c.use_vqf = true;
    c.body_frame_relative = true;  // exact on both axes whatever the posture at the recenter
    c.level_reference = true;      // the room's vertical is "up": panels on the horizon, level
    c.hold_until_bias_settles = true;  // no start with a rough gyro bias (yaw drift)
    c.recenter_pitch = true;           // the recentre button takes up / down too
    return c;
}

Quat level_head(const Quat& head) {
    const Vec3 f = head.rotate({1.0f, 0.0f, 0.0f});
    const float yaw = std::atan2(f.y, f.x);
    const float pitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, f.z)));
    return (Quat::from_axis_angle({0.0f, 0.0f, 1.0f}, yaw) * Quat::from_axis_angle({0.0f, -1.0f, 0.0f}, pitch)).normalized();
}

Quat heading_only(const Quat& head) {
    const Vec3 f = head.rotate({1.0f, 0.0f, 0.0f});
    if (f.x * f.x + f.y * f.y < 1e-6f) return Quat{};  // straight up / down: no heading
    return Quat::from_axis_angle({0.0f, 0.0f, 1.0f}, std::atan2(f.y, f.x));
}

Quat head_roll(const Quat& head) { return (level_head(head).conjugate() * head).normalized(); }

Quat body_frame_delta(const Quat& reference, const Quat& current, const Quat& sensor_to_head, bool heading) {
    // Head orientations (head -> earth): sensor -> earth composed with head -> sensor.
    const Quat ref = reference * sensor_to_head.conjugate();
    const Quat head_ref = heading ? heading_only(ref) : level_head(ref);
    const Quat head_now = current * sensor_to_head.conjugate();
    return (head_ref.conjugate() * head_now).normalized();
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

    Vec3 gyro_corr_degs = bias_.update(s.gyro_degs, s.accel_mps2, dt);
    wear_.update(s.gyro_degs, dt);

    if (cfg_.use_vqf && bias_.tracking()) {
        if (!attitude_init_) {
            vqf_.initialize_from_accel(s.accel_mps2);
            // Cold start: the bias learned at this temperature on earlier runs, else the startup
            // calibration's.
            const auto mapped = bias_map_.lookup(s.temp_c, cfg_.bias_map_seed_gap_c);
            const auto trend = mapped ? std::nullopt : bias_map_.trend(s.temp_c);
            if (mapped) {
                vqf_.set_bias(*mapped, 0.05f);  // learned here: trusted
            } else if (trend) {
                vqf_.set_bias(*trend, cfg_.bias_trend_std_degs);
            } else {
                vqf_.set_bias(bias_.bias(), 0.0f);
            }
            bias_seeded_ = mapped.has_value();
            bias_settled_ = false;
            convergence_.reset();
            attitude_init_ = true;
            attitude_s_ = 0.0f;
        } else if (dt > 0.0f && dt < cfg_.bias.gap_reset_s) {
            vqf_.set_temperature(s.temp_c);
            vqf_.update(s.gyro_degs * kDegToRad, s.accel_mps2, dt);
            noise_.update(s.gyro_degs, vqf_.rest(), dt);
            attitude_s_ += dt;
            if (!bias_settled_) {
                convergence_.update(vqf_.bias_degs(), s.temp_c, vqf_.rest(), dt);
                if (convergence_.converged()) {
                    bias_settled_ = true;
                    // Remembered as trusted: the next start at this temperature is seeded with it.
                    if (!bias_seeded_) bias_map_.learn_settled(s.temp_c, vqf_.bias_degs());
                }
            }
            if (vqf_.rest() && vqf_.settled()) bias_map_.learn(s.temp_c, vqf_.bias_degs(), dt);
        }
        gyro_corr_degs = s.gyro_degs - vqf_.bias_degs();
        stabilizer_.set_rest(vqf_.rest());
    } else if (bias_.tracking()) {
        if (!attitude_init_) {
            filter_.initialize_from_accel(s.accel_mps2);
            attitude_init_ = true;
            attitude_s_ = 0.0f;
        } else if (dt > 0.0f && dt < cfg_.bias.gap_reset_s) {
            attitude_s_ += dt;
            if (cfg_.startup_beta > cfg_.madgwick_beta) {
                const float span = std::max(1e-3f, cfg_.startup_beta_end_s - cfg_.startup_beta_hold_s);
                const float k = std::clamp((attitude_s_ - cfg_.startup_beta_hold_s) / span, 0.0f, 1.0f);
                filter_.set_beta(cfg_.startup_beta + (cfg_.madgwick_beta - cfg_.startup_beta) * k);
            }
            const Vec3 g = gyro_corr_degs * kDegToRad;
            if (cfg_.use_magnetometer) {
                filter_.update_marg(g, s.accel_mps2, s.mag_ut, dt);
            } else {
                filter_.update_imu(g, s.accel_mps2, dt);
            }
        }
    }

    Pose p;
    p.absolute = cfg_.use_vqf ? vqf_.quaternion() : filter_.quaternion();
    p.tracking = bias_.tracking();
    p.gyro_noise_degs = noise_.noise_degs();
    p.gyro_noisy = noise_.noisy();
    p.temp_c = s.temp_c;
    p.worn = wear_.worn();
    if (cfg_.use_vqf && cfg_.hold_until_bias_settles && p.tracking) {
        p.tracking = attitude_init_ && (bias_seeded_ || bias_settled_ || attitude_s_ >= cfg_.bias_settle_max_s);
    }
    p.gyro_bias_degs = cfg_.use_vqf && attitude_init_ ? vqf_.bias_degs() : bias_.bias();
    p.gyro_corrected_degs = gyro_corr_degs;
    p.accel_mps2 = s.accel_mps2;
    p.tick_100us = s.tick_100us;

    if (p.tracking && recenter_pending_) {
        reference_ = p.absolute;
        // Head forward (+X) in sensor coordinates, then in earth coordinates.
        const Vec3 fwd_sensor = sensor_to_head_.conjugate().rotate({1.0f, 0.0f, 0.0f});
        heading_ref_ = heading_of(p.absolute.rotate(fwd_sensor));
        roll_fix_ = cfg_.level_reference ? head_roll(p.absolute * sensor_to_head_.conjugate()) : Quat{};
        recenter_pending_ = false;
        stabilizer_.reset();  // the stabilizer snaps; the glide below hides the jump
        if (glide_requested_ && cfg_.recenter_glide_s > 0.0f) {
            glide_from_ = last_relative_;  // relative is ~identity now: offset = previous output
            glide_t_ = 0.0f;
        }
        if (glide_requested_ && cfg_.recenter_pitch && cfg_.level_reference) {
            pitch_from_ = pitch_now_;
            pitch_pending_ = true;  // the head's elevation: the relative pose below, levelled
        }
        glide_requested_ = false;
    }
    // Earth-frame delta (q_ref^-1 * q would leak yaw into pitch/roll when recentered with a
    // tilted head), conjugated by a pure yaw so its axes read as head forward/left/up.
    if (cfg_.body_frame_relative) {
        // The head's frame turned by the roll it had at the recenter (right-multiplied: head axes).
        p.relative = (body_frame_delta(reference_, p.absolute, sensor_to_head_, cfg_.level_reference) *
                      roll_fix_.conjugate()).normalized();
        // d(relative)/dt = 1/2 * relative * w_head = 1/2 * (relative w_head relative^-1) * relative,
        // w_head seen in the roll-fixed axes.
        const Vec3 w_head = roll_fix_.rotate(sensor_to_head_.rotate(gyro_corr_degs * kDegToRad));
        p.angular_velocity_rads = p.tracking ? p.relative.rotate(w_head) : Vec3{};
    } else {
        const Quat delta = p.absolute * reference_.conjugate();
        p.relative = (heading_ref_.conjugate() * delta * heading_ref_).normalized();
        // d(relative)/dt = 1/2 * w_rel * relative with w_rel = H^-1 * w_earth.
        const Vec3 w_earth = p.absolute.rotate(gyro_corr_degs * kDegToRad);
        p.angular_velocity_rads = p.tracking ? heading_ref_.conjugate().rotate(w_earth) : Vec3{};
    }
    p.relative_raw = p.relative;
    if (pitch_pending_) {
        // Right after a level recentre the relative pose is the head's pitch (and the roll taken
        // as upright): its gaze elevation is where to lift the panels.
        pitch_to_ = std::clamp(gaze_placement(p.relative).pitch_rad, -cfg_.max_recenter_pitch_rad,
                               cfg_.max_recenter_pitch_rad);
        pitch_pending_ = false;
        if (glide_t_ >= 1.0f) pitch_now_ = pitch_to_;
    }
    if (cfg_.stabilize && p.tracking) {
        p.relative = stabilizer_.update(p.relative, p.angular_velocity_rads, dt);
        p.angular_velocity_rads = stabilizer_.omega();
        // Stationary lock: a head held still for 150 ms refines the gyro bias in the background
        // (no yaw drift: the magnetometer is off and yaw is the gyro alone).
        if (!cfg_.use_vqf && cfg_.stabilizer.stationary_lock && stabilizer_.locked_seconds() >= 0.15f) {
            bias_.adapt_locked(s.gyro_degs, dt);
        }
    }
    if (glide_t_ < 1.0f && p.tracking) {
        glide_t_ = std::min(1.0f, glide_t_ + dt / cfg_.recenter_glide_s);
        const float k = 1.0f - (1.0f - glide_t_) * (1.0f - glide_t_) * (1.0f - glide_t_);  // ease-out
        p.relative = (slerp(glide_from_, Quat{}, k) * p.relative).normalized();
        pitch_now_ = pitch_from_ + (pitch_to_ - pitch_from_) * k;
    } else {
        pitch_now_ = pitch_to_;
    }
    p.panels_pitch_rad = pitch_now_;
    last_relative_ = p.relative;
    p.euler = to_euler_deg(p.relative);
    return p;
}

}  // namespace rayneo::core
