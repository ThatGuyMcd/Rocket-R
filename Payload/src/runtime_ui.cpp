#include "runtime_ui.hpp"

#include "game_registration.hpp"
#include "graphics_enhancements.hpp"
#include "platform.hpp"
#include "presentation_identity.hpp"
#include "runtime_input.hpp"
#include "widescreen_culling.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Unknwn.h>
#include <oaidl.h>
#include <commdlg.h>
#endif

#include "gui/rt64_inspector.h"
#include "hle/rt64_application.h"
#include "hle/rt64_present_queue.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl2_custom.h"
#include "imgui/backends/imgui_impl_sdlrenderer2.h"
#include "ultramodern/config.hpp"
#include "ultramodern/ultramodern.hpp"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <set>
#include <string>
#include <vector>

namespace {

std::filesystem::path g_config_directory;
std::atomic<bool> g_overlay_visible{false};
std::atomic<bool> g_n64_dithering_enabled{true};
std::mutex g_inspector_guard;
RT64::Inspector* g_inspector = nullptr;
int g_overlay_page = 0;
ImFont* g_launcher_body_font = nullptr;
ImFont* g_launcher_heading_font = nullptr;
bool g_launcher_context_active = false;


enum class CaptureDevice { None, Keyboard, Controller };
CaptureDevice g_capture_device = CaptureDevice::None;
rocket::input::BindingSlot g_capture_slot = rocket::input::BindingSlot::KeyboardPrimary;
int g_capture_action = -1;
bool g_capture_shortcut = false;
rocket::input::ShortcutAction g_capture_shortcut_action = rocket::input::ShortcutAction::ToggleOverlay;
bool g_capture_popup_pending = false;
bool g_capture_finished = false;
std::chrono::steady_clock::time_point g_capture_started{};
int g_controls_section = 0;
int g_graphics_section = 0;

constexpr ImVec4 kBackground{0.025F, 0.055F, 0.09F, 1.0F};
constexpr ImVec4 kPanel{0.04F, 0.095F, 0.14F, 1.0F};
constexpr ImVec4 kPanelSoft{0.065F, 0.15F, 0.20F, 1.0F};
constexpr ImVec4 kText{0.96F, 0.97F, 0.94F, 1.0F};
constexpr ImVec4 kMuted{0.65F, 0.72F, 0.75F, 1.0F};
constexpr ImVec4 kAccent{0.13F, 0.78F, 0.70F, 1.0F};
constexpr ImVec4 kWarm{1.0F, 0.48F, 0.08F, 1.0F};
constexpr ImVec4 kRed{0.82F, 0.16F, 0.13F, 1.0F};


std::string PathUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}

std::filesystem::path SettingsPath() {
    return g_config_directory / "rocket-r-settings.ini";
}

std::filesystem::path LastRomPath() {
    return g_config_directory / "last-rom.txt";
}


std::optional<std::filesystem::path> FindComicSansFont(bool bold) {
    const char* override_name = bold ? "ROCKET_R_COMIC_SANS_BOLD" : "ROCKET_R_COMIC_SANS";
    if (const char* override_path = std::getenv(override_name)) {
        if (*override_path != '\0') {
            const auto candidate = std::filesystem::u8path(override_path);
            std::error_code ec;
            if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
        }
    }

    std::array<std::filesystem::path, 10> candidates{};
    std::size_t count = 0;
#if defined(_WIN32)
    wchar_t windows_dir[MAX_PATH]{};
    const UINT windows_len = GetWindowsDirectoryW(windows_dir, MAX_PATH);
    if (windows_len > 0 && windows_len < MAX_PATH) {
        const std::filesystem::path fonts = std::filesystem::path(windows_dir) / L"Fonts";
        if (bold) {
            candidates[count++] = fonts / L"comicbd.ttf";
            candidates[count++] = fonts / L"Comic Sans MS Bold.ttf";
            candidates[count++] = fonts / L"comic.ttf";
        } else {
            candidates[count++] = fonts / L"comic.ttf";
            candidates[count++] = fonts / L"Comic Sans MS.ttf";
            candidates[count++] = fonts / L"comicbd.ttf";
        }
    }
#elif defined(__linux__)
    if (bold) {
        candidates[count++] = "/usr/share/fonts/truetype/msttcorefonts/comicbd.ttf";
        candidates[count++] = "/usr/share/fonts/truetype/msttcorefonts/Comic_Sans_MS_Bold.ttf";
        candidates[count++] = "/usr/local/share/fonts/comicbd.ttf";
        candidates[count++] = "/usr/local/share/fonts/Comic_Sans_MS_Bold.ttf";
        candidates[count++] = "/usr/share/fonts/truetype/msttcorefonts/comic.ttf";
    } else {
        candidates[count++] = "/usr/share/fonts/truetype/msttcorefonts/comic.ttf";
        candidates[count++] = "/usr/share/fonts/truetype/msttcorefonts/Comic_Sans_MS.ttf";
        candidates[count++] = "/usr/local/share/fonts/comic.ttf";
        candidates[count++] = "/usr/local/share/fonts/Comic_Sans_MS.ttf";
        candidates[count++] = "/usr/share/fonts/truetype/msttcorefonts/comicbd.ttf";
    }
#endif

    for (std::size_t index = 0; index < count; ++index) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidates[index], ec)) return candidates[index];
    }
    return std::nullopt;
}

void ConfigureLauncherFonts() {
    ImGuiIO& io = ImGui::GetIO();
    constexpr float BodyPixels = 24.0F;
    constexpr float HeadingPixels = 34.0F;

    const auto comic_body = FindComicSansFont(false);
    const auto comic_heading = FindComicSansFont(true);
    if (comic_body.has_value()) {
        const std::string body_path = PathUtf8(*comic_body);
        g_launcher_body_font = io.Fonts->AddFontFromFileTTF(body_path.c_str(), BodyPixels);
        const std::string heading_path = comic_heading.has_value() ? PathUtf8(*comic_heading) : body_path;
        g_launcher_heading_font = io.Fonts->AddFontFromFileTTF(heading_path.c_str(), HeadingPixels);
        if (g_launcher_body_font != nullptr && g_launcher_heading_font != nullptr) {
            io.FontDefault = g_launcher_body_font;
            std::fprintf(stderr,
                         "[launcher] Comic Sans UI fonts: body=%s (24px), heading=%s (34px)\n",
                         body_path.c_str(), heading_path.c_str());
            return;
        }
    }

    ImFontConfig body_fallback{};
    body_fallback.SizePixels = BodyPixels;
    g_launcher_body_font = io.Fonts->AddFontDefault(&body_fallback);
    ImFontConfig heading_fallback{};
    heading_fallback.SizePixels = HeadingPixels;
    g_launcher_heading_font = io.Fonts->AddFontDefault(&heading_fallback);
    io.FontDefault = g_launcher_body_font;
    std::fprintf(stderr,
                 "[launcher] Comic Sans was not found; using larger 24px/34px ImGui fallback fonts. "
                 "Set ROCKET_R_COMIC_SANS and optionally ROCKET_R_COMIC_SANS_BOLD to local TTF paths to override.\n");
}

void LauncherHeading(const char* text) {
    if (g_launcher_context_active && g_launcher_heading_font != nullptr) {
        ImGui::PushFont(g_launcher_heading_font);
        ImGui::TextUnformatted(text);
        ImGui::PopFont();
    } else {
        ImGui::TextUnformatted(text);
    }
}

void ApplyStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0F;
    style.ChildRounding = 16.0F;
    style.FrameRounding = 10.0F;
    style.PopupRounding = 12.0F;
    style.GrabRounding = 10.0F;
    style.WindowPadding = {24.0F, 24.0F};
    style.FramePadding = {18.0F, 13.0F};
    style.ItemSpacing = {16.0F, 15.0F};
    style.WindowBorderSize = 0.0F;
    style.ChildBorderSize = 1.0F;
    style.FrameBorderSize = 1.0F;
    style.Colors[ImGuiCol_WindowBg] = kBackground;
    style.Colors[ImGuiCol_ChildBg] = kPanel;
    style.Colors[ImGuiCol_PopupBg] = kPanel;
    style.Colors[ImGuiCol_Border] = {0.15F, 0.34F, 0.38F, 1.0F};
    style.Colors[ImGuiCol_Text] = kText;
    style.Colors[ImGuiCol_TextDisabled] = kMuted;
    style.Colors[ImGuiCol_FrameBg] = kPanelSoft;
    style.Colors[ImGuiCol_FrameBgHovered] = {0.08F, 0.24F, 0.28F, 1.0F};
    style.Colors[ImGuiCol_FrameBgActive] = {0.10F, 0.32F, 0.32F, 1.0F};
    style.Colors[ImGuiCol_Button] = {0.06F, 0.35F, 0.43F, 1.0F};
    style.Colors[ImGuiCol_ButtonHovered] = kWarm;
    style.Colors[ImGuiCol_ButtonActive] = {0.92F, 0.31F, 0.05F, 1.0F};
    style.Colors[ImGuiCol_Header] = {0.05F, 0.38F, 0.42F, 1.0F};
    style.Colors[ImGuiCol_HeaderHovered] = kWarm;
    style.Colors[ImGuiCol_HeaderActive] = {0.90F, 0.30F, 0.05F, 1.0F};
    style.Colors[ImGuiCol_CheckMark] = kAccent;
    style.Colors[ImGuiCol_SliderGrab] = kWarm;
    style.Colors[ImGuiCol_SliderGrabActive] = {1.0F, 0.65F, 0.12F, 1.0F};
    style.Colors[ImGuiCol_Separator] = {0.20F, 0.53F, 0.50F, 0.65F};
    style.Colors[ImGuiCol_NavHighlight] = {1.0F, 0.72F, 0.15F, 1.0F};
}

