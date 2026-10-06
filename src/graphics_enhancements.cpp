#include "graphics_enhancements.hpp"
#include "presentation_identity.hpp"

#include "recomp.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>

namespace {

using rocket::graphics::Settings;

std::mutex g_settings_mutex;
Settings g_settings{};
std::filesystem::path g_config_directory;
std::atomic<std::uint64_t> g_revision{1U};
std::atomic<float> g_window_aspect{16.0F / 10.0F};
std::atomic<bool> g_cutscene_active{false};
std::mutex g_performance_mutex;
rocket::graphics::PerformanceStats g_performance{};

constexpr std::uint32_t kRdramStart = 0x80000000U;
constexpr std::uint32_t kRdramEnd = 0x80800000U;
constexpr std::uint32_t kCameraBytes = 0xB0U;
constexpr int kViewMatrixOffset = 0x30;
constexpr int kFovRadiansOffset = 0xA0;
constexpr int kAspectOffset = 0xA4;
constexpr int kNearOffset = 0xA8;
constexpr int kFarOffset = 0xAC;
constexpr float kPi = 3.14159265358979323846F;
constexpr float kRadToDeg = 180.0F / kPi;
constexpr float kDegToRad = kPi / 180.0F;

[[nodiscard]] gpr GuestAddress(std::uint32_t address) {
    return static_cast<gpr>(static_cast<std::int64_t>(
        static_cast<std::int32_t>(address)));
}

[[nodiscard]] float ReadFloat(std::uint8_t* rdram, gpr base, int offset) {
    const auto bits = static_cast<std::uint32_t>(MEM_W(offset, base));
    return std::bit_cast<float>(bits);
}

void WriteFloat(std::uint8_t* rdram, gpr base, int offset, float value) {
    MEM_W(offset, base) = static_cast<std::int32_t>(
        std::bit_cast<std::uint32_t>(value));
}

[[nodiscard]] bool SameBits(float a, float b) {
    return std::bit_cast<std::uint32_t>(a) ==
           std::bit_cast<std::uint32_t>(b);
}

struct CameraOwnedState {
    bool active = false;
    std::uint32_t address = 0U;
    float saved_fov = 0.0F;
    float written_fov = 0.0F;
    float saved_near = 0.0F;
    float written_near = 0.0F;
    float saved_far = 0.0F;
    float written_far = 0.0F;
    std::array<float, 16> saved_matrix{};
    std::array<float, 16> written_matrix{};
    bool matrix_owned = false;
};

thread_local CameraOwnedState g_camera_owned{};

struct CameraHistory {
    std::uint32_t address = 0U;
    bool have_previous = false;
    bool have_previous2 = false;
    std::array<float, 16> previous{};
    std::array<float, 16> previous2{};
    float baseline_fov_deg = 0.0F;
    int stable_frames = 0;
    bool baseline_locked = false;
    int cinematic_frames = 0;
};

thread_local CameraHistory g_camera_history{};
thread_local bool g_camera_presentation_discontinuity_v36 = false;

void RestoreOwnedCamera(std::uint8_t* rdram) {
    if (!g_camera_owned.active) return;
    const std::uint32_t address = g_camera_owned.address;
    if (address < kRdramStart || address > kRdramEnd - kCameraBytes) {
        g_camera_owned = {};
        return;
    }
    const gpr camera = GuestAddress(address);
    if (SameBits(ReadFloat(rdram, camera, kFovRadiansOffset),
                 g_camera_owned.written_fov)) {
        WriteFloat(rdram, camera, kFovRadiansOffset,
                   g_camera_owned.saved_fov);
    }
    if (SameBits(ReadFloat(rdram, camera, kFarOffset),
                 g_camera_owned.written_far)) {
        WriteFloat(rdram, camera, kFarOffset,
                   g_camera_owned.saved_far);
    }
    if (SameBits(ReadFloat(rdram, camera, kNearOffset),
                 g_camera_owned.written_near)) {
        WriteFloat(rdram, camera, kNearOffset,
                   g_camera_owned.saved_near);
    }
    if (g_camera_owned.matrix_owned) {
        bool ours = true;
        for (int index = 0; index < 16; ++index) {
            if (!SameBits(ReadFloat(rdram, camera,
                                    kViewMatrixOffset + index * 4),
                          g_camera_owned.written_matrix[index])) {
                ours = false;
                break;
            }
        }
        if (ours) {
            for (int index = 0; index < 16; ++index) {
                WriteFloat(rdram, camera, kViewMatrixOffset + index * 4,
                           g_camera_owned.saved_matrix[index]);
            }
        }
    }
    g_camera_owned = {};
}

float PresentationNear(float near_distance, float authored_fov,
                       float effective_fov, float authored_aspect,
                       float output_aspect) {
    if (!std::isfinite(near_distance) || near_distance <= 0.0F ||
        !std::isfinite(authored_aspect) || authored_aspect <= 0.1F ||
        !std::isfinite(output_aspect) || output_aspect <= 0.1F) {
        return near_distance;
    }

    // Retail func_80039C34 derives the camera's collision radius from the
    // authored near-plane rectangle (near * tan(fov/2) * sqrt(1+aspect^2)).
    // Increasing FOV/aspect without moving the near plane makes that rectangle
    // reach beyond the original clearance, cutting open nearby walls. Keep
    // BOTH half-extents within their authored sizes, including viewport corners.
    // RT64 expands aspect later, so use the selected output aspect here without
    // changing the guest aspect a second time. Narrower views keep retail near.
    const float vertical_growth = std::tan(effective_fov * 0.5F) /
                                  std::tan(authored_fov * 0.5F);
    const float horizontal_growth = vertical_growth * output_aspect / authored_aspect;
    const float growth = std::max({1.0F, vertical_growth, horizontal_growth});
    if (!std::isfinite(growth)) return near_distance;
    const float adjusted = near_distance / growth;
    return std::isfinite(adjusted) && adjusted > 0.0F ? adjusted : near_distance;
}

[[nodiscard]] bool DetectCutscene(std::uint32_t camera_address,
                                  float authored_fov_deg,
                                  const std::array<float, 16>& raw_matrix) {
    auto& history = g_camera_history;
    if (history.address != camera_address) {
        history = {};
        history.address = camera_address;
        history.baseline_fov_deg = authored_fov_deg;
    }

    const float fov_delta = std::fabs(authored_fov_deg - history.baseline_fov_deg);
    if (!history.baseline_locked) {
        if (fov_delta <= 0.35F) {
            ++history.stable_frames;
            history.baseline_fov_deg =
                history.baseline_fov_deg * 0.98F + authored_fov_deg * 0.02F;
            if (history.stable_frames >= 30) history.baseline_locked = true;
        } else {
            history.baseline_fov_deg = authored_fov_deg;
            history.stable_frames = 0;
        }
    }

    bool discontinuity = false;
    if (history.have_previous) {
        float max_rotation_residual = 0.0F;
        float max_translation_residual = 0.0F;
        for (int index = 0; index < 16; ++index) {
            const float predicted = history.have_previous2
                ? history.previous[index] +
                      (history.previous[index] - history.previous2[index])
                : history.previous[index];
            const float residual = std::fabs(raw_matrix[index] - predicted);
            if (index == 12 || index == 13 || index == 14) {
                max_translation_residual =
                    std::max(max_translation_residual, residual);
            } else {
                max_rotation_residual =
                    std::max(max_rotation_residual, residual);
            }
        }
        // Deliberately conservative: only a material camera discontinuity is
        // considered cinematic. Ordinary analogue camera movement never trips
        // this path.
        discontinuity = max_rotation_residual > 0.20F ||
                        max_translation_residual > 24.0F;
    }

    const bool fov_cinematic = history.baseline_locked && fov_delta > 1.25F;
    if (fov_cinematic || discontinuity) {
        history.cinematic_frames = std::max(history.cinematic_frames, 24);
    } else if (history.cinematic_frames > 0) {
        --history.cinematic_frames;
    }
    g_camera_presentation_discontinuity_v36 =
        fov_cinematic || discontinuity;
    return fov_cinematic || history.cinematic_frames > 0;
}

void ApplyShakeReduction(std::uint8_t* rdram, gpr camera,
                         std::uint32_t camera_address,
                         float strength_percent,
                         const std::array<float, 16>& raw) {
    auto& history = g_camera_history;
    const float strength = std::clamp(strength_percent / 100.0F, 0.0F, 1.0F);

    if (strength >= 0.9999F || !history.have_previous) {
        history.previous2 = history.previous;
        history.previous = raw;
        history.have_previous2 = history.have_previous;
        history.have_previous = true;
        return;
    }

    std::array<float, 16> filtered = raw;
    for (int index = 0; index < 16; ++index) {
        const float predicted = history.have_previous2
            ? history.previous[index] +
                  (history.previous[index] - history.previous2[index])
            : history.previous[index];
        const float residual = raw[index] - predicted;
        const bool translation = index == 12 || index == 13 || index == 14;
        const float limit = translation ? 4.0F : 0.055F;
        // Only attenuate small high-frequency residuals. Large deltas are
        // authored camera movement/cuts and pass through unchanged.
        if (std::fabs(residual) <= limit) {
            filtered[index] = predicted + residual * strength;
        }
    }

    g_camera_owned.matrix_owned = true;
    g_camera_owned.saved_matrix = raw;
    g_camera_owned.written_matrix = filtered;
    for (int index = 0; index < 16; ++index) {
        WriteFloat(rdram, camera, kViewMatrixOffset + index * 4,
                   filtered[index]);
    }

    history.previous2 = history.previous;
    history.previous = raw;
    history.have_previous2 = history.have_previous;
    history.have_previous = true;
    (void)camera_address;
}

} // namespace

