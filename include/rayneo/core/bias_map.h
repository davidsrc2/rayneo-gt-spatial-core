#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rayneo/core/types.h"

namespace rayneo::core {

// The gyro bias learned per sensor temperature. The GT's gyro bias shifts while the glasses
// warm up (the first minutes, under ~30 C): starting cold, the bias for the current temperature
// comes from earlier runs instead of a fresh rough estimate; whenever the head rests, the
// estimate refines the entry for the temperature now. 0.5 C bins from 10 to 60 C.
class BiasMap {
public:
    static constexpr float kMinC = 10.0f, kMaxC = 60.0f, kStepC = 0.5f;

    BiasMap();
    // The bias at rest (deg/s) at this temperature, blended in over learn_tau_s.
    void learn(float temp_c, const Vec3& bias_degs, float dt);
    // Interpolated from the learned bins within max_gap_c of temp_c; nullopt when none.
    std::optional<Vec3> lookup(float temp_c, float max_gap_c = 3.0f) const;
    bool empty() const;
    bool dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = false; }

    std::string to_json() const;
    static std::optional<BiasMap> from_json(std::string_view json);

private:
    struct Bin {
        Vec3 bias;
        float weight = 0.0f;  // seconds of rest learned, capped
    };
    std::vector<Bin> bins_;
    bool dirty_ = false;
    int index(float temp_c) const;
};

}  // namespace rayneo::core
