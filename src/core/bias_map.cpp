#include "rayneo/core/bias_map.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rayneo::core {
namespace {

constexpr float kLearnTauS = 30.0f;  // a bin follows the estimate at rest over this long
constexpr float kWeightCapS = 600.0f;
constexpr float kUsableS = 20.0f;    // a bin is trusted after this much rest learned
constexpr int kVersion = 2;          // version 1 learned from a bias that took head motion for drift

}  // namespace

BiasMap::BiasMap() : bins_(static_cast<size_t>(std::lround((kMaxC - kMinC) / kStepC)) + 1) {}

int BiasMap::index(float temp_c) const {
    if (!(temp_c >= kMinC && temp_c <= kMaxC)) return -1;
    return static_cast<int>(std::lround((temp_c - kMinC) / kStepC));
}

void BiasMap::learn(float temp_c, const Vec3& bias_degs, float dt) {
    const int i = index(temp_c);
    if (i < 0 || dt <= 0.0f) return;
    Bin& b = bins_[static_cast<size_t>(i)];
    b.bias = b.weight <= 0.0f ? bias_degs : b.bias + (bias_degs - b.bias) * (dt / (kLearnTauS + dt));
    b.weight = std::min(kWeightCapS, b.weight + dt);
    dirty_ = true;
}

void BiasMap::learn_settled(float temp_c, const Vec3& bias_degs) { learn(temp_c, bias_degs, kUsableS); }

std::optional<Vec3> BiasMap::lookup(float temp_c, float max_gap_c) const {
    const int i = index(temp_c);
    if (i < 0) return std::nullopt;
    const int reach = static_cast<int>(std::lround(max_gap_c / kStepC));
    int lo = -1, hi = -1;
    for (int k = i; k >= std::max(0, i - reach); --k) {
        if (bins_[static_cast<size_t>(k)].weight >= kUsableS) { lo = k; break; }
    }
    for (int k = i; k <= std::min(static_cast<int>(bins_.size()) - 1, i + reach); ++k) {
        if (bins_[static_cast<size_t>(k)].weight >= kUsableS) { hi = k; break; }
    }
    if (lo < 0 && hi < 0) return std::nullopt;
    if (lo < 0) return bins_[static_cast<size_t>(hi)].bias;
    if (hi < 0 || hi == lo) return bins_[static_cast<size_t>(lo)].bias;
    const float t = (temp_c - (kMinC + lo * kStepC)) / ((hi - lo) * kStepC);
    const Vec3& a = bins_[static_cast<size_t>(lo)].bias;
    const Vec3& b = bins_[static_cast<size_t>(hi)].bias;
    return a + (b - a) * std::clamp(t, 0.0f, 1.0f);
}

std::optional<Vec3> BiasMap::trend(float temp_c, float min_span_c, float max_reach_c) const {
    double sw = 0, st = 0, stt = 0, nearest = 1e9, lo = 1e9, hi = -1e9;
    double sb[3] = {0, 0, 0}, stb[3] = {0, 0, 0};
    for (size_t i = 0; i < bins_.size(); ++i) {
        const Bin& b = bins_[i];
        if (b.weight < kUsableS) continue;
        const double t = kMinC + static_cast<double>(i) * kStepC, w = b.weight;
        const double v[3] = {b.bias.x, b.bias.y, b.bias.z};
        sw += w;
        st += w * t;
        stt += w * t * t;
        for (int k = 0; k < 3; ++k) {
            sb[k] += w * v[k];
            stb[k] += w * t * v[k];
        }
        nearest = std::min(nearest, std::fabs(t - temp_c));
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    const double det = sw * stt - st * st;
    if (sw <= 0 || hi - lo < min_span_c || nearest > max_reach_c || std::fabs(det) < 1e-9) return std::nullopt;
    float out[3];
    for (int k = 0; k < 3; ++k) {
        const double slope = (sw * stb[k] - st * sb[k]) / det;
        const double icpt = (sb[k] - slope * st) / sw;
        out[k] = static_cast<float>(icpt + slope * temp_c);
    }
    return Vec3{out[0], out[1], out[2]};
}

bool BiasMap::empty() const {
    return std::none_of(bins_.begin(), bins_.end(), [](const Bin& b) { return b.weight >= kUsableS; });
}

// {"bins":[[temp, x, y, z, weight], ...]}: only the learned bins.
std::string BiasMap::to_json() const {
    std::string out = "{\"version\":" + std::to_string(kVersion) + ",\"bins\":[";
    bool first = true;
    char buf[160];
    for (size_t i = 0; i < bins_.size(); ++i) {
        const Bin& b = bins_[i];
        if (b.weight <= 0.0f) continue;
        std::snprintf(buf, sizeof(buf), "%s[%.1f,%.5f,%.5f,%.5f,%.1f]", first ? "" : ",",
                      kMinC + static_cast<float>(i) * kStepC, b.bias.x, b.bias.y, b.bias.z, b.weight);
        out += buf;
        first = false;
    }
    return out + "]}\n";
}

std::optional<BiasMap> BiasMap::from_json(std::string_view json) {
    BiasMap m;
    const size_t ver = json.find("\"version\":");
    if (ver == std::string_view::npos || std::atoi(std::string(json.substr(ver + 10, 4)).c_str()) != kVersion) return std::nullopt;
    const size_t start = json.find('[');
    if (start == std::string_view::npos) return std::nullopt;
    std::string s(json.substr(start + 1));
    const char* p = s.c_str();
    while (*p != '\0') {
        const char* open = std::strchr(p, '[');
        if (open == nullptr) break;
        float v[5];
        char* end = nullptr;
        const char* q = open + 1;
        bool ok = true;
        for (float& x : v) {
            x = std::strtof(q, &end);
            if (end == q) { ok = false; break; }
            q = end;
            while (*q == ',' || *q == ' ') ++q;
        }
        if (!ok) return std::nullopt;
        const int i = m.index(v[0]);
        if (i >= 0) m.bins_[static_cast<size_t>(i)] = Bin{Vec3{v[1], v[2], v[3]}, std::min(kWeightCapS, v[4])};
        p = q;
    }
    return m;
}

}  // namespace rayneo::core
