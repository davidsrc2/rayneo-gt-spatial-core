// replay_eval: how well the head pose is predicted, offline, on an IMU recording
// (--record-imu). The recording runs through the core pipeline as the space at real size
// runs it; frames at the display rate read the latest pose `latch` ms before their deadline and
// reach the eyes `photon` ms after that read (both measured in the TRACK line: latch applied,
// sample->presented minus pose age). The pose the pipeline gives at the photon time is the
// truth: the error is the angle between it and the frame's predicted pose. Also the TRACK line's
// moving jitter (second difference of the drawn pose) and still wobble.
//
//   replay_eval REC.imu [--calib calibration.json] [--hz 120] [--latch 1.0] [--photon 17.4]
//               [--horizons 0,4,8,10,12,14,16] [--map biasmap.json]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "rayneo/core/head_calibration.h"
#include "rayneo/core/imu_pipeline.h"
#include "rayneo/core/imu_recording.h"
#include "rayneo/core/render_math.h"

using namespace rayneo::core;

namespace {

constexpr double kRadToDeg = 57.29577951308232;

struct Sample {
    uint64_t t;
    Quat q;
    Vec3 w;
    bool tracking;
};

std::string read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

double angle_deg(const Quat& a, const Quat& b) {
    const Quat r = (a.conjugate() * b).normalized();
    const double v = std::sqrt(double(r.x) * r.x + double(r.y) * r.y + double(r.z) * r.z);
    return 2.0 * std::asin(std::fmin(1.0, v)) * kRadToDeg;
}

// The pipeline's pose at time t (slerp between the samples around it).
Quat pose_at(const std::vector<Sample>& s, size_t& k, uint64_t t) {
    while (k + 1 < s.size() && s[k + 1].t <= t) ++k;
    if (k + 1 >= s.size() || s[k].t >= t) return s[k].q;
    const float u = static_cast<float>(double(t - s[k].t) / double(s[k + 1].t - s[k].t));
    return slerp(s[k].q, s[k + 1].q, u);
}

struct Result {
    double err_moving_sq = 0, err_still_sq = 0, err_moving_max = 0;
    int n_moving = 0, n_still = 0;
    double jitter_sq = 0;
    int jitter_n = 0;
    std::vector<double> errs;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: replay_eval REC.imu [--calib FILE] [--hz HZ] [--latch MS] [--photon MS] "
                             "[--horizons a,b,..] [--map FILE] [--accel]\n");
        return 2;
    }
    const char* calib = nullptr;
    const char* map_path = nullptr;
    double hz = 120, latch_ms = 1.0, photon_ms = 17.4;
    std::vector<float> horizons = {0, 4, 6, 8, 10, 12, 14, 16};
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--calib") calib = v, ++i;
        else if (a == "--map") map_path = v, ++i;
        else if (a == "--hz") hz = std::atof(v), ++i;
        else if (a == "--latch") latch_ms = std::atof(v), ++i;
        else if (a == "--photon") photon_ms = std::atof(v), ++i;
        else if (a == "--horizons") {
            horizons.clear();
            for (const char* p = v; *p;) {
                horizons.push_back(std::strtof(p, const_cast<char**>(&p)));
                if (*p == ',') ++p;
            }
            ++i;
        }
    }
    const std::string bytes = read_file(argv[1]);
    const auto rec = parse_imu_recording(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    if (!rec || rec->empty()) {
        std::fprintf(stderr, "not an IMU recording: %s\n", argv[1]);
        return 1;
    }
    ImuPipeline pipeline(real_size_pipeline_config());
    if (calib) {
        if (const auto q = calibration_from_json(read_file(calib))) pipeline.set_sensor_to_head(*q);
    }
    if (map_path) {
        if (auto m = BiasMap::from_json(read_file(map_path))) pipeline.set_bias_map(*m);
    }
    std::vector<Sample> s;
    std::vector<uint64_t> recentres;
    s.reserve(rec->size());
    for (const auto& r : *rec) {
        if (r.length == 0) {  // a user recentre in the session
            pipeline.recenter(true);
            recentres.push_back(r.host_ns);
            continue;
        }
        if (const auto p = pipeline.process_report(r.data, r.length)) {
            s.push_back({r.host_ns, p->relative, p->angular_velocity_rads, p->tracking});
        }
    }
    const uint64_t period = static_cast<uint64_t>(1e9 / hz);
    const uint64_t latch = static_cast<uint64_t>(latch_ms * 1e6), photon = static_cast<uint64_t>(photon_ms * 1e6);
    std::printf("%s: %zu samples, %.0f Hz frames, pose read %.1f ms before the deadline, photons %.1f ms after the read\n",
                argv[1], s.size(), hz, latch_ms, photon_ms);
    std::printf("%-26s %10s %10s %10s %10s %12s\n", "model / horizon (ms)", "err mov", "err p95", "err max", "err still",
                "jitter");
    for (int model = 0; model < 2; ++model) {
        for (const float h : horizons) {
            Result res;
            size_t k = 0, kt = 0;
            Quat last_drawn;
            Vec3 last_step, prev_w;
            uint64_t prev_t = 0;
            bool have_drawn = false, have_step = false;
            for (uint64_t deadline = s.front().t + period; deadline + photon < s.back().t; deadline += period) {
                const uint64_t read = deadline - latch;
                while (k + 1 < s.size() && s[k + 1].t <= read) ++k;
                const Sample& x = s[k];
                if (!x.tracking) continue;
                // A recentre jumps the truth: frames from before it to the end of its glide left out.
                bool near_recentre = false;
                for (const uint64_t rc : recentres) near_recentre |= read + photon >= rc && read < rc + 600000000ull;
                if (near_recentre) {
                    have_drawn = have_step = false;
                    continue;
                }
                const float age_ms = static_cast<float>(double(read - x.t) / 1e6);
                const float horizon_s = fixed_prediction_horizon_ms(h, 0.0f, 1000.0f / static_cast<float>(hz), age_ms) * 1e-3f;
                Vec3 w = without_roll(x.q, x.w);  // as the renderer (PredXY)
                if (model == 1 && prev_t != 0 && x.t > prev_t) {
                    // Constant angular acceleration over the horizon: w + a h / 2, a from the
                    // last frame's change.
                    const float dt = static_cast<float>(double(x.t - prev_t) / 1e9);
                    const Vec3 a = (x.w - prev_w) * (1.0f / dt);
                    w = without_roll(x.q, x.w + a * (0.5f * horizon_s));
                }
                prev_w = x.w;
                prev_t = x.t;
                const Quat drawn = predict_orientation(x.q, w, h > 0 ? horizon_s : 0.0f);
                const Quat truth = pose_at(s, kt, read + photon);
                const double e = angle_deg(drawn, truth);
                const bool still = x.w.norm() < 0.0175f;
                if (still) {
                    res.err_still_sq += e * e;
                    ++res.n_still;
                } else {
                    res.err_moving_sq += e * e;
                    ++res.n_moving;
                    res.err_moving_max = std::fmax(res.err_moving_max, e);
                    res.errs.push_back(e);
                }
                if (have_drawn) {
                    Quat r = last_drawn.conjugate() * drawn;
                    if (r.w < 0) r = Quat{-r.w, -r.x, -r.y, -r.z};
                    const Vec3 step = Vec3{r.x, r.y, r.z} * static_cast<float>(2 * kRadToDeg);
                    if (!still && have_step) {
                        const double j = (step - last_step).norm();
                        if (j < 2) {
                            res.jitter_sq += j * j;
                            ++res.jitter_n;
                        }
                    }
                    last_step = step;
                    have_step = true;
                }
                last_drawn = drawn;
                have_drawn = true;
            }
            std::sort(res.errs.begin(), res.errs.end());
            const double p95 = res.errs.empty() ? 0 : res.errs[static_cast<size_t>(0.95 * (res.errs.size() - 1))];
            char name[32];
            std::snprintf(name, sizeof(name), "%s %.0f", model == 0 ? "constant w" : "constant accel", h);
            std::printf("%-26s %10.3f %10.3f %10.3f %10.4f %12.3f\n", name,
                        std::sqrt(res.err_moving_sq / std::max(1, res.n_moving)), p95, res.err_moving_max,
                        std::sqrt(res.err_still_sq / std::max(1, res.n_still)),
                        std::sqrt(res.jitter_sq / std::max(1, res.jitter_n)));
        }
    }
    return 0;
}
