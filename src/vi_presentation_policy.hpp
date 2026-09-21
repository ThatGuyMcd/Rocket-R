#pragma once

#include <cstdint>

namespace rocket::renderer::presentation {

inline constexpr std::uint32_t kCanonicalViWidth = 320U;
inline constexpr std::uint32_t kCanonicalViHeight = 240U;

constexpr std::uint32_t vi_region_start(std::uint32_t region) {
    return (region >> 16U) & 0x3FFU;
}

constexpr std::uint32_t vi_region_end(std::uint32_t region) {
    return region & 0x3FFU;
}

constexpr std::uint32_t round_positive(double value) {
    return static_cast<std::uint32_t>(value + 0.5);
}

// Mirrors the progressive-height estimate used by the pinned RT64 VI path.
// RT64 adds two guard rows and rounds to a multiple of four. Rocket authors a
// canonical 320x240 image, so a 244/248 inference would expose unused rows at
// presentation time after the game's intentional safe-area crop is removed.
constexpr std::uint32_t inferred_vi_height(std::uint32_t v_region,
                                           std::uint32_t y_scale) {
    const std::uint32_t start = vi_region_start(v_region);
    const std::uint32_t end = vi_region_end(v_region);
    y_scale &= 0xFFFU;
    if (end <= start || y_scale == 0U) {
        return 0U;
    }

    const double sampled_rows =
        (double(end - start) * double(y_scale)) / 2048.0;
    const std::uint32_t with_guard_rows = round_positive(sampled_rows) + 2U;
    return round_positive(double(with_guard_rows) / 4.0) * 4U;
}

constexpr std::uint32_t canonicalise_rocket_v_region(std::uint32_t v_region,
                                                      std::uint32_t y_scale) {
    const std::uint32_t height = inferred_vi_height(v_region, y_scale);
    if (height <= kCanonicalViHeight || height > 248U) {
        return v_region;
    }

    const std::uint32_t start = vi_region_start(v_region);
    const std::uint32_t end = vi_region_end(v_region);
    for (std::uint32_t trim = 1U; trim <= 16U && end > start + trim; ++trim) {
        const std::uint32_t candidate =
            (v_region & 0xFFFFFC00U) | ((end - trim) & 0x3FFU);
        if (inferred_vi_height(candidate, y_scale) == kCanonicalViHeight) {
            return candidate;
        }
    }

    return v_region;
}

// Known progressive NTSC shape used by Rocket. Keep this constexpr regression
// here so a future edit cannot silently re-introduce RT64's 244-row guard area.
static_assert(inferred_vi_height((37U << 16U) | 517U, 1024U) == 244U);
static_assert(inferred_vi_height(
                  canonicalise_rocket_v_region((37U << 16U) | 517U, 1024U),
                  1024U) == kCanonicalViHeight);

} // namespace rocket::renderer::presentation