void SaveSettings() {
    std::error_code ec;
    std::filesystem::create_directories(g_config_directory, ec);
    std::ofstream out(SettingsPath(), std::ios::trunc);
    if (!out) return;
    const auto& g = ultramodern::renderer::get_graphics_config();
    out << "window_mode=" << static_cast<int>(g.wm_option) << '\n';
    out << "graphics_api=" << static_cast<int>(g.api_option) << '\n';
    out << "aspect_ratio=" << static_cast<int>(g.ar_option) << '\n';
    out << "resolution=" << static_cast<int>(g.res_option) << '\n';
    out << "msaa=" << static_cast<int>(g.msaa_option) << '\n';
    out << "refresh_rate=" << static_cast<int>(g.rr_option) << '\n';
    out << "refresh_manual=" << std::clamp(g.rr_manual_value, 30, 500) << '\n';
    out << "downsample=" << g.ds_option << '\n';
    out << "n64_dithering=" << (g_n64_dithering_enabled.load(std::memory_order_relaxed) ? 1 : 0) << '\n';
    const auto gx = rocket::graphics::settings();
    out << "graphics_preset=" << static_cast<int>(gx.preset) << '\n';
    out << "aspect_preset=" << static_cast<int>(gx.aspect) << '\n';
    out << "aspect_custom=" << gx.custom_aspect << '\n';
    out << "texture_filtering=" << static_cast<int>(gx.texture_filtering) << '\n';
    out << "three_point_filtering=" << (gx.three_point_filtering ? 1 : 0) << '\n';
    out << "texture_scaling_2d=" << static_cast<int>(gx.texture_scaling_2d) << '\n';
    out << "framebuffer_precision=" << static_cast<int>(gx.framebuffer_precision) << '\n';
    out << "display_buffering=" << static_cast<int>(gx.display_buffering) << '\n';
    out << "hardware_resolve=" << static_cast<int>(gx.hardware_resolve) << '\n';
    out << "anisotropy=" << gx.anisotropy << '\n';
    out << "mip_lod_bias=" << gx.mip_lod_bias << '\n';
    out << "fov_offset=" << gx.fov_offset_degrees << '\n';
    out << "preserve_cutscene_fov=" << (gx.preserve_cutscene_fov ? 1 : 0) << '\n';
    out << "cutscene_original_aspect=" << (gx.original_aspect_cutscenes ? 1 : 0) << '\n';
    out << "screen_shake_strength=" << gx.screen_shake_strength << '\n';
    out << "draw_distance=" << gx.draw_distance_multiplier << '\n';
    out << "maximum_detail=" << (gx.maximum_detail ? 1 : 0) << '\n';
    out << "widescreen_culling=" << (gx.widescreen_culling ? 1 : 0) << '\n';
    out << "fog_distance=" << gx.fog_distance_multiplier << '\n';
    out << "hud_aspect=" << static_cast<int>(gx.hud_aspect) << '\n';
    out << "hud_scale=" << gx.hud_scale_percent << '\n';
    out << "hud_safe_margin=" << gx.hud_safe_margin_percent << '\n';
    out << "vsync=" << (gx.vsync ? 1 : 0) << '\n';
    out << "vi_filter=" << static_cast<int>(gx.vi_filter) << '\n';
    out << "texture_deband=" << (gx.texture_deband ? 1 : 0) << '\n';
    out << "texture_deband_strength=" << gx.texture_deband_strength << '\n';
    out << "z_fighting=" << static_cast<int>(gx.z_fighting) << '\n';
    out << "post_process=" << static_cast<int>(gx.post_process) << '\n';
    out << "post_process_strength=" << gx.post_process_strength << '\n';
    out << "custom_shader=" << gx.custom_shader << '\n';
    out << "performance_overlay=" << (gx.performance_overlay ? 1 : 0) << '\n';
    out << "interpolation_overlay=" << (gx.interpolation_overlay ? 1 : 0) << '\n';
    out << "volume=" << rocket::platform::master_volume() << '\n';
    out << "audio_profile=2\n";
    out << "rumble=" << (rocket::platform::rumble_enabled() ? 1 : 0) << '\n';
    out << "rumble_strength=" << rocket::platform::rumble_strength() << '\n';
    out << "controller_device=" << rocket::platform::preferred_controller_key() << '\n';
    out << "background_input=" << (rocket::input::background_input_enabled() ? 1 : 0) << '\n';
    out << "stick_deadzone=" << rocket::input::stick_deadzone() << '\n';
    out << "stick_anti_deadzone=" << rocket::input::stick_anti_deadzone() << '\n';
    out << "stick_sensitivity=" << rocket::input::stick_sensitivity() << '\n';
    out << "stick_curve=" << rocket::input::stick_curve() << '\n';
    out << "stick_x_inverted=" << (rocket::input::stick_x_inverted() ? 1 : 0) << '\n';
    out << "stick_y_inverted=" << (rocket::input::stick_y_inverted() ? 1 : 0) << '\n';
    out << "trigger_threshold=" << rocket::input::trigger_threshold() << '\n';
    for (std::size_t index = 0; index < rocket::input::action_count(); ++index) {
        const auto action = static_cast<rocket::input::Action>(index);
        const std::string prefix = std::string("input.") + rocket::input::action_identifier(action) + ".";
        out << prefix << "keyboard_primary=" << rocket::input::binding(action, rocket::input::BindingSlot::KeyboardPrimary) << '\n';
        out << prefix << "keyboard_secondary=" << rocket::input::binding(action, rocket::input::BindingSlot::KeyboardSecondary) << '\n';
        out << prefix << "controller_primary=" << rocket::input::binding(action, rocket::input::BindingSlot::ControllerPrimary) << '\n';
        out << prefix << "controller_secondary=" << rocket::input::binding(action, rocket::input::BindingSlot::ControllerSecondary) << '\n';
    }
    out << "shortcut.overlay.keyboard=" << rocket::input::shortcut_keyboard_binding(rocket::input::ShortcutAction::ToggleOverlay) << '\n';
    out << "shortcut.overlay.controller=" << rocket::input::shortcut_controller_binding(rocket::input::ShortcutAction::ToggleOverlay) << '\n';
    out << "shortcut.fullscreen.keyboard=" << rocket::input::shortcut_keyboard_binding(rocket::input::ShortcutAction::ToggleFullscreen) << '\n';
    out << "shortcut.fullscreen.controller=" << rocket::input::shortcut_controller_binding(rocket::input::ShortcutAction::ToggleFullscreen) << '\n';
}

void LoadSettings() {
    ultramodern::renderer::GraphicsConfig graphics{};
    graphics.developer_mode = false;
    graphics.res_option = ultramodern::renderer::Resolution::Auto;
    graphics.wm_option = ultramodern::renderer::WindowMode::Windowed;
    graphics.hr_option = ultramodern::renderer::HUDRatioMode::Original;
    graphics.api_option = ultramodern::renderer::GraphicsApi::Auto;
    graphics.ar_option = ultramodern::renderer::AspectRatio::Original;
    graphics.msaa_option = ultramodern::renderer::Antialiasing::None;
    graphics.rr_option = ultramodern::renderer::RefreshRate::Original;
    graphics.hpfb_option = ultramodern::renderer::HighPrecisionFramebuffer::Auto;
    graphics.rr_manual_value = 60;
    graphics.ds_option = 1;
    float volume = 0.65F;
    bool n64_dithering = true;
    int audio_profile = 0;
    rocket::graphics::Settings graphics_extra{};
    bool aspect_preset_loaded = false;

    std::ifstream in(SettingsPath());
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        bool input_binding_handled = false;
        for (std::size_t index = 0; index < rocket::input::action_count() && !input_binding_handled; ++index) {
            const auto action = static_cast<rocket::input::Action>(index);
            const std::string prefix = std::string("input.") + rocket::input::action_identifier(action) + ".";
            try {
                if (key == prefix + "keyboard_primary") {
                    rocket::input::set_binding(action, rocket::input::BindingSlot::KeyboardPrimary, std::stoi(value));
                    input_binding_handled = true;
                } else if (key == prefix + "keyboard_secondary") {
                    rocket::input::set_binding(action, rocket::input::BindingSlot::KeyboardSecondary, std::stoi(value));
                    input_binding_handled = true;
                } else if (key == prefix + "controller_primary") {
                    rocket::input::set_binding(action, rocket::input::BindingSlot::ControllerPrimary, std::stoi(value));
                    input_binding_handled = true;
                } else if (key == prefix + "controller_secondary") {
                    rocket::input::set_binding(action, rocket::input::BindingSlot::ControllerSecondary, std::stoi(value));
                    input_binding_handled = true;
                }
            } catch (...) {}
        }
        if (input_binding_handled) continue;
        try {
            if (key == "window_mode") graphics.wm_option = static_cast<ultramodern::renderer::WindowMode>(std::stoi(value));
            else if (key == "graphics_api") graphics.api_option = static_cast<ultramodern::renderer::GraphicsApi>(std::stoi(value));
            else if (key == "aspect_ratio") graphics.ar_option = static_cast<ultramodern::renderer::AspectRatio>(std::stoi(value));
            else if (key == "resolution") graphics.res_option = static_cast<ultramodern::renderer::Resolution>(std::stoi(value));
            else if (key == "msaa") graphics.msaa_option = static_cast<ultramodern::renderer::Antialiasing>(std::stoi(value));
            else if (key == "refresh_rate") {
                const int parsed = std::stoi(value);
                if (parsed >= 0 && parsed < static_cast<int>(ultramodern::renderer::RefreshRate::OptionCount)) {
                    graphics.rr_option = static_cast<ultramodern::renderer::RefreshRate>(parsed);
                }
            }
            else if (key == "refresh_manual") graphics.rr_manual_value = std::clamp(std::stoi(value), 30, 500);
            else if (key == "downsample") graphics.ds_option = std::clamp(std::stoi(value), 1, 4);
            else if (key == "n64_dithering") n64_dithering = std::stoi(value) != 0;
            else if (key == "graphics_preset") graphics_extra.preset = static_cast<rocket::graphics::GraphicsPreset>(std::clamp(std::stoi(value), 0, 4));
            else if (key == "aspect_preset") { graphics_extra.aspect = static_cast<rocket::graphics::AspectPreset>(std::clamp(std::stoi(value), 0, 5)); aspect_preset_loaded = true; }
            else if (key == "aspect_custom") graphics_extra.custom_aspect = std::stof(value);
            else if (key == "texture_filtering") graphics_extra.texture_filtering = static_cast<rocket::graphics::TextureFiltering>(std::clamp(std::stoi(value), 0, 2));
            else if (key == "three_point_filtering") graphics_extra.three_point_filtering = std::stoi(value) != 0;
            else if (key == "texture_scaling_2d") graphics_extra.texture_scaling_2d = static_cast<rocket::graphics::TextureScaling2D>(std::clamp(std::stoi(value), 0, 2));
            else if (key == "framebuffer_precision") graphics_extra.framebuffer_precision = static_cast<rocket::graphics::FramebufferPrecision>(std::clamp(std::stoi(value), 0, 2));
            else if (key == "display_buffering") graphics_extra.display_buffering = static_cast<rocket::graphics::DisplayBuffering>(std::clamp(std::stoi(value), 0, 1));
            else if (key == "fullscreen_style") { /* legacy v9-preview setting; RT64 uses borderless fullscreen */ }
            else if (key == "hardware_resolve") graphics_extra.hardware_resolve = static_cast<rocket::graphics::HardwareResolve>(std::clamp(std::stoi(value), 0, 2));
            else if (key == "anisotropy") graphics_extra.anisotropy = std::stoi(value);
            else if (key == "mip_lod_bias") graphics_extra.mip_lod_bias = std::stof(value);
            else if (key == "fov_offset") graphics_extra.fov_offset_degrees = std::stof(value);
            else if (key == "preserve_cutscene_fov") graphics_extra.preserve_cutscene_fov = std::stoi(value) != 0;
            else if (key == "cutscene_original_aspect") graphics_extra.original_aspect_cutscenes = std::stoi(value) != 0;
            else if (key == "screen_shake_strength") graphics_extra.screen_shake_strength = std::stof(value);
            else if (key == "draw_distance") graphics_extra.draw_distance_multiplier = std::stof(value);
            else if (key == "maximum_detail") graphics_extra.maximum_detail = std::stoi(value) != 0;
            else if (key == "widescreen_culling") graphics_extra.widescreen_culling = std::stoi(value) != 0;
            else if (key == "fog_distance") graphics_extra.fog_distance_multiplier = std::stof(value);
            else if (key == "hud_aspect") graphics_extra.hud_aspect = static_cast<rocket::graphics::HudAspectMode>(std::clamp(std::stoi(value), 0, 2));
            else if (key == "hud_scale") graphics_extra.hud_scale_percent = std::stof(value);
            else if (key == "hud_safe_margin") graphics_extra.hud_safe_margin_percent = std::stof(value);
            else if (key == "vsync") graphics_extra.vsync = std::stoi(value) != 0;
            else if (key == "vi_filter") graphics_extra.vi_filter = static_cast<rocket::graphics::ViFilterMode>(std::clamp(std::stoi(value), 0, 1));
            else if (key == "texture_deband") graphics_extra.texture_deband = std::stoi(value) != 0;
            else if (key == "texture_deband_strength") graphics_extra.texture_deband_strength = std::stof(value);
            else if (key == "z_fighting") graphics_extra.z_fighting = static_cast<rocket::graphics::ZFightingMode>(std::clamp(std::stoi(value), 0, 2));
            else if (key == "post_process") graphics_extra.post_process = static_cast<rocket::graphics::PostProcessMode>(std::clamp(std::stoi(value), 0, 3));
            else if (key == "post_process_strength") graphics_extra.post_process_strength = std::stof(value);
            else if (key == "custom_shader") graphics_extra.custom_shader = value;
            else if (key == "performance_overlay") graphics_extra.performance_overlay = std::stoi(value) != 0;
            else if (key == "interpolation_overlay") graphics_extra.interpolation_overlay = std::stoi(value) != 0;
            else if (key == "volume") volume = std::clamp(std::stof(value), 0.0F, 1.0F);
            else if (key == "rumble") rocket::platform::set_rumble_enabled(std::stoi(value) != 0);
            else if (key == "rumble_strength") rocket::platform::set_rumble_strength(std::stof(value));
            else if (key == "controller_device") rocket::platform::set_preferred_controller_key(value);
            else if (key == "background_input") rocket::input::set_background_input_enabled(std::stoi(value) != 0);
            else if (key == "stick_deadzone") rocket::input::set_stick_deadzone(std::stof(value));
            else if (key == "stick_anti_deadzone") rocket::input::set_stick_anti_deadzone(std::stof(value));
            else if (key == "stick_sensitivity") rocket::input::set_stick_sensitivity(std::stof(value));
            else if (key == "stick_curve") rocket::input::set_stick_curve(std::stof(value));
            else if (key == "stick_x_inverted") rocket::input::set_stick_x_inverted(std::stoi(value) != 0);
            else if (key == "stick_y_inverted") rocket::input::set_stick_y_inverted(std::stoi(value) != 0);
            else if (key == "trigger_threshold") rocket::input::set_trigger_threshold(std::stof(value));
            else if (key == "shortcut.overlay.keyboard") rocket::input::set_shortcut_keyboard_binding(rocket::input::ShortcutAction::ToggleOverlay, std::stoi(value));
            else if (key == "shortcut.overlay.controller") rocket::input::set_shortcut_controller_binding(rocket::input::ShortcutAction::ToggleOverlay, std::stoi(value));
            else if (key == "shortcut.fullscreen.keyboard") rocket::input::set_shortcut_keyboard_binding(rocket::input::ShortcutAction::ToggleFullscreen, std::stoi(value));
            else if (key == "shortcut.fullscreen.controller") rocket::input::set_shortcut_controller_binding(rocket::input::ShortcutAction::ToggleFullscreen, std::stoi(value));
            else if (key == "audio_profile") audio_profile = std::max(std::stoi(value), 0);
        } catch (...) {}
    }
    // Early Rocket-R builds defaulted the unqualified raw PCM path to 100%.
    // Preserve the one-time normalized audio-volume migration introduced before FIXED24. Once
    // audio_profile=2 has been saved, the user may deliberately choose any
    // value up to 100% without another automatic cap.
    if (audio_profile < 2) {
        volume = std::min(volume, 0.65F);
    }
    if (!aspect_preset_loaded) {
        if (graphics.ar_option == ultramodern::renderer::AspectRatio::Expand)
            graphics_extra.aspect = rocket::graphics::AspectPreset::FitWindow;
        else if (graphics.ar_option == ultramodern::renderer::AspectRatio::Manual)
            graphics_extra.aspect = rocket::graphics::AspectPreset::Ratio16x9;
    }
    switch (graphics_extra.framebuffer_precision) {
        case rocket::graphics::FramebufferPrecision::High: graphics.hpfb_option = ultramodern::renderer::HighPrecisionFramebuffer::On; break;
        case rocket::graphics::FramebufferPrecision::Original: graphics.hpfb_option = ultramodern::renderer::HighPrecisionFramebuffer::Off; break;
        default: graphics.hpfb_option = ultramodern::renderer::HighPrecisionFramebuffer::Auto; break;
    }
    switch (graphics_extra.aspect) {
        case rocket::graphics::AspectPreset::Original4x3: graphics.ar_option = ultramodern::renderer::AspectRatio::Original; break;
        case rocket::graphics::AspectPreset::FitWindow: graphics.ar_option = ultramodern::renderer::AspectRatio::Expand; break;
        default: graphics.ar_option = ultramodern::renderer::AspectRatio::Manual; break;
    }
    rocket::graphics::set_settings(graphics_extra, false);
    ultramodern::renderer::set_graphics_config(graphics);
    g_n64_dithering_enabled.store(n64_dithering, std::memory_order_relaxed);
    rocket::platform::set_master_volume(volume);
}