void rocket::graphics::set_config_directory(const std::filesystem::path& path) {
    std::lock_guard lock(g_settings_mutex);
    g_config_directory = path;
}

std::filesystem::path rocket::graphics::shader_directory() {
    std::lock_guard lock(g_settings_mutex);
    return g_config_directory / "shaders";
}

std::string rocket::graphics::custom_shader_base_path() {
    std::lock_guard lock(g_settings_mutex);
    if (g_settings.custom_shader.empty()) return {};
    const std::filesystem::path safe_name =
        std::filesystem::u8path(g_settings.custom_shader).filename();
    const auto value = (g_config_directory / "shaders" / safe_name).u8string();
    return std::string(value.begin(), value.end());
}

rocket::graphics::Settings rocket::graphics::settings() {
    std::lock_guard lock(g_settings_mutex);
    return g_settings;
}

void rocket::graphics::set_settings(const Settings& value, bool mark_custom) {
    Settings normalized = value;
    normalized.custom_aspect = std::clamp(normalized.custom_aspect, 1.0F, 4.0F);
    normalized.anisotropy = std::clamp(normalized.anisotropy, 1, 16);
    normalized.mip_lod_bias = std::clamp(normalized.mip_lod_bias, -2.0F, 2.0F);
    normalized.texture_detail_at_distance = std::isfinite(normalized.texture_detail_at_distance)
        ? std::clamp(normalized.texture_detail_at_distance, 0.0F, 1.0F) : 0.0F;
    normalized.sky_dither_reduction = std::isfinite(normalized.sky_dither_reduction)
        ? std::clamp(normalized.sky_dither_reduction, 0.0F, 1.0F) : 0.0F;
    normalized.fov_offset_degrees = std::clamp(normalized.fov_offset_degrees, -20.0F, 40.0F);
    normalized.draw_distance_multiplier = static_cast<float>(std::clamp(static_cast<int>(std::lround(normalized.draw_distance_multiplier)), 1, 6));
    normalized.preserve_cutscene_fov = false;
    normalized.three_point_filtering = true;
    normalized.texture_scaling_2d = TextureScaling2D::ScaledOnly;
    normalized.vi_filter = ViFilterMode::Authentic;
    normalized.texture_deband = false;
    normalized.texture_deband_strength = 0.0F;
    normalized.z_fighting = ZFightingMode::Original;
    normalized.texture_deband_strength = std::clamp(normalized.texture_deband_strength, 0.0F, 100.0F);
    normalized.post_process_strength = std::clamp(normalized.post_process_strength, 0.0F, 100.0F);
    if (mark_custom && normalized.preset != GraphicsPreset::Custom) {
        normalized.preset = GraphicsPreset::Custom;
    }
    {
        std::lock_guard lock(g_settings_mutex);
        g_settings = std::move(normalized);
    }
    g_revision.fetch_add(1U, std::memory_order_acq_rel);
}

