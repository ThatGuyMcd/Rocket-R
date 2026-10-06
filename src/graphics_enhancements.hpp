#pragma once
#include "recomp.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace rocket::graphics {

enum class GraphicsPreset : int {
    Original = 0,
    Modern,
    HighQuality,
    Performance,
    Custom,
    LowPower,
};

enum class AspectPreset : int {
    Original4x3 = 0,
    FitWindow,
    Ratio16x9,
    Ratio16x10,
    Ratio21x9,
    Custom,
};

enum class TextureFiltering : int {
    Nearest = 0,
    Linear,
    AntiAliasedPixelScaling,
};

enum class TextureScaling2D : int {
    Original = 0,
    ScaledOnly,
    All,
};

enum class FramebufferPrecision : int {
    Automatic = 0,
    Original,
    High,
};

enum class DisplayBuffering : int {
    Double = 0,
    Triple,
};

enum class HardwareResolve : int {
    Automatic = 0,
    Off,
    On,
};

enum class ViFilterMode : int {
    Authentic = 0,
    Clean,
};

enum class ZFightingMode : int {
    Original = 0,
    Consistent,
    Strong,
};

enum class PostProcessMode : int {
    Off = 0,
    Scanlines,
    CRT,
    Custom,
};

struct Settings {
    GraphicsPreset preset = GraphicsPreset::Original;
    AspectPreset aspect = AspectPreset::Original4x3;
    float custom_aspect = 16.0F / 9.0F;

    TextureFiltering texture_filtering = TextureFiltering::AntiAliasedPixelScaling;
    bool three_point_filtering = true;
    TextureScaling2D texture_scaling_2d = TextureScaling2D::ScaledOnly;
    FramebufferPrecision framebuffer_precision = FramebufferPrecision::Automatic;
    DisplayBuffering display_buffering = DisplayBuffering::Triple;
    HardwareResolve hardware_resolve = HardwareResolve::Automatic;
    int anisotropy = 16;
    float mip_lod_bias = -0.25F;
    float texture_detail_at_distance = 0.0F;
    float sky_dither_reduction = 0.0F;

    float fov_offset_degrees = 0.0F;
    bool preserve_cutscene_fov = true;

    float draw_distance_multiplier = 1.0F;


    bool vsync = true;
    ViFilterMode vi_filter = ViFilterMode::Authentic;
    bool texture_deband = false;
    float texture_deband_strength = 35.0F;
    ZFightingMode z_fighting = ZFightingMode::Original;
    PostProcessMode post_process = PostProcessMode::Off;
    float post_process_strength = 70.0F;
    std::string custom_shader;

    bool performance_overlay = false;
    bool performance_logging = false;
    bool interpolation_overlay = false;
};

struct PerformanceStats {
    float fps = 0.0F;
    float frame_ms = 0.0F;
    float resolution_scale = 1.0F;
    int display_rate = 0;
    int target_rate = 0;
    std::uint64_t presents = 0;
    std::uint64_t interpolated_presents = 0;
    std::string device_name;
    bool software_renderer = false;
};

void set_config_directory(const std::filesystem::path& path);
std::filesystem::path shader_directory();
std::string custom_shader_base_path();

Settings settings();
void set_settings(const Settings& value, bool mark_custom = true);
void reset_settings();
Settings preset_settings(GraphicsPreset preset);
void apply_preset(GraphicsPreset preset);
std::uint64_t revision();

void set_window_aspect(float aspect);
float window_aspect();
float selected_aspect(float authored_aspect = 4.0F / 3.0F);
bool widescreen_active(float authored_aspect = 4.0F / 3.0F);
bool cutscene_active();
float effective_fov_radians(float authored_fov_radians);

void publish_performance(const PerformanceStats& stats);
PerformanceStats performance_stats();

// Render-thread notification used by UI/settings changes that are not part of
// N64ModernRuntime's GraphicsConfig. The RT64 context consumes the revision at
// a frame boundary and applies all renderer-owned settings together.
void request_renderer_refresh();

} // namespace rocket::graphics

extern "C" {
void rocket_graphics_camera_begin(std::uint8_t* rdram, recomp_context* context);
void rocket_graphics_camera_end(std::uint8_t* rdram, recomp_context* context);
void rocket_graphics_frustum_begin(std::uint8_t* rdram, recomp_context* context);
}