void SaveLastRom(const std::filesystem::path& path) {
    std::ofstream out(LastRomPath(), std::ios::trunc);
    if (out) out << PathUtf8(path);
}

std::filesystem::path LoadLastRom() {
    std::ifstream in(LastRomPath());
    std::string line;
    if (std::getline(in, line) && !line.empty()) return std::filesystem::u8path(line);
    return {};
}

bool FeedGamepadNavigationEvent(const SDL_Event& event) {
    if (event.type != SDL_CONTROLLERBUTTONDOWN &&
        event.type != SDL_CONTROLLERBUTTONUP) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    const bool down = event.type == SDL_CONTROLLERBUTTONDOWN;
    ImGuiKey key = ImGuiKey_None;
    switch (event.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_A: key = ImGuiKey_GamepadFaceDown; break;
        case SDL_CONTROLLER_BUTTON_B: key = ImGuiKey_GamepadFaceRight; break;
        case SDL_CONTROLLER_BUTTON_X: key = ImGuiKey_GamepadFaceLeft; break;
        case SDL_CONTROLLER_BUTTON_Y: key = ImGuiKey_GamepadFaceUp; break;
        case SDL_CONTROLLER_BUTTON_BACK: key = ImGuiKey_GamepadBack; break;
        case SDL_CONTROLLER_BUTTON_START: key = ImGuiKey_GamepadStart; break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: key = ImGuiKey_GamepadL1; break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: key = ImGuiKey_GamepadR1; break;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: key = ImGuiKey_GamepadL3; break;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: key = ImGuiKey_GamepadR3; break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: key = ImGuiKey_GamepadDpadUp; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: key = ImGuiKey_GamepadDpadDown; break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: key = ImGuiKey_GamepadDpadLeft; break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: key = ImGuiKey_GamepadDpadRight; break;
        default: break;
    }
    if (key == ImGuiKey_None) return false;
    io.AddKeyEvent(key, down);
    return true;
}

std::filesystem::path BrowseForRom() {
#if defined(_WIN32)
    wchar_t file_name[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = static_cast<HWND>(nullptr);
    dialog.lpstrFilter = L"Nintendo 64 ROMs (*.z64;*.n64;*.v64)\0*.z64;*.n64;*.v64\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = file_name;
    dialog.nMaxFile = static_cast<DWORD>(sizeof(file_name) / sizeof(file_name[0]));
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    dialog.lpstrTitle = L"Select your Rocket: Robot on Wheels US ROM";
    if (GetOpenFileNameW(&dialog) != FALSE) return std::filesystem::path(file_name);
#endif
    return {};
}

const char* GraphicsApiName(ultramodern::renderer::GraphicsApi api) {
    using A = ultramodern::renderer::GraphicsApi;
    switch (api) {
        case A::D3D12: return "Direct3D 12";
        case A::Vulkan: return "Vulkan";
        case A::Metal: return "Metal";
        default: return "Automatic";
    }
}

const char* AspectName(ultramodern::renderer::AspectRatio value) {
    using A = ultramodern::renderer::AspectRatio;
    switch (value) {
        case A::Expand: return "Expand to window";
        case A::Manual: return "Manual";
        default: return "Original 4:3";
    }
}

const char* ResolutionName(ultramodern::renderer::Resolution value) {
    using R = ultramodern::renderer::Resolution;
    switch (value) {
        case R::Original: return "Original";
        case R::Original2x: return "Original 2x";
        default: return "Window integer scale";
    }
}

const char* MsaaName(ultramodern::renderer::Antialiasing value) {
    using A = ultramodern::renderer::Antialiasing;
    switch (value) {
        case A::MSAA2X: return "MSAA 2x";
        case A::MSAA4X: return "MSAA 4x";
        case A::MSAA8X: return "MSAA 8x";
        default: return "None";
    }
}

const char* RefreshName(ultramodern::renderer::RefreshRate value) {
    using R = ultramodern::renderer::RefreshRate;
    switch (value) {
        case R::Display: return "Match display";
        case R::Manual: return "Custom";
        default: return "Original 30 FPS";
    }
}

const char* GraphicsPresetName(rocket::graphics::GraphicsPreset value) {
    using P = rocket::graphics::GraphicsPreset;
    switch (value) {
        case P::Modern: return "Modern";
        case P::HighQuality: return "High quality";
        case P::Performance: return "Performance";
        case P::Custom: return "Custom";
        default: return "Original";
    }
}

const char* AspectPresetName(rocket::graphics::AspectPreset value) {
    using A = rocket::graphics::AspectPreset;
    switch (value) {
        case A::FitWindow: return "Fit window";
        case A::Ratio16x9: return "16:9";
        case A::Ratio16x10: return "16:10";
        case A::Ratio21x9: return "21:9";
        case A::Custom: return "Custom";
        default: return "Original 4:3";
    }
}

const char* TextureFilteringName(rocket::graphics::TextureFiltering value) {
    using F = rocket::graphics::TextureFiltering;
    switch (value) {
        case F::Nearest: return "Nearest";
        case F::AntiAliasedPixelScaling: return "Anti-aliased pixel scaling";
        default: return "Linear";
    }
}

const char* TextureScalingName(rocket::graphics::TextureScaling2D value) {
    using U = rocket::graphics::TextureScaling2D;
    switch (value) {
        case U::ScaledOnly: return "Scaled 2D only";
        case U::All: return "All 2D";
        default: return "Original";
    }
}

const char* FramebufferPrecisionName(rocket::graphics::FramebufferPrecision value) {
    using F = rocket::graphics::FramebufferPrecision;
    switch (value) {
        case F::Original: return "Original / standard";
        case F::High: return "High precision";
        default: return "Automatic";
    }
}

const char* DisplayBufferingName(rocket::graphics::DisplayBuffering value) {
    return value == rocket::graphics::DisplayBuffering::Double ? "Double" : "Triple";
}

const char* HardwareResolveName(rocket::graphics::HardwareResolve value) {
    using H = rocket::graphics::HardwareResolve;
    switch (value) {
        case H::Off: return "Disabled";
        case H::On: return "Enabled";
        default: return "Automatic";
    }
}

const char* HudAspectName(rocket::graphics::HudAspectMode value) {
    using H = rocket::graphics::HudAspectMode;
    switch (value) {
        case H::SafeArea16x9: return "16:9 safe area";
        case H::FitWindow: return "Fit window";
        default: return "Original 4:3";
    }
}

const char* ZFightingName(rocket::graphics::ZFightingMode value) {
    using Z = rocket::graphics::ZFightingMode;
    switch (value) {
        case Z::Consistent: return "Consistent decals";
        case Z::Strong: return "Strong / no vanishing decals";
        default: return "Original";
    }
}

const char* PostProcessName(rocket::graphics::PostProcessMode value) {
    using P = rocket::graphics::PostProcessMode;
    switch (value) {
        case P::Scanlines: return "Scanlines";
        case P::CRT: return "CRT";
        case P::Custom: return "Custom shader";
        default: return "Off";
    }
}

void SyncExtraGraphicsConfig(const rocket::graphics::Settings& extra,
                             ultramodern::renderer::GraphicsConfig& config) {
    using AP = rocket::graphics::AspectPreset;
    config.ar_option = extra.aspect == AP::Original4x3
        ? ultramodern::renderer::AspectRatio::Original
        : extra.aspect == AP::FitWindow
            ? ultramodern::renderer::AspectRatio::Expand
            : ultramodern::renderer::AspectRatio::Manual;
    switch (extra.framebuffer_precision) {
        case rocket::graphics::FramebufferPrecision::High:
            config.hpfb_option = ultramodern::renderer::HighPrecisionFramebuffer::On;
            break;
        case rocket::graphics::FramebufferPrecision::Original:
            config.hpfb_option = ultramodern::renderer::HighPrecisionFramebuffer::Off;
            break;
        default:
            config.hpfb_option = ultramodern::renderer::HighPrecisionFramebuffer::Auto;
            break;
    }
}

std::vector<std::string> CustomShaderStems() {
    std::set<std::string> stems;
    std::error_code ec;
    const auto directory = rocket::graphics::shader_directory();
    if (!std::filesystem::is_directory(directory, ec)) return {};
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec || !entry.is_regular_file(ec)) continue;
        const auto ext = entry.path().extension().string();
        if (ext != ".dxil" && ext != ".spv") continue;
        stems.insert(entry.path().stem().string());
    }
    return {stems.begin(), stems.end()};
}