void rocket::graphics::reset_settings() {
#if defined(__ANDROID__)
    set_settings(preset_settings(GraphicsPreset::LowPower), false);
#else
    set_settings(Settings{}, false);
#endif
}

rocket::graphics::Settings rocket::graphics::preset_settings(GraphicsPreset preset) {
    Settings s{};
    s.preset = preset;
    switch (preset) {
    case GraphicsPreset::Original:
        break;
    case GraphicsPreset::Modern:
        s.aspect = AspectPreset::FitWindow;
        s.texture_filtering = TextureFiltering::Linear;
        s.three_point_filtering = true;
        s.framebuffer_precision = FramebufferPrecision::High;
        s.anisotropy = 8;
        s.mip_lod_bias = -0.25F;
        s.draw_distance_multiplier = 2.0F;
        s.vi_filter = ViFilterMode::Clean;
        break;
    case GraphicsPreset::HighQuality:
        s.aspect = AspectPreset::FitWindow;
        s.texture_filtering = TextureFiltering::AntiAliasedPixelScaling;
        s.three_point_filtering = false;
        s.texture_scaling_2d = TextureScaling2D::All;
        s.framebuffer_precision = FramebufferPrecision::High;
        s.display_buffering = DisplayBuffering::Triple;
        s.hardware_resolve = HardwareResolve::Automatic;
        s.anisotropy = 16;
        s.mip_lod_bias = -0.65F;
        s.draw_distance_multiplier = 2.0F;
        s.vi_filter = ViFilterMode::Clean;
        s.texture_deband = true;
        s.texture_deband_strength = 45.0F;
        s.z_fighting = ZFightingMode::Consistent;
        break;
    case GraphicsPreset::Performance:
        s.aspect = AspectPreset::Original4x3;
        s.texture_filtering = TextureFiltering::Linear;
        s.three_point_filtering = true;
        s.framebuffer_precision = FramebufferPrecision::Original;
        s.display_buffering = DisplayBuffering::Double;
        s.hardware_resolve = HardwareResolve::Automatic;
        s.anisotropy = 1;
        s.mip_lod_bias = 0.0F;
        s.draw_distance_multiplier = 1.0F;
        s.vi_filter = ViFilterMode::Clean;
        s.post_process = PostProcessMode::Off;
        break;
    case GraphicsPreset::Custom:
        s = settings();
        s.preset = GraphicsPreset::Custom;
        break;
    case GraphicsPreset::LowPower:
        s.texture_filtering = TextureFiltering::Nearest;
        s.framebuffer_precision = FramebufferPrecision::Original;
        s.display_buffering = DisplayBuffering::Double;
        s.anisotropy = 1;
        s.mip_lod_bias = 0.0F;
        break;
    }
    return s;
}

