#pragma once

#include <algorithm>
#include <cmath>

namespace rocket::presentation {
inline float sky_pitch_displacement(float original, float authored_fov, float rendered_fov) {
    if (!std::isfinite(original) || !std::isfinite(authored_fov) || !std::isfinite(rendered_fov) ||
        authored_fov <= .05F || rendered_fov <= .05F || rendered_fov >= 3.09F) return original;
    const float pitch = original * authored_fov / 240.0F;
    // Outside this angle the horizon is well off screen; avoid the tangent pole.
    // D_800A5CB0 has a vertical viewport scale of 430 quarter-pixels.
    return 107.5F * std::tan(std::clamp(pitch, -1.4F, 1.4F)) / std::tan(rendered_fov * .5F);
}

struct SkyRows {
    int load = 0;
    int origin = 0;
    float current = 0;
    float previous = 0;
};

// The retail rectangle samples 212 rows, from row 211 down to row 0.
// Keep both presentation endpoints inside the loaded texture, with room for
// filtering. A turn too large for TMEM starts a fresh presentation endpoint.
inline SkyRows sky_rows(int original, float requested, float previous,
                        int image_height, int loaded_rows, bool continuous) {
    SkyRows rows;
    rows.origin = original;
    const float maximum = float(std::max(image_height - 241, 0));
    rows.current = std::isfinite(requested) ? std::clamp(requested, 0.0F, maximum) : float(original);
    rows.previous = continuous && std::isfinite(previous) ? std::clamp(previous, 0.0F, maximum) : rows.current;
    const int spare = std::max(loaded_rows - 212, 0);
    if (std::abs(rows.current - rows.previous) > std::max(spare - 4, 0)) rows.previous = rows.current;
    const int before = int(std::floor(std::min(rows.current, rows.previous)));
    const int after = int(std::ceil(std::max(rows.current, rows.previous))) + 212;
    const int lower = std::max(after - loaded_rows, 0);
    const int upper = std::max(std::min(before, image_height - loaded_rows), lower);
    rows.load = std::clamp(before - 2, lower, upper);
    return rows;
}
}