void GraphicsSectionButton(const char* label, int index) {
    if (g_graphics_section == index) ImGui::PushStyleColor(ImGuiCol_Button, kWarm);
    if (ImGui::Button(label, {0.0F, 42.0F})) g_graphics_section = index;
    if (g_graphics_section == index) ImGui::PopStyleColor();
}

void DrawGraphicsPage(float width, bool in_game) {
    auto config = ultramodern::renderer::get_graphics_config();
    auto extra = rocket::graphics::settings();
    bool config_changed = false;
    bool extra_changed = false;
    bool keep_named_preset = false;

    LauncherHeading("GRAPHICS");
    ImGui::TextDisabled("Rocket-R rendering, presentation and visual enhancement settings");
    ImGui::Separator();

    ImGui::TextUnformatted("Preset");
    if (ImGui::BeginCombo("##graphics-preset", GraphicsPresetName(extra.preset))) {
        const std::array<rocket::graphics::GraphicsPreset, 4> values{
            rocket::graphics::GraphicsPreset::Original,
            rocket::graphics::GraphicsPreset::Modern,
            rocket::graphics::GraphicsPreset::HighQuality,
            rocket::graphics::GraphicsPreset::Performance};
        for (auto value : values) {
            if (ImGui::Selectable(GraphicsPresetName(value), extra.preset == value)) {
                rocket::graphics::apply_preset(value);
                extra = rocket::graphics::settings();
                SyncExtraGraphicsConfig(extra, config);
                config_changed = true;
                extra_changed = true;
                keep_named_preset = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("Changing any individual enhancement switches the preset to Custom.");
    ImGui::Spacing();

    GraphicsSectionButton("DISPLAY", 0); ImGui::SameLine();
    GraphicsSectionButton("IMAGE", 1); ImGui::SameLine();
    GraphicsSectionButton("WORLD / CAMERA", 2); ImGui::SameLine();
    GraphicsSectionButton("HUD", 3); ImGui::SameLine();
    GraphicsSectionButton("DIAGNOSTICS", 4);
    ImGui::Separator();

    const float control_width = std::min(width, 560.0F);
    if (g_graphics_section == 0) {
        ImGui::TextUnformatted("Graphics API");
        if (in_game) {
            ImGui::TextDisabled("%s - restart from the launcher to change the renderer backend.",
                                GraphicsApiName(config.api_option));
        } else if (ImGui::BeginCombo("##graphics-api", GraphicsApiName(config.api_option))) {
            std::array<ultramodern::renderer::GraphicsApi, 4> values{
                ultramodern::renderer::GraphicsApi::Auto,
                ultramodern::renderer::GraphicsApi::D3D12,
                ultramodern::renderer::GraphicsApi::Vulkan,
                ultramodern::renderer::GraphicsApi::Metal};
            for (auto value : values) {
#if !defined(__APPLE__)
                if (value == ultramodern::renderer::GraphicsApi::Metal) continue;
#endif
#if !defined(_WIN32)
                if (value == ultramodern::renderer::GraphicsApi::D3D12) continue;
#endif
                if (ImGui::Selectable(GraphicsApiName(value), config.api_option == value)) {
                    config.api_option = value; config_changed = true;
                }
            }
            ImGui::EndCombo();
        }

        ImGui::TextUnformatted("Window mode");
        bool fullscreen = config.wm_option == ultramodern::renderer::WindowMode::Fullscreen;
        if (ImGui::BeginCombo("##window-mode", fullscreen ? "Borderless fullscreen" : "Windowed")) {
            if (ImGui::Selectable("Windowed", !fullscreen)) {
                config.wm_option = ultramodern::renderer::WindowMode::Windowed; config_changed = true;
            }
            if (ImGui::Selectable("Borderless fullscreen", fullscreen)) {
                config.wm_option = ultramodern::renderer::WindowMode::Fullscreen; config_changed = true;
            }
            ImGui::EndCombo();
        }
        if (ImGui::Checkbox("VSync", &extra.vsync)) extra_changed = true;
        ImGui::TextDisabled("Fullscreen uses RT64's cross-platform borderless mode; F11 or Alt+Enter toggles it at any time.");

        ImGui::TextUnformatted("Aspect ratio");
        if (ImGui::BeginCombo("##aspect-preset", AspectPresetName(extra.aspect))) {
            const std::array<rocket::graphics::AspectPreset, 6> values{
                rocket::graphics::AspectPreset::Original4x3,
                rocket::graphics::AspectPreset::FitWindow,
                rocket::graphics::AspectPreset::Ratio16x9,
                rocket::graphics::AspectPreset::Ratio16x10,
                rocket::graphics::AspectPreset::Ratio21x9,
                rocket::graphics::AspectPreset::Custom};
            for (auto value : values) {
                if (ImGui::Selectable(AspectPresetName(value), extra.aspect == value)) {
                    extra.aspect = value; extra_changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (extra.aspect == rocket::graphics::AspectPreset::Custom) {
            ImGui::SetNextItemWidth(control_width);
            if (ImGui::SliderFloat("##aspect-custom", &extra.custom_aspect, 1.0F, 3.5F, "%.3f:1")) extra_changed = true;
        }

        ImGui::TextUnformatted("Resolution");
        if (ImGui::BeginCombo("##resolution", ResolutionName(config.res_option))) {
            const std::array<ultramodern::renderer::Resolution, 3> values{
                ultramodern::renderer::Resolution::Auto,
                ultramodern::renderer::Resolution::Original2x,
                ultramodern::renderer::Resolution::Original};
            for (auto value : values) {
                if (ImGui::Selectable(ResolutionName(value), config.res_option == value)) {
                    config.res_option = value; config_changed = true;
                }
            }
            ImGui::EndCombo();
        }
        int scale = std::clamp(config.ds_option, 1, 4);
        ImGui::TextUnformatted("Internal scale / downsample factor");
        ImGui::SetNextItemWidth(control_width);
        if (ImGui::SliderInt("##internal-scale", &scale, 1, 4, "%dx")) {
            config.ds_option = scale; config_changed = true;
        }

        ImGui::TextUnformatted("Frame rate");
        if (ImGui::BeginCombo("##refresh-rate", RefreshName(config.rr_option))) {
            const std::array<ultramodern::renderer::RefreshRate, 3> values{
                ultramodern::renderer::RefreshRate::Original,
                ultramodern::renderer::RefreshRate::Display,
                ultramodern::renderer::RefreshRate::Manual};
            for (auto value : values) {
                if (ImGui::Selectable(RefreshName(value), config.rr_option == value)) {
                    config.rr_option = value; config_changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (config.rr_option == ultramodern::renderer::RefreshRate::Manual) {
            int manual_rate = std::clamp(config.rr_manual_value, 30, 500);
            ImGui::SetNextItemWidth(control_width);
            if (ImGui::SliderInt("##refresh-manual", &manual_rate, 30, 500, "%d FPS")) {
                config.rr_manual_value = manual_rate; config_changed = true;
            }
        }
        ImGui::TextDisabled(config.rr_option == ultramodern::renderer::RefreshRate::Original
            ? "Retail 30 FPS presentation; interpolation is disabled."
            : "Presentation-only interpolation; simulation, input and audio remain on Rocket's retail timing.");

        ImGui::TextUnformatted("Display buffering");
        if (ImGui::BeginCombo("##buffering", DisplayBufferingName(extra.display_buffering))) {
            for (auto value : {rocket::graphics::DisplayBuffering::Double,
                               rocket::graphics::DisplayBuffering::Triple}) {
                if (ImGui::Selectable(DisplayBufferingName(value), extra.display_buffering == value)) {
                    extra.display_buffering = value; extra_changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (in_game) ImGui::TextDisabled("Display buffering changes are applied on the next game start.");

        ImGui::TextUnformatted("Framebuffer precision");
        if (ImGui::BeginCombo("##fb-precision", FramebufferPrecisionName(extra.framebuffer_precision))) {
            for (auto value : {rocket::graphics::FramebufferPrecision::Automatic,
                               rocket::graphics::FramebufferPrecision::Original,
                               rocket::graphics::FramebufferPrecision::High}) {
                if (ImGui::Selectable(FramebufferPrecisionName(value), extra.framebuffer_precision == value)) {
                    extra.framebuffer_precision = value; extra_changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (in_game) ImGui::TextDisabled("Framebuffer precision is selected when the renderer starts.");

        ImGui::TextUnformatted("Hardware resolve");
        if (ImGui::BeginCombo("##hardware-resolve", HardwareResolveName(extra.hardware_resolve))) {
            for (auto value : {rocket::graphics::HardwareResolve::Automatic,
                               rocket::graphics::HardwareResolve::Off,
                               rocket::graphics::HardwareResolve::On}) {
                if (ImGui::Selectable(HardwareResolveName(value), extra.hardware_resolve == value)) {
                    extra.hardware_resolve = value; extra_changed = true;
                }
            }
            ImGui::EndCombo();
        }
    } else if (g_graphics_section == 1) {
        ImGui::TextUnformatted("Anti-aliasing");
        if (ImGui::BeginCombo("##msaa", MsaaName(config.msaa_option))) {
            const std::array<ultramodern::renderer::Antialiasing, 4> values{
                ultramodern::renderer::Antialiasing::None,
                ultramodern::renderer::Antialiasing::MSAA2X,
                ultramodern::renderer::Antialiasing::MSAA4X,
                ultramodern::renderer::Antialiasing::MSAA8X};
            for (auto value : values) {
                if (ImGui::Selectable(MsaaName(value), config.msaa_option == value)) {
                    config.msaa_option = value; config_changed = true;
                }
            }
            ImGui::EndCombo();
        }

        ImGui::TextUnformatted("Output scaling filter");
        if (ImGui::BeginCombo("##texture-filter", TextureFilteringName(extra.texture_filtering))) {
            for (auto value : {rocket::graphics::TextureFiltering::Nearest,
                               rocket::graphics::TextureFiltering::Linear,
                               rocket::graphics::TextureFiltering::AntiAliasedPixelScaling}) {
                if (ImGui::Selectable(TextureFilteringName(value), extra.texture_filtering == value)) {
                    extra.texture_filtering = value; extra_changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::Checkbox("N64 three-point texture filtering", &extra.three_point_filtering)) extra_changed = true;

        ImGui::TextUnformatted("2D scaling policy");
        if (ImGui::BeginCombo("##2d-scaling", TextureScalingName(extra.texture_scaling_2d))) {
            for (auto value : {rocket::graphics::TextureScaling2D::Original,
                               rocket::graphics::TextureScaling2D::ScaledOnly,
                               rocket::graphics::TextureScaling2D::All}) {
                if (ImGui::Selectable(TextureScalingName(value), extra.texture_scaling_2d == value)) {
                    extra.texture_scaling_2d = value; extra_changed = true;
                }
            }
            ImGui::EndCombo();
        }

        ImGui::TextUnformatted("Anisotropic filtering");
        ImGui::SetNextItemWidth(control_width);
        if (ImGui::SliderInt("##anisotropy", &extra.anisotropy, 1, 16, "%dx")) extra_changed = true;
        if (in_game) ImGui::TextDisabled("Anisotropy is baked into RT64 sampler objects and applies on the next game start.");

        ImGui::TextUnformatted("Texture mip LOD bias");
        ImGui::SetNextItemWidth(control_width);
        if (ImGui::SliderFloat("##mip-bias", &extra.mip_lod_bias, -2.0F, 2.0F, "%+.2f")) extra_changed = true;
        ImGui::TextDisabled("Negative values sharpen mip selection; positive values favour lower-detail mips.");

        bool n64_dithering = g_n64_dithering_enabled.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("N64 colour dithering", &n64_dithering)) {
            g_n64_dithering_enabled.store(n64_dithering, std::memory_order_relaxed);
            config_changed = true;
        }
        ImGui::TextDisabled("Controls Rocket's original 4x4 Bayer RDP colour dithering, including the skybox pattern you identified.");

        bool clean_vi = extra.vi_filter == rocket::graphics::ViFilterMode::Clean;
        if (ImGui::Checkbox("Clean VI output", &clean_vi)) {
            extra.vi_filter = clean_vi ? rocket::graphics::ViFilterMode::Clean
                                       : rocket::graphics::ViFilterMode::Authentic;
            extra_changed = true;
        }
        ImGui::TextDisabled("Original keeps a light N64-style VI smoothing pass; Clean presents the rendered image without it.");

        if (ImGui::Checkbox("Deband smooth gradients", &extra.texture_deband)) extra_changed = true;
        if (extra.texture_deband) {
            ImGui::SetNextItemWidth(control_width);
            if (ImGui::SliderFloat("##deband-strength", &extra.texture_deband_strength, 0.0F, 100.0F, "%.0f%%")) extra_changed = true;
        }

        ImGui::TextUnformatted("Z-fighting reduction");
        if (ImGui::BeginCombo("##zfighting", ZFightingName(extra.z_fighting))) {
            for (auto value : {rocket::graphics::ZFightingMode::Original,
                               rocket::graphics::ZFightingMode::Consistent,
                               rocket::graphics::ZFightingMode::Strong}) {
                if (ImGui::Selectable(ZFightingName(value), extra.z_fighting == value)) {
                    extra.z_fighting = value; extra_changed = true;
                }
            }
            ImGui::EndCombo();
        }

        ImGui::TextUnformatted("Post-process shader");
        if (ImGui::BeginCombo("##post-process", PostProcessName(extra.post_process))) {
            for (auto value : {rocket::graphics::PostProcessMode::Off,
                               rocket::graphics::PostProcessMode::Scanlines,
                               rocket::graphics::PostProcessMode::CRT,
                               rocket::graphics::PostProcessMode::Custom}) {
                if (ImGui::Selectable(PostProcessName(value), extra.post_process == value)) {
                    extra.post_process = value; extra_changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (extra.post_process != rocket::graphics::PostProcessMode::Off) {
            ImGui::SetNextItemWidth(control_width);
            if (ImGui::SliderFloat("##post-strength", &extra.post_process_strength, 0.0F, 100.0F, "%.0f%%")) extra_changed = true;
        }
        if (extra.post_process == rocket::graphics::PostProcessMode::Custom) {
            const auto stems = CustomShaderStems();
            const char* preview = extra.custom_shader.empty() ? "No shader selected" : extra.custom_shader.c_str();
            if (ImGui::BeginCombo("##custom-shader", preview)) {
                for (const auto& stem : stems) {
                    if (ImGui::Selectable(stem.c_str(), extra.custom_shader == stem)) {
                        extra.custom_shader = stem; extra_changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            if (stems.empty()) {
                ImGui::TextDisabled("Place matching <name>.dxil and/or <name>.spv binaries in the Rocket-R config/shaders folder.");
            }
            ImGui::TextDisabled("Custom shader binaries are loaded when RT64 starts; restart the game after changing the selection.");
        }
    } else if (g_graphics_section == 2) {
        ImGui::TextUnformatted("Field of view offset");
        ImGui::SetNextItemWidth(control_width);
        if (ImGui::SliderFloat("##fov-offset", &extra.fov_offset_degrees, -20.0F, 40.0F, "%+.1f deg")) extra_changed = true;
        if (ImGui::Checkbox("Preserve authored cutscene FOV", &extra.preserve_cutscene_fov)) extra_changed = true;
        if (ImGui::Checkbox("Use original 4:3 framing during detected cutscenes", &extra.original_aspect_cutscenes)) extra_changed = true;
        ImGui::TextDisabled("Cutscene protection is conservative: authored FOV changes/camera cuts are left alone rather than being forced to gameplay FOV.");

        ImGui::TextUnformatted("Screen shake strength");
        ImGui::SetNextItemWidth(control_width);
        if (ImGui::SliderFloat("##screen-shake", &extra.screen_shake_strength, 0.0F, 100.0F, "%.0f%%")) extra_changed = true;
        ImGui::TextDisabled("100%% is retail. Lower values attenuate only small high-frequency camera residuals; major camera movement and cuts pass through.");

        ImGui::TextUnformatted("Draw distance");
        ImGui::SetNextItemWidth(control_width);
        if (ImGui::SliderFloat("##draw-distance", &extra.draw_distance_multiplier, 0.5F, 8.0F, "%.2fx")) extra_changed = true;
        if (ImGui::Checkbox("Maximum visibility (disable distance culling)", &extra.maximum_detail)) extra_changed = true;
        ImGui::TextDisabled("Rocket's current decomp exposes object distance culling here; no separate model-mesh LOD selector has been identified, so this option does not invent one.");
        if (ImGui::Checkbox("Widescreen culling fix", &extra.widescreen_culling)) extra_changed = true;
        ImGui::TextDisabled("The culling fix widens only Rocket's horizontal object-frustum planes and keeps the existing fail-closed edge guard.");

        ImGui::TextUnformatted("Fog distance");
        ImGui::SetNextItemWidth(control_width);
        if (ImGui::SliderFloat("##fog-distance", &extra.fog_distance_multiplier, 0.0F, 4.0F,
                               extra.fog_distance_multiplier < 0.01F ? "Off" : "%.2fx")) extra_changed = true;
        ImGui::TextDisabled("0 disables decoded RSP fog; 1.0 keeps the authored fog response; higher values push the fog response outward.");
    } else if (g_graphics_section == 3) {
        ImGui::TextUnformatted("HUD aspect policy");
        if (ImGui::BeginCombo("##hud-aspect", HudAspectName(extra.hud_aspect))) {
            for (auto value : {rocket::graphics::HudAspectMode::Original4x3,
                               rocket::graphics::HudAspectMode::SafeArea16x9,
                               rocket::graphics::HudAspectMode::FitWindow}) {
                if (ImGui::Selectable(HudAspectName(value), extra.hud_aspect == value)) {
                    extra.hud_aspect = value; extra_changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TextUnformatted("HUD scale");
        ImGui::SetNextItemWidth(control_width);
        if (ImGui::SliderFloat("##hud-scale", &extra.hud_scale_percent, 50.0F, 150.0F, "%.0f%%")) extra_changed = true;
        ImGui::TextUnformatted("HUD safe-area margin");
        ImGui::SetNextItemWidth(control_width);
        if (ImGui::SliderFloat("##hud-safe", &extra.hud_safe_margin_percent, 0.0F, 20.0F, "%.1f%%")) extra_changed = true;
        ImGui::TextDisabled("HUD changes affect 2D/orthographic presentation only; perspective world geometry is not rescaled.");
    } else {
        if (ImGui::Checkbox("Performance overlay", &extra.performance_overlay)) extra_changed = true;
        if (ImGui::Checkbox("Interpolation coverage overlay", &extra.interpolation_overlay)) extra_changed = true;
        const auto perf = rocket::graphics::performance_stats();
        const auto coverage = rocket::presentation::coverage_stats();
        ImGui::Separator();
        ImGui::Text("Presentation: %.1f FPS   %.2f ms", perf.fps, perf.frame_ms);
        ImGui::Text("Display / target: %d / %d Hz", perf.display_rate, perf.target_rate);
        ImGui::Text("Resolution scale: %.2fx", perf.resolution_scale);
        ImGui::Text("Presents: %llu   interpolated: %llu",
                    static_cast<unsigned long long>(perf.presents),
                    static_cast<unsigned long long>(perf.interpolated_presents));
        ImGui::Text("Semantic interpolation bindings: %llu",
                    static_cast<unsigned long long>(coverage.semantic_bindings));
        ImGui::Text("Fail-closed snapped bindings: %llu",
                    static_cast<unsigned long long>(coverage.snapped_bindings));
        ImGui::Text("Dynamic-vertex bindings: %llu",
                    static_cast<unsigned long long>(coverage.dynamic_vertex_bindings));
        ImGui::Text("Sidecar mismatches: %llu",
                    static_cast<unsigned long long>(coverage.sidecar_mismatches));
        ImGui::TextDisabled("A snapped binding is intentional when Rocket-R cannot prove a safe correspondence. It is never handed back to anonymous RT64 matching.");

        ImGui::Spacing();
        if (ImGui::Button("RESET GRAPHICS TO ORIGINAL", {330.0F, 54.0F})) {
            rocket::graphics::reset_settings();
            extra = rocket::graphics::settings();
            config.res_option = ultramodern::renderer::Resolution::Auto;
            config.ar_option = ultramodern::renderer::AspectRatio::Original;
            config.msaa_option = ultramodern::renderer::Antialiasing::None;
            config.rr_option = ultramodern::renderer::RefreshRate::Original;
            config.rr_manual_value = 60;
            config.ds_option = 1;
            config.hpfb_option = ultramodern::renderer::HighPrecisionFramebuffer::Auto;
            g_n64_dithering_enabled.store(true, std::memory_order_relaxed);
            config_changed = true;
            extra_changed = true;
            keep_named_preset = true;
        }
    }

    if (extra_changed) {
        SyncExtraGraphicsConfig(extra, config);
        rocket::graphics::set_settings(extra, !keep_named_preset);
        extra = rocket::graphics::settings();
        // Aspect/culling settings are also consumed by the guest-thread frustum
        // guard. Republish them immediately instead of waiting for a resize event.
        rocket::widescreen::update_window_aspect(rocket::platform::sdl_window());
        config_changed = true;
    }
    if (config_changed) {
        ultramodern::renderer::set_graphics_config(config);
        SaveSettings();
    }
}

void DrawSoundPage(float width) {
    LauncherHeading("SOUND");
    ImGui::TextDisabled("Host output controls; Rocket's original mixer remains unchanged");
    ImGui::Separator();
    float volume = rocket::platform::master_volume() * 100.0F;
    ImGui::TextUnformatted("Master volume");
    ImGui::TextDisabled("65%% is the normalized default; 100%% exposes the full game PCM level.");
    ImGui::SetNextItemWidth(std::min(width, 520.0F));
    if (ImGui::SliderFloat("##volume", &volume, 0.0F, 100.0F, "%.0f%%")) {
        rocket::platform::set_master_volume(volume / 100.0F);
        SaveSettings();
    }
}

std::string CaptureBindingName(CaptureDevice device, int source) {
    return device == CaptureDevice::Keyboard
        ? rocket::input::keyboard_binding_name(source)
        : rocket::input::controller_binding_name(source);
}

void BeginBindingCapture(rocket::input::Action action,
                         rocket::input::BindingSlot slot) {
    g_capture_action = static_cast<int>(action);
    g_capture_slot = slot;
    g_capture_shortcut = false;
    g_capture_device = (slot == rocket::input::BindingSlot::KeyboardPrimary ||
                        slot == rocket::input::BindingSlot::KeyboardSecondary)
        ? CaptureDevice::Keyboard : CaptureDevice::Controller;
    g_capture_popup_pending = true;
    g_capture_finished = false;
    g_capture_started = std::chrono::steady_clock::now();
}

void BeginShortcutCapture(CaptureDevice device,
                          rocket::input::ShortcutAction action) {
    g_capture_action = -1;
    g_capture_shortcut = true;
    g_capture_shortcut_action = action;
    g_capture_device = device;
    g_capture_popup_pending = true;
    g_capture_finished = false;
    g_capture_started = std::chrono::steady_clock::now();
}

void CommitCapturedSource(int source) {
    if (g_capture_shortcut) {
        if (g_capture_device == CaptureDevice::Keyboard) {
            rocket::input::set_shortcut_keyboard_binding(g_capture_shortcut_action, source);
        } else {
            rocket::input::set_shortcut_controller_binding(g_capture_shortcut_action, source);
        }
    } else if (g_capture_action >= 0 &&
               g_capture_action < static_cast<int>(rocket::input::Action::Count)) {
        rocket::input::set_binding(
            static_cast<rocket::input::Action>(g_capture_action),
            g_capture_slot, source);
    }
    SaveSettings();
    g_capture_finished = true;
    g_capture_device = CaptureDevice::None;
}

bool HandleInputCaptureEvent(SDL_Event* event) {
    if (event == nullptr || g_capture_device == CaptureDevice::None) return false;
    if (event->type == SDL_KEYDOWN && event->key.repeat == 0) {
        if (event->key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
            g_capture_finished = true;
            g_capture_device = CaptureDevice::None;
            return true;
        }
        if (event->key.keysym.scancode == SDL_SCANCODE_BACKSPACE ||
            event->key.keysym.scancode == SDL_SCANCODE_DELETE) {
            CommitCapturedSource(rocket::input::kUnbound);
            return true;
        }
        if (g_capture_device == CaptureDevice::Keyboard) {
            CommitCapturedSource(static_cast<int>(event->key.keysym.scancode));
            return true;
        }
        return true;
    }
    if (g_capture_device != CaptureDevice::Controller) return false;
    if (event->type == SDL_CONTROLLERBUTTONDOWN) {
        CommitCapturedSource(rocket::input::encode_controller_button(
            static_cast<int>(event->cbutton.button)));
        return true;
    }
    if (event->type == SDL_CONTROLLERAXISMOTION &&
        std::chrono::steady_clock::now() - g_capture_started >
            std::chrono::milliseconds(150) &&
        std::abs(static_cast<int>(event->caxis.value)) >= 24000) {
        CommitCapturedSource(rocket::input::encode_controller_axis(
            static_cast<int>(event->caxis.axis), event->caxis.value > 0));
        return true;
    }
    return event->type == SDL_CONTROLLERAXISMOTION;
}

void DrawCapturePopup() {
    constexpr const char* kPopup = "Bind control";
    if (g_capture_popup_pending) {
        ImGui::OpenPopup(kPopup);
        g_capture_popup_pending = false;
    }
    if (!ImGui::BeginPopupModal(kPopup, nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    if (g_capture_finished) {
        g_capture_finished = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    LauncherHeading("WAITING FOR INPUT");
    ImGui::Separator();
    if (g_capture_device == CaptureDevice::Keyboard) {
        ImGui::TextWrapped("Press the keyboard key you want to use.");
    } else {
        ImGui::TextWrapped("Press a gamepad button or move an axis fully in the direction you want to use.");
    }
    ImGui::TextDisabled("Escape cancels. Backspace/Delete clears the binding.");
    ImGui::Spacing();
    if (ImGui::Button("CANCEL", {240.0F, 44.0F})) {
        g_capture_device = CaptureDevice::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void DrawControlsPage(float width) {
    using rocket::input::Action;
    using rocket::input::BindingSlot;
    LauncherHeading("CONTROLS");
    ImGui::TextDisabled("Single-player input profiles - keyboard and SDL gamepads can be used together");
    ImGui::Separator();

    constexpr std::array<const char*, 4> sections{
        "DEVICE", "N64 BINDINGS", "STICK", "SHORTCUTS"};
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float section_width = std::max((width - gap * 3.0F) / 4.0F, 1.0F);
    for (std::size_t index = 0; index < sections.size(); ++index) {
        if (index != 0U) ImGui::SameLine();
        const bool selected = g_controls_section == static_cast<int>(index);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, kWarm);
            ImGui::PushStyleColor(ImGuiCol_Text, kBackground);
        }
        if (ImGui::Button(sections[index], {section_width, 42.0F})) {
            g_controls_section = static_cast<int>(index);
        }
        if (selected) ImGui::PopStyleColor(2);
    }
    ImGui::Dummy({0.0F, 12.0F});

    if (g_controls_section == 0) {
        ImGui::SeparatorText("ACTIVE CONTROLLER");
        const auto controller_choices = rocket::platform::controller_choices();
        const std::string preferred_controller = rocket::platform::preferred_controller_key();
        std::string controller_preview = preferred_controller.empty()
            ? "Automatic (first connected)"
            : rocket::platform::controller_name();
        ImGui::TextUnformatted("Player 1 gamepad");
        ImGui::SetNextItemWidth(std::min(width, 620.0F));
        if (ImGui::BeginCombo("##player1-controller", controller_preview.c_str())) {
            const bool automatic = preferred_controller.empty();
            if (ImGui::Selectable("Automatic (first connected)", automatic)) {
                rocket::platform::set_preferred_controller_key({});
                SaveSettings();
            }
            if (automatic) ImGui::SetItemDefaultFocus();
            for (const auto& choice : controller_choices) {
                const bool selected = choice.key == preferred_controller;
                if (ImGui::Selectable(choice.name.c_str(), selected)) {
                    rocket::platform::set_preferred_controller_key(choice.key);
                    SaveSettings();
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (rocket::platform::controller_connected()) {
            ImGui::TextColored(kAccent, "Connected: %s", rocket::platform::controller_name().c_str());
            ImGui::TextDisabled("Keyboard and gamepad input can be used together for Player 1.");
        } else if (!preferred_controller.empty()) {
            ImGui::TextColored(kWarm, "The selected gamepad is disconnected");
            ImGui::TextDisabled("Rocket-R will reclaim it when it reconnects. Keyboard controls remain available.");
        } else {
            ImGui::TextColored(kWarm, "No SDL gamepad connected");
            ImGui::TextDisabled("Keyboard controls remain available. Connect a controller and press RESCAN.");
        }
        if (ImGui::Button("RESCAN CONTROLLERS", {std::min(width, 320.0F), 44.0F})) {
            rocket::platform::rescan_controller();
        }
        ImGui::Dummy({0.0F, 8.0F});
        bool background = rocket::input::background_input_enabled();
        if (ImGui::Checkbox("Allow controller input while Rocket-R is in the background", &background)) {
            rocket::input::set_background_input_enabled(background);
            SaveSettings();
        }
        ImGui::TextDisabled("Keyboard input is always focus-only to avoid typing into another app while moving Rocket.");
        ImGui::Dummy({0.0F, 8.0F});
        ImGui::SeparatorText("RUMBLE PAK");
        bool rumble = rocket::platform::rumble_enabled();
        if (ImGui::Checkbox("Enable Rumble Pak", &rumble)) {
            rocket::platform::set_rumble_enabled(rumble);
            SaveSettings();
        }
        if (rumble) {
            float strength = rocket::platform::rumble_strength() * 100.0F;
            ImGui::TextUnformatted("Rumble strength");
            ImGui::SetNextItemWidth(std::min(width, 620.0F));
            if (ImGui::SliderFloat("##rumble-strength", &strength, 0.0F, 100.0F, "%.0f%%")) {
                rocket::platform::set_rumble_strength(strength / 100.0F);
                SaveSettings();
            }
            ImGui::BeginDisabled(!rocket::platform::controller_connected());
            if (ImGui::Button("TEST RUMBLE", {220.0F, 42.0F})) {
                rocket::platform::test_rumble();
            }
            ImGui::EndDisabled();
        }
    } else if (g_controls_section == 1) {
        ImGui::SeparatorText("N64 BINDINGS");
        ImGui::TextDisabled("Every N64 control can have two keyboard inputs and two gamepad inputs.");
        const auto draw_binding_button = [&](Action action, BindingSlot slot,
                                             const char* prefix, float button_width) {
            const int source = rocket::input::binding(action, slot);
            const bool keyboard = slot == BindingSlot::KeyboardPrimary ||
                                  slot == BindingSlot::KeyboardSecondary;
            const std::string name = std::string(prefix) + (keyboard
                ? rocket::input::keyboard_binding_name(source)
                : rocket::input::controller_binding_name(source));
            ImGui::PushID(static_cast<int>(slot));
            if (ImGui::Button(name.c_str(), {button_width, 36.0F})) {
                BeginBindingCapture(action, slot);
            }
            ImGui::PopID();
        };

        if (width >= 700.0F && ImGui::BeginTable("rocket-controls-bindings", 3,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_SizingStretchProp, {width, 0.0F})) {
            ImGui::TableSetupColumn("N64 CONTROL", ImGuiTableColumnFlags_WidthFixed,
                                    std::clamp(width * 0.24F, 160.0F, 250.0F));
            ImGui::TableSetupColumn("KEYBOARD", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("GAMEPAD", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (std::size_t index = 0; index < rocket::input::action_count(); ++index) {
                const auto action = static_cast<Action>(index);
                ImGui::PushID(static_cast<int>(index));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextWrapped("%s", rocket::input::action_label(action));
                ImGui::TableSetColumnIndex(1);
                draw_binding_button(action, BindingSlot::KeyboardPrimary, "P: ", -1.0F);
                draw_binding_button(action, BindingSlot::KeyboardSecondary, "Alt: ", -1.0F);
                ImGui::TableSetColumnIndex(2);
                draw_binding_button(action, BindingSlot::ControllerPrimary, "P: ", -1.0F);
                draw_binding_button(action, BindingSlot::ControllerSecondary, "Alt: ", -1.0F);
                ImGui::PopID();
            }
            ImGui::EndTable();
        } else {
            for (std::size_t index = 0; index < rocket::input::action_count(); ++index) {
                const auto action = static_cast<Action>(index);
                ImGui::PushID(static_cast<int>(index));
                ImGui::TextUnformatted(rocket::input::action_label(action));
                draw_binding_button(action, BindingSlot::KeyboardPrimary, "Keyboard: ", width);
                draw_binding_button(action, BindingSlot::KeyboardSecondary, "Keyboard alt: ", width);
                draw_binding_button(action, BindingSlot::ControllerPrimary, "Gamepad: ", width);
                draw_binding_button(action, BindingSlot::ControllerSecondary, "Gamepad alt: ", width);
                ImGui::Separator();
                ImGui::PopID();
            }
        }
        ImGui::Spacing();
        if (ImGui::Button("RESTORE DEFAULT N64 BINDINGS", {std::min(width, 420.0F), 44.0F})) {
            rocket::input::reset_bindings();
            SaveSettings();
        }
    } else if (g_controls_section == 2) {
        ImGui::SeparatorText("CONTROLLER FEEL");
        const auto slider = [&](const char* label, const char* id, float value,
                                float minimum, float maximum, const char* format,
                                auto setter) {
            ImGui::TextUnformatted(label);
            ImGui::SetNextItemWidth(std::min(width, 700.0F));
            if (ImGui::SliderFloat(id, &value, minimum, maximum, format,
                                   ImGuiSliderFlags_AlwaysClamp)) {
                setter(value);
                SaveSettings();
            }
        };
        slider("Stick deadzone", "##stick-deadzone", rocket::input::stick_deadzone(),
               0.0F, 35.0F, "%.1f%%", rocket::input::set_stick_deadzone);
        slider("Stick anti-deadzone", "##stick-antideadzone", rocket::input::stick_anti_deadzone(),
               0.0F, 50.0F, "%.1f%%", rocket::input::set_stick_anti_deadzone);
        slider("Stick sensitivity", "##stick-sensitivity", rocket::input::stick_sensitivity(),
               50.0F, 150.0F, "%.0f%%", rocket::input::set_stick_sensitivity);
        slider("Response curve", "##stick-curve", rocket::input::stick_curve(),
               0.5F, 2.5F, "%.2f", rocket::input::set_stick_curve);
        slider("Analogue trigger threshold", "##trigger-threshold", rocket::input::trigger_threshold(),
               0.05F, 0.95F, "%.2f", rocket::input::set_trigger_threshold);
        bool invert_x = rocket::input::stick_x_inverted();
        bool invert_y = rocket::input::stick_y_inverted();
        if (ImGui::Checkbox("Invert horizontal stick", &invert_x)) {
            rocket::input::set_stick_x_inverted(invert_x);
            SaveSettings();
        }
        if (ImGui::Checkbox("Invert vertical stick", &invert_y)) {
            rocket::input::set_stick_y_inverted(invert_y);
            SaveSettings();
        }
        ImGui::TextDisabled("The defaults reproduce Rocket-R's previous fixed deadzone and trigger behaviour.");
        ImGui::Spacing();
        if (ImGui::Button("RESTORE DEFAULT STICK FEEL", {std::min(width, 420.0F), 44.0F})) {
            rocket::input::reset_stick_settings();
            SaveSettings();
        }
    } else {
        ImGui::SeparatorText("IN-GAME SHORTCUTS");
        ImGui::TextDisabled("Escape always opens/closes the overlay and Alt+Enter always toggles fullscreen as emergency fallbacks.");
        struct ShortcutRow {
            const char* label;
            rocket::input::ShortcutAction action;
        };
        constexpr std::array<ShortcutRow, 2> rows{{
            {"Toggle Rocket-R overlay", rocket::input::ShortcutAction::ToggleOverlay},
            {"Toggle fullscreen", rocket::input::ShortcutAction::ToggleFullscreen},
        }};
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const auto& row = rows[index];
            ImGui::PushID(static_cast<int>(index));
            ImGui::TextUnformatted(row.label);
            const float button_gap = ImGui::GetStyle().ItemSpacing.x;
            const float button_width = std::max((width - button_gap) * 0.5F, 1.0F);
            const std::string keyboard = "Keyboard: " + rocket::input::keyboard_binding_name(
                rocket::input::shortcut_keyboard_binding(row.action));
            const std::string controller = "Gamepad: " + rocket::input::controller_binding_name(
                rocket::input::shortcut_controller_binding(row.action));
            if (ImGui::Button(keyboard.c_str(), {button_width, 40.0F})) {
                BeginShortcutCapture(CaptureDevice::Keyboard, row.action);
            }
            ImGui::SameLine();
            if (ImGui::Button(controller.c_str(), {button_width, 40.0F})) {
                BeginShortcutCapture(CaptureDevice::Controller, row.action);
            }
            ImGui::Spacing();
            ImGui::PopID();
        }
        if (ImGui::Button("RESTORE DEFAULT SHORTCUTS", {std::min(width, 420.0F), 44.0F})) {
            rocket::input::reset_shortcuts();
            SaveSettings();
        }
    }
    DrawCapturePopup();
}



void DrawAboutPage() {
    LauncherHeading("ABOUT ROCKET-R");
    ImGui::Separator();
    ImGui::TextWrapped("Rocket-R is a native static recompilation frontend for the US release of Rocket: Robot on Wheels, using N64Recomp, N64ModernRuntime and RT64.");
    ImGui::Spacing();
    ImGui::TextWrapped("The game ROM is never distributed with Rocket-R. Select your own verified US ROM in the launcher.");
    ImGui::Spacing();
    ImGui::TextDisabled("FIXED38 platform/runtime baseline with Graphics v9 and DKR-R-style semantic presentation enhancements.");
}

void DrawSidebar(int& page, float width, bool overlay) {
    LauncherHeading("ROCKET-R");
    ImGui::TextDisabled("ROCKET: ROBOT ON WHEELS");
    ImGui::TextDisabled("RECOMPILED");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const std::array<const char*, 5> labels{"PLAY", "GRAPHICS", "SOUND", "CONTROLS", "ABOUT"};
    for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
        if (i == page) ImGui::PushStyleColor(ImGuiCol_Button, kWarm);
        if (ImGui::Button(labels[static_cast<std::size_t>(i)], {width, 54.0F})) page = i;
        if (i == page) ImGui::PopStyleColor();
    }
    if (overlay) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, kRed);
        if (ImGui::Button("EXIT TO DESKTOP", {width, 54.0F})) ultramodern::quit();
        ImGui::PopStyleColor();
    }
}

void DrawOverlayPage(int page, float width) {
    switch (page) {
        case 1: DrawGraphicsPage(width, true); break;
        case 2: DrawSoundPage(width); break;
        case 3: DrawControlsPage(width); break;
        case 4: DrawAboutPage(); break;
        default:
            ImGui::TextUnformatted("PLAY");
            ImGui::Separator();
            ImGui::TextWrapped("Rocket is running. Close this overlay to return to the game.");
            ImGui::Spacing();
            if (ImGui::Button("RESUME ROCKET", {270.0F, 56.0F})) {
                g_overlay_visible.store(false, std::memory_order_release);
            }
            ImGui::Spacing();
            ImGui::TextDisabled("F1 or Escape also closes the overlay.");
            break;
    }
}

void DrawDiagnosticsOverlay() {
    const auto settings = rocket::graphics::settings();
    if (!settings.performance_overlay && !settings.interpolation_overlay) return;

    ImGuiIO& io = ImGui::GetIO();
    constexpr float kPad = 14.0F;
    ImGui::SetNextWindowPos({io.DisplaySize.x - kPad, kPad},
                            ImGuiCond_Always, {1.0F, 0.0F});
    ImGui::SetNextWindowBgAlpha(0.78F);
    ImGui::Begin("Rocket-R diagnostics", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
    if (settings.performance_overlay) {
        const auto perf = rocket::graphics::performance_stats();
        ImGui::Text("%.1f FPS  %.2f ms", perf.fps, perf.frame_ms);
        ImGui::Text("%d Hz target / %d Hz display", perf.target_rate, perf.display_rate);
        ImGui::Text("Internal %.2fx", perf.resolution_scale);
        ImGui::Text("Presents %llu  interpolated %llu",
                    static_cast<unsigned long long>(perf.presents),
                    static_cast<unsigned long long>(perf.interpolated_presents));
    }
    if (settings.performance_overlay && settings.interpolation_overlay) {
        ImGui::Separator();
    }
    if (settings.interpolation_overlay) {
        const auto coverage = rocket::presentation::coverage_stats();
        ImGui::Text("Semantic %llu",
                    static_cast<unsigned long long>(coverage.semantic_bindings));
        ImGui::Text("Dynamic verts %llu",
                    static_cast<unsigned long long>(coverage.dynamic_vertex_bindings));
        ImGui::Text("Fail-closed snap %llu",
                    static_cast<unsigned long long>(coverage.snapped_bindings));
        ImGui::Text("Sidecar mismatch %llu",
                    static_cast<unsigned long long>(coverage.sidecar_mismatches));
    }
    ImGui::End();
}

void Attach(RT64::Application& application) {
    if (application.presentQueue == nullptr || application.device == nullptr ||
        application.swapChain == nullptr) return;
    std::scoped_lock guard(g_inspector_guard);
    std::scoped_lock present_lock(application.presentQueue->inspectorMutex);
    if (application.presentQueue->inspector != nullptr) {
        g_inspector = application.presentQueue->inspector.get();
        return;
    }
    application.presentQueue->inspector = std::make_unique<RT64::Inspector>(
        application.device.get(), application.swapChain.get(),
        application.chosenGraphicsAPI, rocket::platform::sdl_window());
    application.presentQueue->inspector->setIniPath(g_config_directory / "rocket-r-ui.ini");
    g_inspector = application.presentQueue->inspector.get();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard |
                                  ImGuiConfigFlags_NavEnableGamepad;
    std::fprintf(stderr, "[ui] in-game Rocket-R overlay attached (F1/Escape)\n");
}

} // namespace

void rocket::ui::configure(const std::filesystem::path& config_directory) {
    g_config_directory = config_directory;
    rocket::graphics::set_config_directory(config_directory);
    std::error_code ec;
    std::filesystem::create_directories(rocket::graphics::shader_directory(), ec);
    std::filesystem::create_directories(g_config_directory, ec);
    LoadSettings();
}

rocket::ui::StartupResult rocket::ui::run_launcher(
    SDL_Window* window, const std::filesystem::path& preselected_rom) {
    StartupResult result{};
    if (window == nullptr) return result;

    SDL_SetWindowTitle(window, "Rocket-R - Launcher");
#if defined(__linux__) || defined(__ANDROID__)
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
#else
    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer == nullptr) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
#endif
    if (renderer == nullptr) {
        std::fprintf(stderr, "[launcher] SDL renderer failed: %s\n", SDL_GetError());
        return result;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    g_launcher_context_active = true;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard |
                                  ImGuiConfigFlags_NavEnableGamepad;
    ConfigureLauncherFonts();
    ApplyStyle();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    std::filesystem::path selected_rom;
    std::string rom_status = "Choose your unmodified Rocket: Robot on Wheels US ROM.";
    bool rom_ready = false;
    auto try_rom = [&](const std::filesystem::path& path) {
        if (path.empty()) return;
        std::string error;
        if (rocket::select_rom(path, error)) {
            selected_rom = path;
            rom_ready = true;
            rom_status = "ROM verified: Rocket US / NSUE is ready to launch.";
            SaveLastRom(path);
        } else {
            rom_ready = false;
            rom_status = error;
        }
    };

    if (!preselected_rom.empty()) try_rom(preselected_rom);
    if (!rom_ready) {
        const auto last = LoadLastRom();
        if (!last.empty()) try_rom(last);
    }

    int page = 0;
    bool running = true;
    while (running) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (HandleInputCaptureEvent(&event)) {
                continue;
            }
            ImGui_ImplSDL2_ProcessEvent(&event);
            FeedGamepadNavigationEvent(event);
            if (event.type == SDL_QUIT ||
                (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)) {
                result.exit_requested = true;
                running = false;
            } else if (event.type == SDL_DROPFILE && event.drop.file != nullptr) {
                const std::filesystem::path dropped = std::filesystem::u8path(event.drop.file);
                SDL_free(event.drop.file);
                try_rom(dropped);
            } else if (event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
                       (event.key.keysym.scancode == SDL_SCANCODE_F11 ||
                        (event.key.keysym.scancode == SDL_SCANCODE_RETURN &&
                         (event.key.keysym.mod & KMOD_ALT) != 0))) {
                auto graphics = ultramodern::renderer::get_graphics_config();
                const bool fullscreen =
                    graphics.wm_option == ultramodern::renderer::WindowMode::Fullscreen;
                const Uint32 mode = fullscreen ? 0U : SDL_WINDOW_FULLSCREEN_DESKTOP;
                if (SDL_SetWindowFullscreen(window, mode) == 0) {
                    graphics.wm_option = fullscreen
                        ? ultramodern::renderer::WindowMode::Windowed
                        : ultramodern::renderer::WindowMode::Fullscreen;
                    ultramodern::renderer::set_graphics_config(graphics);
                    SaveSettings();
                }
            } else if (event.type == SDL_CONTROLLERDEVICEADDED ||
                       event.type == SDL_CONTROLLERDEVICEREMOVED) {
                rocket::platform::rescan_controller();
                rocket::platform::sample_input();
            }
        }
        rocket::platform::sample_input();

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        ApplyStyle();

        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(window, &win_w, &win_h);
        ImGui::SetNextWindowPos({0.0F, 0.0F});
        ImGui::SetNextWindowSize({static_cast<float>(win_w), static_cast<float>(win_h)});
        ImGui::Begin("Rocket-R Launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

        const float margin = std::clamp(ImGui::GetWindowWidth() * 0.03F, 18.0F, 42.0F);
        const float sidebar_width = std::clamp(ImGui::GetWindowWidth() * 0.27F, 300.0F, 410.0F);
        const float gap = 20.0F;
        const float height = ImGui::GetWindowHeight() - margin * 2.0F;
        ImGui::SetCursorPos({margin, margin});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.025F, 0.08F, 0.12F, 1.0F});
        ImGui::BeginChild("launcher-nav", {sidebar_width, height}, true);
        DrawSidebar(page, sidebar_width - 32.0F, false);
        ImGui::EndChild();
        ImGui::PopStyleColor();

        const float content_x = margin + sidebar_width + gap;
        const float content_width = ImGui::GetWindowWidth() - content_x - margin;
        ImGui::SetCursorPos({content_x, margin});
        ImGui::BeginChild("launcher-content", {content_width, height}, true);
        const float inner = std::max(content_width - 36.0F, 1.0F);
        if (page == 0) {
            LauncherHeading("PLAY ROCKET");
            ImGui::TextDisabled("Native recompilation runtime");
            ImGui::Separator();
            ImGui::TextWrapped("Rocket-R validates your own US cartridge dump and stores only a private local copy for the runtime.");
            ImGui::Spacing();
            if (ImGui::Button("CHOOSE ROM", {280.0F, 62.0F})) {
                const auto chosen = BrowseForRom();
                if (!chosen.empty()) try_rom(chosen);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("You can also drag a .z64/.n64/.v64 file onto this window.");
            ImGui::Spacing();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
            if (rom_ready) ImGui::TextColored(kAccent, "%s", rom_status.c_str());
            else ImGui::TextWrapped("%s", rom_status.c_str());
            if (!selected_rom.empty()) ImGui::TextDisabled("%s", PathUtf8(selected_rom).c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (!rom_ready) ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button, rom_ready ? kWarm : ImVec4{0.2F,0.2F,0.2F,1.0F});
            if (ImGui::Button("LAUNCH ROCKET", {340.0F, 74.0F}) && rom_ready) {
                result.launch = true;
                result.rom_path = selected_rom;
                running = false;
            }
            ImGui::PopStyleColor();
            if (!rom_ready) ImGui::EndDisabled();
        } else if (page == 1) {
            DrawGraphicsPage(inner, false);
        } else if (page == 2) {
            DrawSoundPage(inner);
        } else if (page == 3) {
            DrawControlsPage(inner);
        } else {
            DrawAboutPage();
        }
        ImGui::EndChild();
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 5, 13, 22, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
        SDL_RenderPresent(renderer);
        SDL_Delay(1);
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    g_launcher_context_active = false;
    g_launcher_body_font = nullptr;
    g_launcher_heading_font = nullptr;
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    std::fprintf(stderr, "[launcher] handoff complete; SDL window remains owned by main thread\n");
    return result;
}

void rocket::ui::detach(RT64::Application& application) {
    std::scoped_lock guard(g_inspector_guard);
    g_inspector = nullptr;
    if (application.presentQueue != nullptr) {
        std::scoped_lock present_lock(application.presentQueue->inspectorMutex);
        application.presentQueue->inspector.reset();
    }
}

void rocket::ui::draw(RT64::Application& application) {
    if (application.presentQueue == nullptr ||
        application.framebufferGraphicsWorker == nullptr) return;

    const bool visible = g_overlay_visible.load(std::memory_order_acquire);
    const auto graphics = rocket::graphics::settings();
    const bool diagnostics = graphics.performance_overlay ||
                             graphics.interpolation_overlay;
    if (!visible && !diagnostics) {
        if (application.presentQueue->inspector != nullptr) detach(application);
        return;
    }
    if (application.presentQueue->inspector == nullptr) Attach(application);
    if (application.presentQueue->inspector == nullptr) return;

    RT64::Inspector* inspector = application.presentQueue->inspector.get();
    inspector->newFrame(application.framebufferGraphicsWorker.get());
    ApplyStyle();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard |
                                  ImGuiConfigFlags_NavEnableGamepad;

    if (visible) {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos({0.0F, 0.0F});
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::SetNextWindowBgAlpha(0.90F);
        ImGui::Begin("Rocket-R Overlay", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

        const float margin = std::clamp(ImGui::GetWindowWidth() * 0.025F, 14.0F, 36.0F);
        const float sidebar = std::clamp(ImGui::GetWindowWidth() * 0.24F, 210.0F, 330.0F);
        const float gap = 18.0F;
        const float height = ImGui::GetWindowHeight() - margin * 2.0F;
        ImGui::SetCursorPos({margin, margin});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.02F, 0.07F, 0.10F, 0.94F});
        ImGui::BeginChild("overlay-nav", {sidebar, height}, true);
        DrawSidebar(g_overlay_page, sidebar - 32.0F, true);
        ImGui::EndChild();
        ImGui::PopStyleColor();

        const float content_x = margin + sidebar + gap;
        const float content_width = ImGui::GetWindowWidth() - content_x - margin;
        ImGui::SetCursorPos({content_x, margin});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.03F, 0.085F, 0.12F, 0.94F});
        ImGui::BeginChild("overlay-content", {content_width, height}, true);
        DrawOverlayPage(g_overlay_page, std::max(content_width - 36.0F, 1.0F));
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::End();
    } else {
        // This overlay is presentation-only and deliberately does not capture
        // keyboard/controller input while the full settings overlay is closed.
        DrawDiagnosticsOverlay();
    }
    inspector->endFrame();
}

bool rocket::ui::handle_runtime_event(SDL_Event* event) {
    if (event == nullptr) return false;
    if (HandleInputCaptureEvent(event)) return true;
    // Escape remains a hard-wired emergency route so a bad custom binding can
    // never trap the player outside the Rocket-R overlay. F1/controller Back
    // are ordinary configurable shortcuts handled by runtime_input.
    const bool keyboard_toggle = event->type == SDL_KEYDOWN && event->key.repeat == 0 &&
        event->key.keysym.scancode == SDL_SCANCODE_ESCAPE;
    if (keyboard_toggle) {
        toggle_overlay();
        return true;
    }

    if (!g_overlay_visible.load(std::memory_order_acquire)) return false;
    std::scoped_lock guard(g_inspector_guard);
    if (g_inspector == nullptr) return false;
    std::scoped_lock frame_lock(g_inspector->frameMutex);
    const bool gamepad_navigation = FeedGamepadNavigationEvent(*event);
    return g_inspector->handleSdlEvent(event) || gamepad_navigation;
}

void rocket::ui::toggle_overlay() {
    const bool next = !g_overlay_visible.load(std::memory_order_acquire);
    g_overlay_visible.store(next, std::memory_order_release);
    if (next) g_overlay_page = 0;
}

bool rocket::ui::overlay_visible() {
    return g_overlay_visible.load(std::memory_order_acquire);
}

bool rocket::ui::input_capture_active() {
    return g_capture_device != CaptureDevice::None;
}

bool rocket::ui::n64_dithering_enabled() {
    return g_n64_dithering_enabled.load(std::memory_order_relaxed);
}