void rocket::graphics::apply_preset(GraphicsPreset preset) {
    set_settings(preset_settings(preset), false);
}

std::uint64_t rocket::graphics::revision() {
    return g_revision.load(std::memory_order_acquire);
}

void rocket::graphics::request_renderer_refresh() {
    g_revision.fetch_add(1U, std::memory_order_acq_rel);
}

void rocket::graphics::set_window_aspect(float aspect) {
    if (!std::isfinite(aspect) || aspect < 0.5F || aspect > 6.0F) return;
    const float old = g_window_aspect.exchange(aspect, std::memory_order_acq_rel);
    if (!std::isfinite(old) || std::fabs(old - aspect) > 0.0001F) {
        // Fit-window aspect changes can also change the automatic HUD policy.
        // Republish renderer-owned presentation state on the next frame.
        request_renderer_refresh();
    }
}

float rocket::graphics::window_aspect() {
    return g_window_aspect.load(std::memory_order_acquire);
}

float rocket::graphics::selected_aspect(float authored_aspect) {
    const Settings s = settings();
    switch (s.aspect) {
    case AspectPreset::FitWindow:
        return std::max(authored_aspect, window_aspect());
    case AspectPreset::Ratio16x9:
        return std::max(authored_aspect, 16.0F / 9.0F);
    case AspectPreset::Ratio16x10:
        return std::max(authored_aspect, 16.0F / 10.0F);
    case AspectPreset::Ratio21x9:
        return std::max(authored_aspect, 21.0F / 9.0F);
    case AspectPreset::Custom:
        return std::max(authored_aspect, s.custom_aspect);
    case AspectPreset::Original4x3:
    default:
        return authored_aspect;
    }
}

