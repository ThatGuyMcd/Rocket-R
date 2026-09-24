#include "graphics_enhancements.hpp"

#include "recomp.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
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
    normalized.fov_offset_degrees = std::clamp(normalized.fov_offset_degrees, -20.0F, 40.0F);
    normalized.screen_shake_strength = std::clamp(normalized.screen_shake_strength, 0.0F, 100.0F);
    normalized.draw_distance_multiplier = std::clamp(normalized.draw_distance_multiplier, 0.5F, 8.0F);
    normalized.fog_distance_multiplier = std::clamp(normalized.fog_distance_multiplier, 0.0F, 8.0F);
    normalized.hud_scale_percent = std::clamp(normalized.hud_scale_percent, 50.0F, 150.0F);
    normalized.hud_safe_margin_percent = std::clamp(normalized.hud_safe_margin_percent, 0.0F, 20.0F);
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
    set_settings(Settings{}, false);
}

void rocket::graphics::apply_preset(GraphicsPreset preset) {
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
        s.draw_distance_multiplier = 1.5F;
        s.widescreen_culling = true;
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
        s.maximum_detail = true;
        s.widescreen_culling = true;
        s.vi_filter = ViFilterMode::Clean;
        s.texture_deband = true;
        s.texture_deband_strength = 45.0F;
        s.z_fighting = ZFightingMode::Consistent;
        break;
    case GraphicsPreset::Performance:
        s.aspect = AspectPreset::FitWindow;
        s.texture_filtering = TextureFiltering::Linear;
        s.three_point_filtering = true;
        s.framebuffer_precision = FramebufferPrecision::Original;
        s.display_buffering = DisplayBuffering::Double;
        s.hardware_resolve = HardwareResolve::On;
        s.anisotropy = 4;
        s.draw_distance_multiplier = 1.0F;
        s.vi_filter = ViFilterMode::Clean;
        s.post_process = PostProcessMode::Off;
        break;
    case GraphicsPreset::Custom:
        s = settings();
        s.preset = GraphicsPreset::Custom;
        break;
    }
    set_settings(s, false);
}

std::uint64_t rocket::graphics::revision() {
    return g_revision.load(std::memory_order_acquire);
}

void rocket::graphics::request_renderer_refresh() {
    g_revision.fetch_add(1U, std::memory_order_acq_rel);
}

void rocket::graphics::set_window_aspect(float aspect) {
    if (std::isfinite(aspect) && aspect >= 0.5F && aspect <= 6.0F) {
        g_window_aspect.store(aspect, std::memory_order_release);
    }
}

float rocket::graphics::window_aspect() {
    return g_window_aspect.load(std::memory_order_acquire);
}

float rocket::graphics::selected_aspect(float authored_aspect) {
    const Settings s = settings();
    // Cutscene framing is presentation-only. When requested, keep both RT64's
    // output aspect and Rocket's CPU-side frustum guard on the authored 4:3
    // view for the same detected cinematic interval.
    if (s.original_aspect_cutscenes && cutscene_active()) {
        return authored_aspect;
    }
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
    const Settings s = settings();
    return s.widescreen_culling && selected_aspect(authored_aspect) >
        authored_aspect * 1.0001F;
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
    if (std::fabs(s.fov_offset_degrees) <= 0.001F ||
        (s.preserve_cutscene_fov && cutscene_active())) {
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

    g_camera_owned.active = true;
    g_camera_owned.address = camera_address;
    g_camera_owned.saved_fov = authored_fov;
    g_camera_owned.written_fov = authored_fov;
    g_camera_owned.saved_far = authored_far;
    g_camera_owned.written_far = authored_far;

    const float effective_fov =
        rocket::graphics::effective_fov_radians(authored_fov);
    if (!SameBits(effective_fov, authored_fov)) {
        g_camera_owned.written_fov = effective_fov;
        WriteFloat(rdram, camera, kFovRadiansOffset,
                   g_camera_owned.written_fov);
    }

    if (s.maximum_detail || s.draw_distance_multiplier > 1.0001F) {
        const float multiplier = s.maximum_detail
            ? 8.0F : s.draw_distance_multiplier;
        // Camera far is stored in sixteenth-world units and multiplied by 16
        // immediately before guPerspective. Keep the original near plane and
        // only extend the far clip; never reduce it below the authored value.
        g_camera_owned.written_far = std::min(authored_far * multiplier,
                                               32767.0F);
        WriteFloat(rdram, camera, kFarOffset, g_camera_owned.written_far);
    }

    ApplyShakeReduction(rdram, camera, camera_address,
                        s.screen_shake_strength, raw_matrix);
}

extern "C" void rocket_graphics_frustum_begin(std::uint8_t* rdram,
                                                 recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return;
    // func_8003ACD4 has already consumed the temporary camera values into the
    // graphics task by the time object frustum tests begin. Restore the guest
    // camera immediately so presentation-only FOV/far/shake changes cannot leak
    // into later simulation/camera logic during the same authored frame.
    RestoreOwnedCamera(rdram);
    const Settings s = rocket::graphics::settings();

    // MIPS o32: frustum_test(camera, Vec3f-by-value, cullRadius,
    // renderDistance, ...) places renderDistance at sp+0x14 at function entry.
    // Scale only finite authored distances; FLT_MAX is Rocket's explicit
    // infinity sentinel and remains untouched unless Maximum Detail is chosen.
    const gpr sp = context->r29;
    const std::uint32_t bits = static_cast<std::uint32_t>(MEM_W(0x14, sp));
    const float authored_distance = std::bit_cast<float>(bits);
    if (!std::isfinite(authored_distance) || authored_distance <= 0.0F) return;

    constexpr float kInfiniteRenderDistance = 3.402823466e+38F;
    if (authored_distance >= 3.0e+38F) {
        // Rocket uses FLT_MAX as an explicit no-distance-cull sentinel.
        // Preserve it bit-for-bit unless Maximum Detail also requests the same
        // sentinel (which is still a no-op).
        if (s.maximum_detail) {
            MEM_W(0x14, sp) = static_cast<std::int32_t>(
                std::bit_cast<std::uint32_t>(kInfiniteRenderDistance));
        }
        return;
    }

    float adjusted = s.maximum_detail
        ? kInfiniteRenderDistance
        : authored_distance * s.draw_distance_multiplier;
    if (!s.maximum_detail && std::isfinite(adjusted) && adjusted > 0.0F) {
        adjusted = std::min(adjusted, 1.0e+20F);
    }
    MEM_W(0x14, sp) = static_cast<std::int32_t>(
        std::bit_cast<std::uint32_t>(adjusted));
}
