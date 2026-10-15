#include "rayneo/core/settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rayneo::core {

namespace {

// Just enough JSON for a flat settings object: numbers, booleans, strings (skipped) and the
// array of {col, row} cells. Anything unexpected is skipped, never fatal.
class Reader {
public:
    explicit Reader(std::string_view s) : s_(s) {}

    void ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }
    bool eat(char c) {
        ws();
        if (i_ < s_.size() && s_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }
    bool peek(char c) {
        ws();
        return i_ < s_.size() && s_[i_] == c;
    }
    bool done() const { return i_ >= s_.size(); }

    std::optional<std::string> string() {
        if (!eat('"')) return std::nullopt;
        std::string out;
        while (i_ < s_.size() && s_[i_] != '"') {
            if (s_[i_] == '\\' && i_ + 1 < s_.size()) ++i_;  // escapes: keep the escaped char
            out += s_[i_++];
        }
        if (i_ >= s_.size()) return std::nullopt;
        ++i_;
        return out;
    }

    std::optional<double> number() {
        ws();
        const size_t start = i_;
        while (i_ < s_.size() && std::strchr("+-.0123456789eE", s_[i_]) != nullptr) ++i_;
        if (i_ == start) return std::nullopt;
        const std::string text(s_.substr(start, i_ - start));
        char* end = nullptr;
        const double v = std::strtod(text.c_str(), &end);
        if (end != text.c_str() + text.size()) return std::nullopt;
        return v;
    }

    std::optional<bool> boolean() {
        ws();
        if (s_.substr(i_, 4) == "true") {
            i_ += 4;
            return true;
        }
        if (s_.substr(i_, 5) == "false") {
            i_ += 5;
            return false;
        }
        return std::nullopt;
    }

    // Skips one value of any kind.
    void skip() {
        ws();
        if (i_ >= s_.size()) return;
        const char c = s_[i_];
        if (c == '"') {
            string();
        } else if (c == '{' || c == '[') {
            const char close = c == '{' ? '}' : ']';
            ++i_;
            while (!done() && !eat(close)) {
                if (c == '{') {
                    string();
                    eat(':');
                }
                skip();
                eat(',');
            }
        } else {
            while (i_ < s_.size() && std::strchr(",}] \t\r\n", s_[i_]) == nullptr) ++i_;
        }
    }

private:
    std::string_view s_;
    size_t i_ = 0;
};

std::optional<std::vector<GridCell>> read_cells(Reader& r) {
    if (!r.eat('[')) return std::nullopt;
    std::vector<GridCell> cells;
    while (!r.done() && !r.eat(']')) {
        if (!r.eat('{')) return std::nullopt;
        GridCell c;
        bool has_col = false, has_row = false;
        while (!r.done() && !r.eat('}')) {
            const auto key = r.string();
            if (!key || !r.eat(':')) return std::nullopt;
            const auto v = r.number();
            if (*key == "col" && v) {
                c.col = static_cast<int>(*v);
                has_col = true;
            } else if (*key == "row" && v) {
                c.row = static_cast<int>(*v);
                has_row = true;
            } else if (!v) {
                r.skip();
            }
            r.eat(',');
        }
        if (!has_col || !has_row) return std::nullopt;
        cells.push_back(c);
        r.eat(',');
    }
    if (cells.empty() || cells.size() > 8) return std::nullopt;
    return cells;
}

void put(std::string& out, const char* key, float v, bool last = false) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "  \"%s\": %g%s\n", key, static_cast<double>(v), last ? "" : ",");
    out += buf;
}

void put(std::string& out, const char* key, bool v, bool last = false) {
    out += std::string("  \"") + key + "\": " + (v ? "true" : "false") + (last ? "\n" : ",\n");
}

}  // namespace

std::string settings_to_json(const Settings& s) {
    std::string out = "{\n  \"layoutCells\": [";
    for (size_t i = 0; i < s.cells.size(); ++i) {
        out += (i > 0 ? ", " : "") + std::string("{\"col\": ") + std::to_string(s.cells[i].col) +
               ", \"row\": " + std::to_string(s.cells[i].row) + "}";
    }
    out += "],\n  \"textSize\": " + std::to_string(s.text_size) + ",\n";
    put(out, "distance", s.distance_m);
    put(out, "gap", s.gap_deg);
    put(out, "curvature", s.curvature);
    put(out, "size", s.size_deg);
    put(out, "autoStart", s.auto_start);
    put(out, "fovY", s.fov_y_deg);
    put(out, "ipd", s.ipd_mm);
    put(out, "stereo", s.stereo);
    put(out, "deadZone", s.dead_zone_deg);
    put(out, "gazeCursor", s.gaze_cursor);
    put(out, "dimOthers", s.dim_others);
    put(out, "gatherWindows", s.gather_windows);
    out += "  \"tracking\": " + std::to_string(s.tracking) + ",\n";
    put(out, "k1", s.k1);
    put(out, "k2", s.k2);
    put(out, "onboardingDone", s.onboarding_done, true);
    return out + "}\n";
}

std::optional<Settings> settings_from_json(std::string_view json) {
    Reader r(json);
    if (!r.eat('{')) return std::nullopt;
    Settings s;
    const struct {
        const char* key;
        float* value;
    } floats[] = {{"distance", &s.distance_m}, {"gap", &s.gap_deg},     {"curvature", &s.curvature},
                  {"size", &s.size_deg},       {"fovY", &s.fov_y_deg},  {"ipd", &s.ipd_mm},
                  {"deadZone", &s.dead_zone_deg}, {"k1", &s.k1},        {"k2", &s.k2}};
    const struct {
        const char* key;
        bool* value;
    } bools[] = {{"autoStart", &s.auto_start},       {"stereo", &s.stereo},
                 {"gazeCursor", &s.gaze_cursor},     {"dimOthers", &s.dim_others},
                 {"gatherWindows", &s.gather_windows}, {"onboardingDone", &s.onboarding_done}};
    bool has_tracking = false, legacy_yaw_only = true, has_legacy = false;

    while (!r.done() && !r.eat('}')) {
        const auto key = r.string();
        if (!key || !r.eat(':')) return std::nullopt;
        bool handled = false;
        if (*key == "layoutCells") {
            if (const auto cells = read_cells(r)) s.cells = *cells;
            handled = true;  // malformed: reading stops below, keeping the keys before it
        } else if (*key == "tracking") {
            if (const auto v = r.number()) {
                s.tracking = std::max(0, std::min(2, static_cast<int>(*v)));
                has_tracking = handled = true;
            }
        } else if (*key == "yawOnly") {  // earlier builds: a switch for left/right only
            if (const auto v = r.boolean()) {
                legacy_yaw_only = *v;
                has_legacy = handled = true;
            }
        } else if (*key == "textSize") {
            if (const auto v = r.number()) {
                s.text_size = static_cast<int>(*v);
                handled = true;
            }
        } else {
            for (const auto& f : floats) {
                if (*key == f.key) {
                    if (const auto v = r.number()) {
                        *f.value = static_cast<float>(*v);
                        handled = true;
                    }
                }
            }
            for (const auto& b : bools) {
                if (*key == b.key) {
                    if (const auto v = r.boolean()) {
                        *b.value = *v;
                        handled = true;
                    }
                }
            }
        }
        if (!handled) r.skip();
        if (!r.eat(',') && !r.peek('}')) break;  // garbage: keep what was read
    }
    if (!has_tracking && has_legacy) s.tracking = legacy_yaw_only ? 1 : 0;
    return s;
}

}  // namespace rayneo::core