bool rocket::graphics::widescreen_active(float authored_aspect) {
    return selected_aspect(authored_aspect) > authored_aspect * 1.0001F;
}

bool rocket::graphics::cutscene_active() {
    return g_cutscene_active.load(std::memory_order_acquire);
}

float rocket::graphics::effective_fov_radians(float authored_fov_radians) {
    if (!std::isfinite(authored_fov_radians) || authored_fov_radians <= 0.05F ||
        authored_fov_radians >= kPi - 0.05F) {
        return authored_fov_radians;
    }
    const Settings s = settings();
    if (std::fabs(s.fov_offset_degrees) <= 0.001F) {
        return authored_fov_radians;
    }
    const float adjusted_deg = std::clamp(
        authored_fov_radians * kRadToDeg + s.fov_offset_degrees,
        25.0F, 120.0F);
    return adjusted_deg * kDegToRad;
}

void rocket::graphics::publish_performance(const PerformanceStats& stats) {
    std::lock_guard lock(g_performance_mutex);
    g_performance = stats;
}

rocket::graphics::PerformanceStats rocket::graphics::performance_stats() {
    std::lock_guard lock(g_performance_mutex);
    return g_performance;
}

extern "C" void rocket_graphics_camera_begin(std::uint8_t* rdram,
                                                recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return;
    RestoreOwnedCamera(rdram);

    const std::uint32_t camera_address = static_cast<std::uint32_t>(context->r4);
    if (camera_address < kRdramStart ||
        camera_address > kRdramEnd - kCameraBytes) return;
    const gpr camera = GuestAddress(camera_address);

    const Settings s = rocket::graphics::settings();
    const float authored_fov = ReadFloat(rdram, camera, kFovRadiansOffset);
    const float authored_near = ReadFloat(rdram, camera, kNearOffset);
    const float authored_far = ReadFloat(rdram, camera, kFarOffset);
    if (!std::isfinite(authored_fov) || authored_fov <= 0.05F ||
        authored_fov >= kPi - 0.05F || !std::isfinite(authored_far)) return;

    std::array<float, 16> raw_matrix{};
    for (int index = 0; index < 16; ++index) {
        raw_matrix[index] = ReadFloat(rdram, camera,
                                      kViewMatrixOffset + index * 4);
        if (!std::isfinite(raw_matrix[index])) return;
    }

    const float authored_fov_deg = authored_fov * kRadToDeg;
    const bool cinematic = DetectCutscene(camera_address, authored_fov_deg,
                                          raw_matrix);
    g_cutscene_active.store(cinematic, std::memory_order_release);
    rocket_presentation_camera_source(
        rdram, context, g_camera_presentation_discontinuity_v36 ? 1U : 0U);

    g_camera_owned.active = true;
    g_camera_owned.address = camera_address;
    g_camera_owned.saved_fov = authored_fov;
    g_camera_owned.written_fov = authored_fov;
    g_camera_owned.saved_near = authored_near;
    g_camera_owned.written_near = authored_near;
    g_camera_owned.saved_far = authored_far;
    g_camera_owned.written_far = authored_far;

    const float effective_fov =
        rocket::graphics::effective_fov_radians(authored_fov);
    if (!SameBits(effective_fov, authored_fov)) {
        g_camera_owned.written_fov = effective_fov;
        WriteFloat(rdram, camera, kFovRadiansOffset,
                   g_camera_owned.written_fov);
    }

    const float authored_aspect = ReadFloat(rdram, camera, kAspectOffset);
    const float output_aspect = rocket::graphics::selected_aspect(authored_aspect);
    g_camera_owned.written_near = PresentationNear(
        authored_near, authored_fov, effective_fov, authored_aspect, output_aspect);
    if (!SameBits(g_camera_owned.written_near, authored_near)) {
        WriteFloat(rdram, camera, kNearOffset, g_camera_owned.written_near);
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr,
                "[graphics] near-plane clearance: %.3f -> %.3f world units "
                "(vertical FOV %.1f -> %.1f, aspect %.3f -> %.3f)\n",
                authored_near, g_camera_owned.written_near,
                authored_fov * kRadToDeg, effective_fov * kRadToDeg,
                authored_aspect, output_aspect);
        }
    }

    if (s.draw_distance_multiplier > 1.0001F) {
        // Camera far is stored in sixteenth-world units and multiplied by 16
        // immediately before guPerspective. Draw Distance may extend the far
        // clip, but it never reduces the authored clip plane.
        g_camera_owned.written_far = std::min(
            authored_far * s.draw_distance_multiplier, 32767.0F);
        WriteFloat(rdram, camera, kFarOffset, g_camera_owned.written_far);
    }

    // Screen-shake adjustment is intentionally no longer a user setting.
    // 100% is Rocket's authored camera motion. Calling the history helper at
    // 100% updates cutscene/FOV history without modifying the view matrix.
    ApplyShakeReduction(rdram, camera, camera_address, 100.0F, raw_matrix);
}

extern "C" void rocket_graphics_camera_end(std::uint8_t* rdram,
                                         recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return;
    // guPerspective and the task's matrix copies have consumed the temporary
    // values. Restore them before camera collision/room logic can run again,
    // even on frames with no frustum tests. The matrices retain the correction.
    RestoreOwnedCamera(rdram);
}

extern "C" void rocket_graphics_frustum_begin(std::uint8_t* rdram,
                                                 recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return;

    // Defensive fallback; the projection-return hook normally restores these.
    RestoreOwnedCamera(rdram);
    const Settings s = rocket::graphics::settings();

    // Rocket's Vec3f parameter decays to a pointer. frustum_test therefore uses:
    //   r4 = camera pointer
    //   r5 = position pointer
    //   r6 = cullRadius float bits
    //   r7 = renderDistance float bits
    //   sp+0x10 = arg4 pointer
    //   sp+0x14 = alphaOut pointer
    // Keep Draw Distance on the real r7 argument. Do not read stack + 0x14.
    const std::uint32_t bits = static_cast<std::uint32_t>(context->r7);
    const float authored_distance = std::bit_cast<float>(bits);
    if (!std::isfinite(authored_distance) || authored_distance <= 0.0F) return;

    constexpr float kInfiniteRenderDistance = 3.402823466e+38F;
    if (authored_distance >= 3.0e+38F) {
        // Retail Rocket already uses FLT_MAX to mean no distance cull.
        context->r7 = static_cast<gpr>(
            std::bit_cast<std::uint32_t>(kInfiniteRenderDistance));
        return;
    }

    // v12's UI cleanup removed the old Maximum Detail toggle. v17 accidentally
    // referenced that retired Settings member, which made current Rocket-R fail
    // to compile. The supported control is Draw Distance, so use only it here.
    const float multiplier = s.draw_distance_multiplier;
    if (!std::isfinite(multiplier) || multiplier <= 0.0F) return;

    float adjusted = authored_distance * multiplier;
    if (!std::isfinite(adjusted) || adjusted <= 0.0F) return;
    adjusted = std::min(adjusted, 1.0e+20F);

    context->r7 = static_cast<gpr>(
        std::bit_cast<std::uint32_t>(adjusted));
}
