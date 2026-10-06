#include "runtime_ui.hpp"
#include "mods/mod_ui.hpp"
#include "mods/sdk_runtime.hpp"

#include "game_registration.hpp"
#include "graphics_enhancements.hpp"
#include "platform.hpp"
#include "presentation_identity.hpp"
#include "runtime_input.hpp"
#include "binding_capture.hpp"
#include "controls_studio.hpp"
#include "ui_theme.hpp"
#include "runtime_log.hpp"
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
#include "stb/stb_image.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

static void LauncherHeading(const char* text) {
    ImGui::Spacing();
    ImGui::TextUnformatted(text);
    ImGui::Separator();
}

static void UiHint(const char* format, ...) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::PushTextWrapPos(0.0F);
    va_list args;
    va_start(args, format);
    ImGui::TextV(format, args);
    va_end(args);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

namespace {

enum class Page { Play, Graphics, Sound, Controls, Mods, About };

struct PageHeading {
    const char* title;
    const char* caption;
};

constexpr std::array<PageHeading, 6> kPageHeadings{{
    {"ROCKET-R", "Rocket: Robot on wheels Recompiled"},
    {"GRAPHICS", "Adjust how the game looks and runs."},
    {"SOUND", "Adjust the game volume."},
    {"CONTROLS", "Set up your keyboard and controller."},
    {"MODS", "Add mods and choose how you want to play."},
    {"ROCKET-R", "Rocket: Robot on wheels Recompiled"},
}};

void DrawPageHeader(Page page) {
    const auto& heading = kPageHeadings[static_cast<size_t>(page)];
    rocket::ui::theme::heading(heading.title, 1.9F);

    // Reserve the same caption space on every page, including when the active
    // font or a narrow window makes the longer captions wrap.
    const float width = std::max(ImGui::GetContentRegionAvail().x, 1.0F);
    float caption_height = 0.0F;
    for (const auto& candidate : kPageHeadings)
        caption_height = std::max(caption_height,
            ImGui::CalcTextSize(candidate.caption, nullptr, false, width).y);
    const float caption_y = ImGui::GetCursorPosY();
    UiHint("%s", heading.caption);
    ImGui::SetCursorPosY(caption_y + caption_height + ImGui::GetStyle().ItemSpacing.y);
    ImGui::Dummy({0, 10});
}

std::filesystem::path g_config_directory;
std::atomic<bool> g_overlay_visible{false};
std::mutex g_inspector_guard;
RT64::Inspector* g_inspector = nullptr;
int g_overlay_page = 0;
int g_rocket_brand_rect = -1;

using CaptureDevice = rocket::input::BindingCapture::Device;
rocket::input::BindingCapture g_capture;
std::mutex g_capture_mutex;
std::vector<ImVec4> g_capture_mouse_controls;
std::atomic<bool> g_capture_active{false};
rocket::input::BindingSlot g_capture_slot = rocket::input::BindingSlot::KeyboardPrimary;
int g_capture_action = -1;
bool g_capture_shortcut = false;
bool g_capture_camera = false;
std::function<void(int)> g_capture_mod_commit;
std::string g_capture_mod_name;
rocket::input::ShortcutAction g_capture_shortcut_action = rocket::input::ShortcutAction::ToggleOverlay;
bool g_capture_popup_pending = false;

int g_controls_section = 0;
bool g_layout_preview = false;

constexpr auto kBackground = rocket::ui::theme::background;
constexpr auto kPanel = rocket::ui::theme::panel;
constexpr auto kPanelSoft = rocket::ui::theme::card;
constexpr auto kAccent = rocket::ui::theme::green;
constexpr auto kWarm = rocket::ui::theme::gold;
constexpr auto kRed = rocket::ui::theme::red;


std::string PathUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}

std::filesystem::path RuntimeUiAssetPath(const char* relative) {
    std::error_code ec;
    const auto probe = [&](const std::filesystem::path& p) -> std::filesystem::path {
        if (p.empty()) return {};
        ec.clear();
        return std::filesystem::is_regular_file(p, ec) && !ec ? p : std::filesystem::path{};
    };
    const auto probe_root = [&](const std::filesystem::path& root) -> std::filesystem::path {
        if (root.empty()) return {};
        if (auto p = probe(root / relative); !p.empty()) return p;
        if (auto p = probe(root / "assets/ui/Rocket-R-green-full-resolution.png"); !p.empty()) return p;
        if (auto p = probe(root / "src/UI/Rocket-R-green-full-resolution.png"); !p.empty()) return p;
        if (auto p = probe(root / "Rocket-R-green-full-resolution.png"); !p.empty()) return p;
        return {};
    };
    if (char* raw = SDL_GetBasePath(); raw != nullptr) {
        std::filesystem::path cur(raw); SDL_free(raw);
        if (auto p = probe_root(cur); !p.empty()) return p;
        for (int i=0;i<8;++i) {
            cur=cur.parent_path(); if (cur.empty()) break;
            if (auto p=probe_root(cur); !p.empty()) return p;
        }
    }
    std::filesystem::path cur = std::filesystem::current_path(ec);
    if (!ec) {
        if (auto p = probe_root(cur); !p.empty()) return p;
        for (int i=0;i<8;++i) {
            cur=cur.parent_path(); if (cur.empty()) break;
            if (auto p=probe_root(cur); !p.empty()) return p;
        }
    }
    return {};
}

std::vector<unsigned char> ReadRocketLogoBytes() {
    std::vector<unsigned char> bytes;
#if defined(__ANDROID__)
    // SDL_RWFromFile resolves relative names through Android's APK asset manager.
    if (SDL_RWops* rw = SDL_RWFromFile("Rocket-R-green-full-resolution.png", "rb")) {
        const Sint64 size = SDL_RWsize(rw);
        if (size > 0 && size <= 16 * 1024 * 1024) {
            bytes.resize(static_cast<std::size_t>(size));
            if (SDL_RWread(rw, bytes.data(), 1U, bytes.size()) != bytes.size()) bytes.clear();
        }
        SDL_RWclose(rw);
        if (!bytes.empty()) return bytes;
    }
#endif
    const std::filesystem::path path =
        RuntimeUiAssetPath("assets/ui/Rocket-R-green-full-resolution.png");
    if (path.empty()) return bytes;
    std::ifstream input(path, std::ios::binary);
    if (!input) return bytes;
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size <= 0 || size > 16 * 1024 * 1024) return {};
    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!input) bytes.clear();
    return bytes;
}

void LoadRocketBrandIntoAtlas() {
    g_rocket_brand_rect = -1;
    const std::vector<unsigned char> encoded = ReadRocketLogoBytes();
    if (encoded.empty()) {
        std::fprintf(stderr, "[ui] Rocket-R brand image unavailable; text fallback will be used\n");
        return;
    }
    int width = 0, height = 0, channels = 0;
    stbi_uc* source = stbi_load_from_memory(
        encoded.data(), static_cast<int>(encoded.size()),
        &width, &height, &channels, STBI_rgb_alpha);
    if (source == nullptr || width <= 0 || height <= 0) {
        std::fprintf(stderr, "[ui] Rocket-R brand decode failed: %s\n",
            stbi_failure_reason() ? stbi_failure_reason() : "unknown image error");
        if (source != nullptr) stbi_image_free(source);
        return;
    }

    // Trim transparent padding once, while loading. The source asset stays intact.
    int left = width, top = height, right = -1, bottom = -1;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (source[(static_cast<std::size_t>(y) * width + x) * 4U + 3] > 8) {
                left = std::min(left, x); right = std::max(right, x);
                top = std::min(top, y); bottom = std::max(bottom, y);
            }
        }
    }
    if (right < left || bottom < top) { stbi_image_free(source); return; }
    left = std::max(0, left - 2); top = std::max(0, top - 2);
    right = std::min(width - 1, right + 2); bottom = std::min(height - 1, bottom + 2);
    const int crop_width = right - left + 1, crop_height = bottom - top + 1;
    constexpr int kMaxBrandDimension = 384;
    const float scale = std::min(
        1.0F, static_cast<float>(kMaxBrandDimension) /
                  static_cast<float>(std::max(crop_width, crop_height)));
    const int dst_width = std::max(1, static_cast<int>(std::lround(crop_width * scale)));
    const int dst_height = std::max(1, static_cast<int>(std::lround(crop_height * scale)));
    std::vector<unsigned char> scaled(
        static_cast<std::size_t>(dst_width) * dst_height * 4U);

    // Bilinear downsample of the user's full-resolution source. Keeping at most
    // 384 px in the font atlas avoids turning a high-res branding asset into a
    // multi-megabyte per-context UI texture.
    for (int y = 0; y < dst_height; ++y) {
        const float sy = top + (static_cast<float>(y) + 0.5F) / scale - 0.5F;
        const int y0 = std::clamp(static_cast<int>(std::floor(sy)), top, bottom);
        const int y1 = std::min(y0 + 1, bottom);
        const float fy = std::clamp(sy - std::floor(sy), 0.0F, 1.0F);
        for (int x = 0; x < dst_width; ++x) {
            const float sx = left + (static_cast<float>(x) + 0.5F) / scale - 0.5F;
            const int x0 = std::clamp(static_cast<int>(std::floor(sx)), left, right);
            const int x1 = std::min(x0 + 1, right);
            const float fx = std::clamp(sx - std::floor(sx), 0.0F, 1.0F);
            for (int c = 0; c < 4; ++c) {
                const auto at = [&](int px, int py) -> float {
                    return static_cast<float>(source[
                        (static_cast<std::size_t>(py) * width + px) * 4U + c]);
                };
                const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
                const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
                scaled[(static_cast<std::size_t>(y) * dst_width + x) * 4U + c] =
                    static_cast<unsigned char>(std::clamp(
                        std::lround(top + (bottom - top) * fy), 0L, 255L));
            }
        }
    }
    stbi_image_free(source);

    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    atlas->TexDesiredWidth = std::max(atlas->TexDesiredWidth, 2048);
    g_rocket_brand_rect = atlas->AddCustomRectRegular(dst_width, dst_height);
    unsigned char* atlas_pixels = nullptr;
    int atlas_width = 0, atlas_height = 0;
    atlas->GetTexDataAsRGBA32(&atlas_pixels, &atlas_width, &atlas_height);
    ImFontAtlasCustomRect* rect = atlas->GetCustomRectByIndex(g_rocket_brand_rect);
    if (atlas_pixels == nullptr || rect == nullptr || !rect->IsPacked() ||
        rect->X + rect->Width > atlas_width || rect->Y + rect->Height > atlas_height) {
        g_rocket_brand_rect = -1;
        std::fprintf(stderr, "[ui] Rocket-R brand image would not fit ImGui atlas\n");
        return;
    }
    for (int row = 0; row < dst_height; ++row) {
        const auto* src_row = scaled.data() +
            static_cast<std::size_t>(row) * dst_width * 4U;
        auto* dst_row = atlas_pixels +
            (static_cast<std::size_t>(rect->Y + row) * atlas_width + rect->X) * 4U;
        std::memcpy(dst_row, src_row, static_cast<std::size_t>(dst_width) * 4U);
    }
    atlas->TexPixelsUseColors = true;
    std::fprintf(stderr, "[ui][brand] Rocket-R sidebar logo packed: %dx%d atlas=%dx%d rect=%d\n", dst_width, dst_height, atlas_width, atlas_height, g_rocket_brand_rect);
}

void ApplyRocketWindowIcon(SDL_Window* window) {
#if !defined(__ANDROID__)
    if (window == nullptr) return;
    const std::vector<unsigned char> encoded = ReadRocketLogoBytes();
    if (encoded.empty()) return;
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(
        encoded.data(), static_cast<int>(encoded.size()),
        &width, &height, &channels, STBI_rgb_alpha);
    if (pixels == nullptr || width <= 0 || height <= 0) {
        if (pixels != nullptr) stbi_image_free(pixels);
        return;
    }
    SDL_Surface* icon = SDL_CreateRGBSurfaceWithFormat(
        0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if (icon != nullptr) {
        for (int row = 0; row < height; ++row) {
            std::memcpy(static_cast<unsigned char*>(icon->pixels) + row * icon->pitch,
                        pixels + static_cast<std::size_t>(row) * width * 4U,
                        static_cast<std::size_t>(width) * 4U);
        }
        SDL_SetWindowIcon(window, icon);
        SDL_FreeSurface(icon);
    }
    stbi_image_free(pixels);
#endif
}

void DrawRocketBrand(float available_width, float maximum_size = 232.0F) {
    auto* atlas = ImGui::GetIO().Fonts;
    const auto* rect = g_rocket_brand_rect >= 0 ? atlas->GetCustomRectByIndex(g_rocket_brand_rect) : nullptr;
    if (!rect || !rect->IsPacked() || !atlas->TexID) {
        rocket::ui::theme::heading("ROCKET-R");
        return;
    }
    const float block_height = ImGui::GetContentRegionAvail().y < 670 ? 84.0F : 108.0F;
    const float w = std::max(1.0F, std::min({available_width, maximum_size,
        (block_height - 4) * rect->Width / rect->Height}));
    const float h = w * rect->Height / rect->Width;
    const float phase = g_layout_preview ? 0.0F : static_cast<float>(ImGui::GetTime()) * (2.0F * 3.14159265F / 4.8F);
    const float face_width = w * (0.06F + 0.94F * std::abs(std::cos(phase)));
    const auto p = ImGui::GetCursorScreenPos();
    const float left = p.x + (available_width - face_width) * 0.5F;
    const float top = p.y + (block_height - h) * 0.5F;
    const float tilt = std::sin(phase) * h * 0.022F;
    ImVec2 uv_min{}, uv_max{};
    atlas->CalcCustomRectUV(rect, &uv_min, &uv_max);
    // One cached image quad keeps the original turning motion and readable front.
    ImGui::GetWindowDrawList()->AddImageQuad(atlas->TexID,
        {left, top + tilt}, {left + face_width, top - tilt},
        {left + face_width, top + h + tilt}, {left, top + h - tilt},
        uv_min, {uv_max.x, uv_min.y}, uv_max, {uv_min.x, uv_max.y});
    ImGui::Dummy({available_width, block_height});
}

void ConfigureUiFont() {
    ImGuiIO& io = ImGui::GetIO();
    constexpr float kUiFontSize = 19.0F;
    // These belong to this context's atlas; never carry pointers across contexts.
    rocket::ui::theme::page_font = nullptr;
    rocket::ui::theme::card_font = nullptr;

    std::array<std::filesystem::path, 8> candidates{};
    std::size_t candidate_count = 0;
#if defined(_WIN32)
    if (const char* windir = std::getenv("WINDIR"); windir != nullptr && *windir != '\0') {
        candidates[candidate_count++] = std::filesystem::path(windir) / "Fonts" / "comic.ttf";
        candidates[candidate_count++] = std::filesystem::path(windir) / "Fonts" / "comicbd.ttf";
    }
#elif defined(__ANDROID__)
    candidates[candidate_count++] = "/system/fonts/ComicSansMS.ttf";
    candidates[candidate_count++] = "/system/fonts/ComicSans.ttf";
#elif defined(__linux__)
    candidates[candidate_count++] = "/usr/share/fonts/truetype/msttcorefonts/comic.ttf";
    candidates[candidate_count++] = "/usr/share/fonts/truetype/msttcorefonts/comicbd.ttf";
    candidates[candidate_count++] = "/usr/share/fonts/truetype/msttcorefonts/Comic_Sans_MS.ttf";
    candidates[candidate_count++] = "/usr/share/fonts/truetype/msttcorefonts/Comic_Sans_MS_Bold.ttf";
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        candidates[candidate_count++] = std::filesystem::path(home) / ".local/share/fonts/comic.ttf";
        candidates[candidate_count++] = std::filesystem::path(home) / ".fonts/comic.ttf";
    }
#endif

    bool font_loaded = false;
    for (std::size_t i = 0; i < candidate_count; ++i) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(candidates[i], ec) || ec) continue;
        const std::string path = PathUtf8(candidates[i]);
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str(), kUiFontSize)) {
            io.FontDefault = font;
            rocket::ui::theme::page_font = io.Fonts->AddFontFromFileTTF(path.c_str(), 42.0F);
            rocket::ui::theme::card_font = io.Fonts->AddFontFromFileTTF(path.c_str(), 26.0F);
            std::fprintf(stderr, "[ui] Comic Sans font: %s (%.0f px)\n",
                         path.c_str(), kUiFontSize);
            font_loaded = true;
            break;
        }
    }
    if (!font_loaded) {
        ImFontConfig fallback{};
        fallback.SizePixels = kUiFontSize;
        io.FontDefault = io.Fonts->AddFontDefault(&fallback);
        fallback.SizePixels = 42.0F;
        rocket::ui::theme::page_font = io.Fonts->AddFontDefault(&fallback);
        fallback.SizePixels = 26.0F;
        rocket::ui::theme::card_font = io.Fonts->AddFontDefault(&fallback);
        std::fprintf(stderr,
                     "[ui] Comic Sans MS was not installed; using enlarged ImGui fallback (%.0f px)\n",
                     kUiFontSize);
    }
    LoadRocketBrandIntoAtlas();
}

std::filesystem::path SettingsPath() {
    return g_config_directory / "rocket-r-settings.ini";
}

std::filesystem::path LastRomPath() {
    return g_config_directory / "last-rom.txt";
}

void ApplyStyle() {
    rocket::ui::theme::apply();
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
    const auto gx = rocket::graphics::settings();
    out << "graphics_preset=" << static_cast<int>(gx.preset) << '\n';
    out << "aspect_preset=" << static_cast<int>(gx.aspect) << '\n';
    out << "aspect_custom=" << gx.custom_aspect << '\n';
    out << "texture_filtering=" << static_cast<int>(gx.texture_filtering) << '\n';
    out << "framebuffer_precision=" << static_cast<int>(gx.framebuffer_precision) << '\n';
    out << "display_buffering=" << static_cast<int>(gx.display_buffering) << '\n';
    out << "hardware_resolve=" << static_cast<int>(gx.hardware_resolve) << '\n';
    out << "anisotropy=" << gx.anisotropy << '\n';
    out << "mip_lod_bias=" << gx.mip_lod_bias << '\n';
    out << "texture_detail_at_distance=" << gx.texture_detail_at_distance << '\n';
    out << "sky_dither_reduction=" << gx.sky_dither_reduction << '\n';
    out << "fov_offset=" << gx.fov_offset_degrees << '\n';
    out << "draw_distance=" << gx.draw_distance_multiplier << '\n';
    out << "vsync=" << (gx.vsync ? 1 : 0) << '\n';
    out << "post_process=" << static_cast<int>(gx.post_process) << '\n';
    out << "post_process_strength=" << gx.post_process_strength << '\n';
    out << "custom_shader=" << gx.custom_shader << '\n';
    out << "performance_overlay=" << (gx.performance_overlay ? 1 : 0) << '\n';
    out << "performance_logging=" << (gx.performance_logging ? 1 : 0) << '\n';
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
    for (std::size_t i=0; i<rocket::input::camera_action_count(); ++i) {
        const auto action = static_cast<rocket::input::CameraAction>(i);
        const std::string prefix = std::string("camera.") + rocket::input::camera_action_identifier(action);
        out << prefix << ".keyboard=" << rocket::input::camera_binding(action, true) << '\n';
        out << prefix << ".controller=" << rocket::input::camera_binding(action, false) << '\n';
    }
    out << "shortcut.overlay.keyboard=" << rocket::input::shortcut_keyboard_binding(rocket::input::ShortcutAction::ToggleOverlay) << '\n';
    out << "camera.mouse.enabled=" << rocket::input::camera_mouse_enabled() << '\n';
    out << "camera.mouse.sensitivity=" << rocket::input::camera_mouse_sensitivity() << '\n';
    out << "camera.mouse.recenter=" << rocket::input::camera_mouse_recenter_button() << '\n';
    out << "shortcut.overlay.controller=" << rocket::input::shortcut_controller_binding(rocket::input::ShortcutAction::ToggleOverlay) << '\n';
    out << "shortcut.fullscreen.keyboard=" << rocket::input::shortcut_keyboard_binding(rocket::input::ShortcutAction::ToggleFullscreen) << '\n';
    out << "shortcut.fullscreen.controller=" << rocket::input::shortcut_controller_binding(rocket::input::ShortcutAction::ToggleFullscreen) << '\n';
}

void LoadSettings() {
    ultramodern::renderer::GraphicsConfig graphics{};
    graphics.developer_mode = false;
    graphics.res_option = ultramodern::renderer::Resolution::Original2x;
    graphics.wm_option = ultramodern::renderer::WindowMode::Windowed;
    graphics.hr_option = ultramodern::renderer::HUDRatioMode::Original;
    graphics.api_option = ultramodern::renderer::GraphicsApi::Auto;
    graphics.ar_option = ultramodern::renderer::AspectRatio::Original;
    graphics.msaa_option = ultramodern::renderer::Antialiasing::None;
    graphics.rr_option = ultramodern::renderer::RefreshRate::Original;
    graphics.hpfb_option = ultramodern::renderer::HighPrecisionFramebuffer::Off;
    graphics.rr_manual_value = 60;
    graphics.ds_option = 1;
    float volume = 0.65F;
    int audio_profile = 0;
    rocket::graphics::Settings graphics_extra{};
    graphics_extra.framebuffer_precision = rocket::graphics::FramebufferPrecision::Original;
    graphics_extra.anisotropy = 4;
#if defined(__ANDROID__)
    // Start within a budget phone's GPU/memory budget, independent of the
    // display's physical resolution. Explicit saved choices still win below.
    graphics.res_option = ultramodern::renderer::Resolution::Original;
    graphics.hpfb_option = ultramodern::renderer::HighPrecisionFramebuffer::Off;
    graphics_extra = rocket::graphics::preset_settings(rocket::graphics::GraphicsPreset::LowPower);
#endif
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
        for (std::size_t i=0; i<rocket::input::camera_action_count(); ++i) {
            const auto action = static_cast<rocket::input::CameraAction>(i);
            const std::string prefix = std::string("camera.") + rocket::input::camera_action_identifier(action);
            try {
                if (key == prefix + ".keyboard") rocket::input::set_camera_binding(action, true, std::stoi(value));
                if (key == prefix + ".controller") rocket::input::set_camera_binding(action, false, std::stoi(value));
            } catch (...) {}
        }
        try {
            if (key == "camera.mouse.enabled") rocket::input::set_camera_mouse_enabled(std::stoi(value) != 0);
            else if (key == "camera.mouse.sensitivity") rocket::input::set_camera_mouse_sensitivity(std::stof(value));
            else if (key == "camera.mouse.recenter") rocket::input::set_camera_mouse_recenter_button(std::stoi(value));
            else if (key == "window_mode") graphics.wm_option = static_cast<ultramodern::renderer::WindowMode>(std::stoi(value));
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
            else if (key == "graphics_preset") graphics_extra.preset = static_cast<rocket::graphics::GraphicsPreset>(std::clamp(std::stoi(value), 0, 5));
            else if (key == "aspect_preset") { graphics_extra.aspect = static_cast<rocket::graphics::AspectPreset>(std::clamp(std::stoi(value), 0, 5)); aspect_preset_loaded = true; }
            else if (key == "aspect_custom") graphics_extra.custom_aspect = std::stof(value);
            else if (key == "texture_filtering") graphics_extra.texture_filtering = static_cast<rocket::graphics::TextureFiltering>(std::clamp(std::stoi(value), 0, 2));
            else if (key == "framebuffer_precision") graphics_extra.framebuffer_precision = static_cast<rocket::graphics::FramebufferPrecision>(std::clamp(std::stoi(value), 0, 2));
            else if (key == "display_buffering") graphics_extra.display_buffering = static_cast<rocket::graphics::DisplayBuffering>(std::clamp(std::stoi(value), 0, 1));
            else if (key == "fullscreen_style") { /* legacy v9-preview setting; RT64 uses borderless fullscreen */ }
            else if (key == "hardware_resolve") graphics_extra.hardware_resolve = static_cast<rocket::graphics::HardwareResolve>(std::clamp(std::stoi(value), 0, 2));
            else if (key == "anisotropy") graphics_extra.anisotropy = std::stoi(value);
            else if (key == "mip_lod_bias") graphics_extra.mip_lod_bias = std::stof(value);
            else if (key == "texture_detail_at_distance") graphics_extra.texture_detail_at_distance = std::stof(value);
            else if (key == "sky_dither_reduction") graphics_extra.sky_dither_reduction = std::stof(value);
            else if (key == "fov_offset") graphics_extra.fov_offset_degrees = std::stof(value);
            else if (key == "draw_distance") graphics_extra.draw_distance_multiplier = std::stof(value);
            else if (key == "vsync") graphics_extra.vsync = std::stoi(value) != 0;
            else if (key == "post_process") graphics_extra.post_process = static_cast<rocket::graphics::PostProcessMode>(std::clamp(std::stoi(value), 0, 3));
            else if (key == "post_process_strength") graphics_extra.post_process_strength = std::stof(value);
            else if (key == "custom_shader") graphics_extra.custom_shader = value;
            else if (key == "performance_overlay") graphics_extra.performance_overlay = std::stoi(value) != 0;
            else if (key == "performance_logging") graphics_extra.performance_logging = std::stoi(value) != 0;
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
        case R::Original: return "Original N64";
        case R::Original2x: return "2x N64";
        default: return "Match window (integer scaling)";
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
        case P::LowPower: return "Low power";
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
        case F::Original: return "Standard";
        case F::High: return "High precision";
        default: return "Automatic";
    }
}

const char* DisplayBufferingName(rocket::graphics::DisplayBuffering value) {
    return value == rocket::graphics::DisplayBuffering::Double ? "Double buffering" : "Triple buffering";
}

const char* HardwareResolveName(rocket::graphics::HardwareResolve value) {
    using H = rocket::graphics::HardwareResolve;
    switch (value) {
        case H::Off: return "Disabled";
        case H::On: return "Enabled";
        default: return "Automatic";
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

const std::vector<std::string>& CustomShaderStems() {
    static std::vector<std::string> cached;
    static std::filesystem::path scanned_directory;
    static std::chrono::steady_clock::time_point scan_due{};
    const auto directory = rocket::graphics::shader_directory();
    const auto now = std::chrono::steady_clock::now();
    if (scanned_directory == directory && now < scan_due) return cached;
    scanned_directory = directory;
    scan_due = now + std::chrono::seconds(2);
    cached.clear();
    std::set<std::string> stems;
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) return cached;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec || !entry.is_regular_file(ec)) continue;
        const auto ext = entry.path().extension().string();
        if (ext != ".dxil" && ext != ".spv") continue;
        stems.insert(entry.path().stem().string());
    }
    cached.assign(stems.begin(), stems.end());
    return cached;
}

bool SectionTabs(const char* const* labels, int count, float width, int& selected) {
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    float minimum = 0;
    for (int i = 0; i < count; ++i)
        minimum = std::max(minimum, ImGui::CalcTextSize(labels[i]).x + ImGui::GetStyle().FramePadding.x * 2.0F);
    const int columns = width >= minimum * count + gap * (count - 1) ? count
        : width >= minimum * 2 + gap ? 2 : 1;
    const float button_width = std::max((width - gap * (columns - 1)) / columns, 1.0F);
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        if (i % columns != 0) ImGui::SameLine();
        const bool active = selected == i;
        if (rocket::ui::theme::button(labels[i], {button_width, 44.0F}, active)) { selected = i; changed = true; }
    }
    ImGui::Dummy({0.0F, 12.0F});
    return changed;
}

void DrawLiveLog() {
    static rocket::diagnostics::LogSnapshot snapshot;
    static bool paused = false;
    static bool follow = true;
    static auto last_refresh = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (!paused && now - last_refresh >= std::chrono::milliseconds(100)) {
        rocket::diagnostics::refresh_log(snapshot);
        last_refresh = now;
    }

    LauncherHeading("LIVE LOG");
    if (rocket::ui::theme::button(paused ? "RESUME LOG" : "PAUSE LOG")) paused = !paused;
    ImGui::SameLine();
    if (rocket::ui::theme::button("COPY LOG")) {
        std::string text;
        for (const auto& line : snapshot.lines) { text += line; text += '\n'; }
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::SameLine();
    if (rocket::ui::theme::button("CLEAR VIEW")) {
        rocket::diagnostics::clear_log();
        snapshot = {};
    }
    ImGui::Checkbox("Auto-scroll", &follow);
    ImGui::SameLine();
    UiHint(paused ? "View paused. Logging continues." : "Live");

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.025F, 0.035F, 0.045F, 1.0F));
    if (ImGui::BeginChild("##live-debug-log", {0, ImGui::GetTextLineHeightWithSpacing() * 11.0F},
                          true, ImGuiWindowFlags_HorizontalScrollbar)) {
        if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel > 0.0F) follow = false;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, 0});
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(snapshot.lines.size()), ImGui::GetTextLineHeight());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& line = snapshot.lines[i];
                if (line.empty()) ImGui::Dummy({0, ImGui::GetTextLineHeight()});
                else ImGui::TextUnformatted(line.data(), line.data() + line.size());
            }
        }
        ImGui::PopStyleVar();
        if (snapshot.lines.empty()) UiHint("Waiting for messages...");
        if (follow && !paused) ImGui::SetScrollY(ImGui::GetScrollMaxY());
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    if (snapshot.discarded_lines) {
        UiHint("Showing recent messages. %llu older lines are no longer displayed.",
            static_cast<unsigned long long>(snapshot.discarded_lines));
    }
    if (!rocket::diagnostics::log_active()) ImGui::TextWrapped("Log capture is unavailable.");
    if (rocket::diagnostics::log_file_available()) {
        ImGui::TextWrapped("Log file: %s", PathUtf8(rocket::diagnostics::log_path()).c_str());
    } else {
        ImGui::TextWrapped("Could not save the log file. Messages are still shown here.");
    }
}

void DrawGraphicsPage(float width, bool in_game) {
    auto config = ultramodern::renderer::get_graphics_config();
    auto extra = rocket::graphics::settings();
    bool config_changed = false;
    bool extra_changed = false;
    bool keep_named_preset = false;

    DrawPageHeader(Page::Graphics);

    ImGui::TextUnformatted("Preset");
    const float preset_width = std::min(330.0F, ImGui::GetContentRegionAvail().x);
    ImGui::SetNextItemWidth(preset_width);
    if (ImGui::BeginCombo("##graphics-preset", GraphicsPresetName(extra.preset))) {
        const std::array<rocket::graphics::GraphicsPreset, 5> values{
            rocket::graphics::GraphicsPreset::Original,
            rocket::graphics::GraphicsPreset::Modern,
            rocket::graphics::GraphicsPreset::HighQuality,
            rocket::graphics::GraphicsPreset::Performance,
            rocket::graphics::GraphicsPreset::LowPower};
        for (auto value : values) {
            if (ImGui::Selectable(GraphicsPresetName(value), extra.preset == value)) {
                rocket::graphics::apply_preset(value);
                extra = rocket::graphics::settings();
                SyncExtraGraphicsConfig(extra, config);
                if (value == rocket::graphics::GraphicsPreset::Performance ||
                    value == rocket::graphics::GraphicsPreset::LowPower) {
                    config.res_option = value == rocket::graphics::GraphicsPreset::LowPower
                        ? ultramodern::renderer::Resolution::Original
                        : ultramodern::renderer::Resolution::Original2x;
                    config.msaa_option = ultramodern::renderer::Antialiasing::None;
                    config.ds_option = 1;
                    config.rr_option = ultramodern::renderer::RefreshRate::Original;
                    config.rr_manual_value = 60;
                }
                config_changed = true;
                extra_changed = true;
                keep_named_preset = true;
            }
        }
        ImGui::EndCombo();
    }
    UiHint("Changing a setting switches to Custom.");
    if (extra.preset == rocket::graphics::GraphicsPreset::Performance)
        UiHint("Original 2X, 30 FPS and no MSAA. You can raise the frame rate under Display.");
    if (extra.preset == rocket::graphics::GraphicsPreset::LowPower)
        UiHint("Original N64 resolution, 30 FPS and no MSAA. Restart the game after changing presets.");
    ImGui::Spacing();

    const auto draw_window = [&] {
        ImGui::PushItemWidth(-FLT_MIN);
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
        UiHint("Matches frames to your display to reduce tearing.");
        UiHint("Press Alt+Enter to switch between windowed and fullscreen.");

        ImGui::PopItemWidth();
    };
    const auto draw_aspect = [&] {
        ImGui::PushItemWidth(-FLT_MIN);
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
            ImGui::TextUnformatted("Custom ratio");
            ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, 560.0F));
            if (rocket::ui::theme::slider_float("##aspect-custom", &extra.custom_aspect, 1.0F, 3.5F, "%.3f:1")) extra_changed = true;
        }
        UiHint("On wide screens, the HUD stays within the centre 16:9 area.");

        ImGui::PopItemWidth();
    };
    const auto draw_resolution = [&] {
        ImGui::PushItemWidth(-FLT_MIN);
        ImGui::TextUnformatted("Render resolution");
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
        ImGui::TextUnformatted("Downsampling");
        ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, 560.0F));
        if (rocket::ui::theme::slider_int("##internal-scale", &scale, 1, 4, "%dx")) {
            config.ds_option = scale; config_changed = true;
        }

        ImGui::PopItemWidth();
    };
    const auto draw_rate = [&] {
        ImGui::PushItemWidth(-FLT_MIN);
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
            ImGui::TextUnformatted("Target FPS");
            int manual_rate = std::clamp(config.rr_manual_value, 30, 500);
            ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, 560.0F));
            if (rocket::ui::theme::slider_int("##refresh-manual", &manual_rate, 30, 500, "%d FPS")) {
                config.rr_manual_value = manual_rate; config_changed = true;
            }
        }
        UiHint(config.rr_option == ultramodern::renderer::RefreshRate::Original
            ? "Original 30 FPS, without frame interpolation."
            : "Smoother animation without changing the game's speed.");

        ImGui::PopItemWidth();
    };
    const auto draw_renderer = [&] {
        ImGui::PushItemWidth(-FLT_MIN);
        ImGui::TextUnformatted("Graphics renderer");
        if (in_game) {
            UiHint("%s. Change this in the launcher before starting the game.",
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
        if (in_game) UiHint("Restart the game to apply.");

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
        if (in_game) UiHint("Restart the game to apply.");

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
        ImGui::PopItemWidth();
    };
    const auto draw_image = [&] {
        ImGui::PushItemWidth(-FLT_MIN);
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

        ImGui::TextUnformatted("Image scaling filter");
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

        ImGui::TextUnformatted("Anisotropic filtering");
        ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, 560.0F));
        if (rocket::ui::theme::slider_int("##anisotropy", &extra.anisotropy, 1, 16, "%dx")) extra_changed = true;
        UiHint("Improves texture clarity at an angle.");
        if (in_game) UiHint("Restart the game to apply.");

        ImGui::TextUnformatted("Distant texture detail");
        ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, 560.0F));
        float distance_detail_percent = extra.texture_detail_at_distance * 100.0F;
        if (rocket::ui::theme::slider_float("##distance-detail", &distance_detail_percent, 0.0F, 100.0F, "%.0f%%")) {
            extra.texture_detail_at_distance = distance_detail_percent / 100.0F;
            extra_changed = true;
        }
        UiHint("0%%: Original   |   100%%: Maximum detail");
        ImGui::TextWrapped("Higher values keep distant textures sharper, but may shimmer or reduce performance. Changes apply immediately.");

        ImGui::TextUnformatted("Sky dithering reduction");
        ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, 560.0F));
        float sky_reduction_percent = extra.sky_dither_reduction * 100.0F;
        if (rocket::ui::theme::slider_float("##sky-dither-reduction", &sky_reduction_percent, 0.0F, 100.0F, "%.0f%%")) {
            extra.sky_dither_reduction = sky_reduction_percent / 100.0F;
            extra_changed = true;
        }
        UiHint("0%%: Original   |   100%%: Smoothest");
        ImGui::TextWrapped("Softens the patterned shading in the sky. Set to 0%% for the original look. Changes apply immediately.");

        ImGui::PopItemWidth();
    };
    const auto draw_advanced = [&] {
        ImGui::PushItemWidth(-FLT_MIN);
        ImGui::TextUnformatted("Replacement texture mip bias");
        ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, 560.0F));
        if (rocket::ui::theme::slider_float("##mip-bias", &extra.mip_lod_bias, -2.0F, 2.0F, "%+.2f")) extra_changed = true;
        UiHint("For replacement textures: lower values use sharper detail; higher values use softer detail.");

        ImGui::TextUnformatted("Screen effect");
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
            ImGui::TextUnformatted("Effect strength");
            ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, 560.0F));
            if (rocket::ui::theme::slider_float("##post-strength", &extra.post_process_strength, 0.0F, 100.0F, "%.0f%%")) extra_changed = true;
        }
        if (extra.post_process == rocket::graphics::PostProcessMode::Custom) {
            const auto& stems = CustomShaderStems();
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
                UiHint("Add .dxil files for Direct3D 12 or .spv files for Vulkan to your config/shaders folder.");
            }
            UiHint("Restart the game to apply your custom shader.");
        }
        ImGui::PopItemWidth();
    };
    const auto draw_camera = [&] {
        ImGui::PushItemWidth(-FLT_MIN);
        ImGui::TextUnformatted("Field of view adjustment");
        ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, 560.0F));
        if (rocket::ui::theme::slider_float("##fov-offset", &extra.fov_offset_degrees, -20.0F, 40.0F, "%+.1f deg")) extra_changed = true;

        ImGui::TextUnformatted("Draw distance");
        ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, 560.0F));
        int draw_distance_step = std::clamp(
            static_cast<int>(std::lround(extra.draw_distance_multiplier)), 1, 6);
        if (rocket::ui::theme::slider_int("##draw-distance", &draw_distance_step, 1, 6, "%dx")) {
            extra.draw_distance_multiplier = static_cast<float>(draw_distance_step);
            extra_changed = true;
        }
        UiHint("1x is the original distance. Higher values show objects farther away.");
        ImGui::PopItemWidth();
    };
    const auto draw_diagnostics = [&] {
        ImGui::PushItemWidth(-FLT_MIN);
        DrawLiveLog();
        ImGui::Spacing();
        if (ImGui::Checkbox("Show performance stats", &extra.performance_overlay)) extra_changed = true;
        if (ImGui::Checkbox("Record performance log", &extra.performance_logging)) extra_changed = true;
        UiHint("Adds a performance summary to the live log every four seconds.");
        if (ImGui::Checkbox("Show interpolation stats", &extra.interpolation_overlay)) extra_changed = true;
        if (ImGui::CollapsingHeader("Detailed statistics")) {
        const auto perf = rocket::graphics::performance_stats();
        const auto coverage = rocket::presentation::coverage_stats();
        ImGui::Separator();
        ImGui::Text("Frame rate: %.1f FPS (%.2f ms)", perf.fps, perf.frame_ms);
        if (!perf.device_name.empty()) ImGui::TextWrapped("Graphics device: %s", perf.device_name.c_str());
        if (perf.software_renderer)
            UiHint("The game is rendering on the CPU. Check that your GPU's Vulkan driver is installed and selected.");
        ImGui::Text("Display: %d Hz  |  Target: %d Hz", perf.display_rate, perf.target_rate);
        ImGui::Text("Resolution scale: %.2fx", perf.resolution_scale);
        ImGui::Text("Frames presented: %llu  |  Interpolated: %llu",
                    static_cast<unsigned long long>(perf.presents),
                    static_cast<unsigned long long>(perf.interpolated_presents));
        ImGui::Text("Identified interpolation bindings: %llu",
                    static_cast<unsigned long long>(coverage.semantic_bindings));
        ImGui::Text("Bindings without interpolation: %llu",
                    static_cast<unsigned long long>(coverage.snapped_bindings));
        ImGui::Text("Dynamic-vertex bindings: %llu",
                    static_cast<unsigned long long>(coverage.dynamic_vertex_bindings));
        ImGui::Text("Interpolation metadata mismatches: %llu",
                    static_cast<unsigned long long>(coverage.sidecar_mismatches));
        UiHint("When a match between frames is uncertain, interpolation is skipped for that item.");
        }
            ImGui::PopItemWidth();
    };
    {
        rocket::ui::theme::Card card("display-card", "GAME DISPLAY");
        {
            rocket::ui::theme::FieldGrid fields("display-fields");
            fields.next(); draw_window();
            fields.next(); draw_aspect();
            fields.next(); draw_resolution();
            fields.next(); draw_rate();
        }
        if (ImGui::CollapsingHeader("Renderer & performance")) draw_renderer();
    }
    const auto quality_card = [&] {
        rocket::ui::theme::Card card("image-card", "IMAGE QUALITY");
        draw_image();
        if (ImGui::CollapsingHeader("Advanced image options")) draw_advanced();
    };
    const auto camera_card = [&] {
        rocket::ui::theme::Card card("camera-card", "CAMERA & SCENERY");
        draw_camera();
    };
    if (width >= 700 && ImGui::BeginTable("quality-camera-grid", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn(); quality_card();
        ImGui::TableNextColumn(); camera_card();
        ImGui::EndTable();
    } else if (width < 700) { quality_card(); camera_card(); }
    {
        rocket::ui::theme::Card card("diagnostics-card");
        if (ImGui::CollapsingHeader("Diagnostics")) draw_diagnostics();
    }
    UiHint("Changes are saved automatically.");
    ImGui::Separator();
    ImGui::Spacing();
    if (rocket::ui::theme::button("RESET GRAPHICS", {std::min(width, 330.0F), 54.0F})) {
        rocket::graphics::reset_settings();
        extra = rocket::graphics::settings();
#if defined(__ANDROID__)
        config.res_option = ultramodern::renderer::Resolution::Original;
#else
        config.res_option = ultramodern::renderer::Resolution::Auto;
#endif
        config.ar_option = ultramodern::renderer::AspectRatio::Original;
        config.msaa_option = ultramodern::renderer::Antialiasing::None;
        config.rr_option = ultramodern::renderer::RefreshRate::Original;
        config.rr_manual_value = 60;
        config.ds_option = 1;
#if defined(__ANDROID__)
        config.hpfb_option = ultramodern::renderer::HighPrecisionFramebuffer::Off;
#else
        config.hpfb_option = ultramodern::renderer::HighPrecisionFramebuffer::Auto;
#endif
        config_changed = true;
        extra_changed = true;
        keep_named_preset = true;
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
    DrawPageHeader(Page::Sound);
    rocket::ui::theme::Card card("sound-card", "GAME VOLUME");
    float volume = rocket::platform::master_volume() * 100.0F;
    ImGui::TextUnformatted("Master volume");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (rocket::ui::theme::slider_float("##volume", &volume, 0.0F, 100.0F, "%.0f%%")) {
        rocket::platform::set_master_volume(volume / 100.0F);
        SaveSettings();
    }
}

std::string CaptureBindingName(CaptureDevice device, int source) {
    return device == CaptureDevice::Keyboard
        ? rocket::input::keyboard_binding_name(source)
        : rocket::input::controller_binding_name(source);
}

void StartCapture(CaptureDevice device) {
    g_capture.begin(device, rocket::platform::controller_instance_id(),
                    rocket::input::BindingCapture::Clock::now());
    g_capture_active.store(true);
    g_capture_popup_pending = true;
    ImGui::GetIO().ClearInputKeys();
}

void BeginBindingCapture(rocket::input::Action action, rocket::input::BindingSlot slot) {
    std::lock_guard lock(g_capture_mutex);
    g_capture_action = static_cast<int>(action);
    g_capture_slot = slot;
    g_capture_shortcut = false;
    g_capture_camera = false;
    g_capture_mod_commit={};
    StartCapture(slot == rocket::input::BindingSlot::KeyboardPrimary ||
                 slot == rocket::input::BindingSlot::KeyboardSecondary
        ? CaptureDevice::Keyboard : CaptureDevice::Controller);
}

void BeginShortcutCapture(CaptureDevice device, rocket::input::ShortcutAction action) {
    std::lock_guard lock(g_capture_mutex);
    g_capture_action = -1;
    g_capture_shortcut = true;
    g_capture_camera = false;
    g_capture_mod_commit={};
    g_capture_shortcut_action = action;
    StartCapture(device);
}

void FinishCapture() {
    g_capture.finish();
    g_capture_active.store(false);
    g_capture_mod_commit={};g_capture_mod_name.clear();
    ImGui::GetIO().ClearInputKeys();
    ImGui::CloseCurrentPopup();
}

void CommitCapturedSource(int source) {
    if(g_capture_mod_commit) {
        auto commit=g_capture_mod_commit;commit(source);
    } else if (g_capture_camera) {
        rocket::input::set_camera_binding(static_cast<rocket::input::CameraAction>(g_capture_action),
            g_capture.device == CaptureDevice::Keyboard, source);
    } else if (g_capture_shortcut) {
        if (g_capture.device == CaptureDevice::Keyboard)
            rocket::input::set_shortcut_keyboard_binding(g_capture_shortcut_action, source);
        else rocket::input::set_shortcut_controller_binding(g_capture_shortcut_action, source);
    } else {
        rocket::input::set_binding(static_cast<rocket::input::Action>(g_capture_action), g_capture_slot, source);
    }
    SaveSettings();
    FinishCapture();
}

bool HandleInputCaptureEvent(SDL_Event* event) {
    if (event == nullptr) return false;
    bool consumed;
    {
        std::lock_guard lock(g_capture_mutex);
        bool over_control = false;
        if (event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP)
            for (const auto& r : g_capture_mouse_controls)
                over_control |= event->button.x >= r.x && event->button.y >= r.y && event->button.x < r.z && event->button.y < r.w;
        consumed = g_capture.event(*event, rocket::platform::controller_instance_id(),
                                   rocket::input::BindingCapture::Clock::now(), over_control);
        g_capture_active.store(g_capture.active());
    }
    if (rocket::ui::controls::testing()) {
        const bool keyboard = event->type == SDL_KEYDOWN || event->type == SDL_KEYUP;
        const bool button = event->type == SDL_CONTROLLERBUTTONDOWN || event->type == SDL_CONTROLLERBUTTONUP;
        if ((event->type == SDL_KEYDOWN && event->key.keysym.scancode == SDL_SCANCODE_ESCAPE) ||
            (event->type == SDL_CONTROLLERBUTTONDOWN &&
             event->cbutton.which == rocket::platform::controller_instance_id() &&
             event->cbutton.button == SDL_CONTROLLER_BUTTON_BACK) ||
            (event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_FOCUS_LOST) ||
            event->type == SDL_APP_WILLENTERBACKGROUND) rocket::ui::controls::end_test();
        return consumed || keyboard || button || event->type == SDL_CONTROLLERAXISMOTION || event->type == SDL_MOUSEWHEEL;
    }
    return consumed;
}

void DrawCapturePopup() {
    using Phase = rocket::input::BindingCapture::Phase;
    std::lock_guard lock(g_capture_mutex);
    g_capture_mouse_controls.clear();
    const auto mouse_control = [] {
        const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        g_capture_mouse_controls.push_back({a.x, a.y, b.x, b.y});
    };
    // Device removal can be the last event in a launcher frame.
    SDL_Event tick{};
    g_capture.event(tick, rocket::platform::controller_instance_id(), rocket::input::BindingCapture::Clock::now());
    constexpr const char* kPopup = "Choose input";
    if (g_capture_popup_pending) { ImGui::OpenPopup(kPopup); g_capture_popup_pending = false; }
    const float width = std::min(500.0F, ImGui::GetIO().DisplaySize.x - 48.0F);
    ImGui::SetNextWindowSizeConstraints({width, 0}, {width, FLT_MAX});
    if (!ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    if (g_capture.phase == Phase::Cancelled || g_capture.phase == Phase::Idle) {
        FinishCapture(); ImGui::EndPopup(); return;
    }
    ImGui::TextWrapped("%s", g_capture_mod_commit?g_capture_mod_name.c_str():g_capture_camera
        ? rocket::input::camera_action_label(static_cast<rocket::input::CameraAction>(g_capture_action)) : g_capture_shortcut
        ? (g_capture_shortcut_action == rocket::input::ShortcutAction::ToggleOverlay
            ? "Open / close settings" : "Toggle fullscreen") : rocket::input::action_label(
        static_cast<rocket::input::Action>(g_capture_action)));
    ImGui::Separator();
    if (g_capture.phase == Phase::Ready) {
        if(g_capture_mod_commit) {
            const bool keyboard=g_capture.device==CaptureDevice::Keyboard;
            bool reserved=false;
            for(const auto shortcut:{rocket::input::ShortcutAction::ToggleOverlay,rocket::input::ShortcutAction::ToggleFullscreen})
                reserved|=g_capture.source>=0&&g_capture.source==(keyboard?rocket::input::shortcut_keyboard_binding(shortcut):rocket::input::shortcut_controller_binding(shortcut));
            if(reserved)ImGui::TextWrapped("This input is assigned to a launcher shortcut. Choose another input.");
            else {CommitCapturedSource(g_capture.source);ImGui::EndPopup();return;}
            if(rocket::ui::theme::button("TRY ANOTHER INPUT",{-1,44}))StartCapture(g_capture.device);
            if(rocket::ui::theme::button("CANCEL",{-1,44}))FinishCapture();
            ImGui::EndPopup();return;
        }
        if (g_capture_camera) {
            const bool keyboard = g_capture.device == CaptureDevice::Keyboard;
            bool reserved = false;
            for (const auto shortcut : {rocket::input::ShortcutAction::ToggleOverlay, rocket::input::ShortcutAction::ToggleFullscreen})
                reserved |= g_capture.source >= 0 && g_capture.source == (keyboard
                    ? rocket::input::shortcut_keyboard_binding(shortcut) : rocket::input::shortcut_controller_binding(shortcut));
            std::vector<std::string> conflicts;
            for (std::size_t i=0; i<rocket::input::camera_action_count(); ++i)
                if (static_cast<int>(i) != g_capture_action && g_capture.source >= 0 &&
                    rocket::input::camera_binding(static_cast<rocket::input::CameraAction>(i), keyboard) == g_capture.source)
                    conflicts.emplace_back(rocket::input::camera_action_label(static_cast<rocket::input::CameraAction>(i)));
            if (reserved) ImGui::TextWrapped("This input is assigned to a launcher shortcut. Choose another input, or change it in Shortcuts first.");
            else if (conflicts.empty()) { CommitCapturedSource(g_capture.source); ImGui::EndPopup(); return; }
            else {
                ImGui::TextWrapped("This input is also assigned to:");
                for (const auto& label : conflicts) ImGui::BulletText("%s", label.c_str());
                if (rocket::ui::theme::button("MOVE INPUT HERE", {-1, 44})) {
                    for (std::size_t i=0; i<rocket::input::camera_action_count(); ++i) {
                        const auto other = static_cast<rocket::input::CameraAction>(i);
                        if (static_cast<int>(i) != g_capture_action && rocket::input::camera_binding(other, keyboard) == g_capture.source)
                            rocket::input::set_camera_binding(other, keyboard, rocket::input::kUnbound);
                    }
                    CommitCapturedSource(g_capture.source);
                }
            }
            if (rocket::ui::theme::button("CANCEL", {-1, 44})) FinishCapture();
            ImGui::EndPopup(); return;
        }
        const auto action = static_cast<rocket::input::Action>(g_capture_action);
        const auto conflicts = g_capture_shortcut ? std::vector<rocket::input::BindingLocation>{}
            : rocket::input::binding_conflicts(action, g_capture_slot, g_capture.source);
        if (conflicts.empty()) { CommitCapturedSource(g_capture.source); ImGui::EndPopup(); return; }
        ImGui::TextColored(kWarm, "INPUT ALREADY IN USE");
        ImGui::TextWrapped("%s is also used by:", CaptureBindingName(g_capture.device, g_capture.source).c_str());
        for (const auto& conflict : conflicts) {
            const bool alternate = conflict.slot == rocket::input::BindingSlot::KeyboardSecondary ||
                                   conflict.slot == rocket::input::BindingSlot::ControllerSecondary;
            ImGui::TextWrapped("%s%s", rocket::input::action_label(conflict.action), alternate ? " (extra input)" : "");
        }
        if (conflicts.size() == 1 && rocket::ui::theme::button("SWAP INPUTS", {-1, 44})) {
            if (rocket::input::swap_binding(action, g_capture_slot, g_capture.source, conflicts[0])) {
                SaveSettings(); FinishCapture();
            }
        }
        if (rocket::ui::theme::button("SHARE INPUT", {-1, 44})) CommitCapturedSource(g_capture.source);
    } else {
        LauncherHeading("WAITING FOR INPUT");
        ImGui::TextWrapped(g_capture.device == CaptureDevice::Keyboard
            ? "Release held inputs, then press a key, click a mouse button or scroll the wheel."
            : "Release the controls, then press a button, pull a trigger or move a stick fully on your controller.");
        if (g_capture.device == CaptureDevice::Controller && !rocket::platform::controller_connected())
            ImGui::TextWrapped("No controller connected. Cancel and choose a controller on the CONTROLLER tab.");
        if (g_capture.device == CaptureDevice::Keyboard) {
            bool motion = g_capture.mouse_motion;
            if (ImGui::Checkbox("Capture mouse movement", &motion))
                g_capture.capture_mouse_motion(motion, rocket::input::BindingCapture::Clock::now());
            mouse_control();
            if (motion) ImGui::TextWrapped("Move the mouse clearly up, down, left or right.");
        }
        ImGui::TextWrapped("Press Escape to cancel, or Backspace / Delete to remove this input.");
        if (rocket::ui::theme::button("REMOVE INPUT", {-1, 44})) CommitCapturedSource(rocket::input::kUnbound);
        mouse_control();
    }
    if (rocket::ui::theme::button("CANCEL", {-1, 44})) FinishCapture();
    mouse_control();
    ImGui::EndPopup();
}

void DrawCameraModSettings(float width) {
    ImGui::SeparatorText("CAMERA CONTROLS");
    bool mouse_enabled = rocket::input::camera_mouse_enabled();
    if (ImGui::Checkbox("Enable mouse look", &mouse_enabled)) {
        rocket::input::set_camera_mouse_enabled(mouse_enabled); SaveSettings();
    }
    if (mouse_enabled) {
        float sensitivity = rocket::input::camera_mouse_sensitivity();
        ImGui::SetNextItemWidth(std::min(width, 420.0F));
        if (rocket::ui::theme::slider_float("Mouse sensitivity", &sensitivity, 0.01F, 1.0F, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
            rocket::input::set_camera_mouse_sensitivity(sensitivity); SaveSettings();
        }
        constexpr const char* mouse_buttons[] = {"Not assigned", "Left button", "Middle button", "Right button", "Side button 1", "Side button 2"};
        int recenter = rocket::input::camera_mouse_recenter_button();
        ImGui::SetNextItemWidth(std::min(width, 420.0F));
        if (ImGui::Combo("Mouse recenter", &recenter, mouse_buttons, 6)) {
            rocket::input::set_camera_mouse_recenter_button(recenter); SaveSettings();
        }
        UiHint("Move the mouse to look around. Escape opens settings and releases the cursor. Mouse look pauses when the window loses focus or the game uses a fixed camera.");
    }
    UiHint("Keyboard / mouse accepts keys, clicks and scrolling. Enable movement capture in the input prompt to bind a mouse direction.");
    UiHint("Cycle zoom uses the game's three camera distances. Toggle first person switches view; the right stick and mouse work in both views. Tap and release these buttons to switch modes.");
    UiHint("Bindings apply immediately and are saved on this device. While the mod controls the camera, these inputs take priority over N64 bindings. Settings shortcuts stay available.");
    for (std::size_t i=0; i<rocket::input::camera_action_count(); ++i) {
        const auto action = static_cast<rocket::input::CameraAction>(i);
        ImGui::PushID(static_cast<int>(i));
        ImGui::Spacing(); ImGui::Separator();
        ImGui::TextUnformatted(rocket::input::camera_action_label(action));
        const bool wide = width >= 660;
        const auto binding_button = [&](bool keyboard) {
            const auto label = CaptureBindingName(keyboard ? CaptureDevice::Keyboard : CaptureDevice::Controller,
                rocket::input::camera_binding(action, keyboard));
            ImGui::PushID(keyboard ? "keyboard" : "controller");
            if (rocket::ui::theme::button(label.c_str(), {ImGui::GetContentRegionAvail().x, 42})) {
                std::lock_guard lock(g_capture_mutex);
                g_capture_action = static_cast<int>(i);
                g_capture_shortcut = false;
                g_capture_camera = true;
                g_capture_mod_commit={};
                StartCapture(keyboard ? CaptureDevice::Keyboard : CaptureDevice::Controller);
            }
            ImGui::PopID();
        };
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {ImGui::GetStyle().ItemSpacing.x * 0.5F, 0});
        if (ImGui::BeginTable("camera-bindings", wide ? 2 : 1,
                ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX)) {
            if (wide) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); UiHint("Keyboard / mouse");
                ImGui::TableNextColumn(); UiHint("Controller");
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); binding_button(true);
                ImGui::TableNextColumn(); binding_button(false);
            } else {
                for (bool keyboard : {true, false}) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    UiHint(keyboard ? "Keyboard / mouse" : "Controller");
                    binding_button(keyboard);
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
        ImGui::PopID();
    }
    ImGui::Spacing();
    if (rocket::ui::theme::button("RESET CAMERA BINDINGS", {std::min(width, 420.0F), 44})) {
        rocket::input::reset_camera_bindings();
        rocket::input::set_camera_mouse_enabled(true);
        rocket::input::set_camera_mouse_sensitivity(0.15F);
        rocket::input::set_camera_mouse_recenter_button(SDL_BUTTON_MIDDLE);
        SaveSettings();
    }
    UiHint("Android touch controls include LOOK, ZOOM and VIEW while the mod controls the camera.");
}

void DrawControlsPage(float width) {
    using rocket::input::Action;
    using rocket::input::BindingSlot;
    DrawPageHeader(Page::Controls);

    constexpr std::array<const char*, 4> sections{
        "CONTROLLER", "N64 CONTROLS", "STICK", "SHORTCUTS"};
    if (SectionTabs(sections.data(), static_cast<int>(sections.size()), width, g_controls_section))
        rocket::ui::controls::end_test();

    rocket::ui::theme::Card card("controls-card");
    width = ImGui::GetContentRegionAvail().x;
    if (g_controls_section == 0) {
        ImGui::SeparatorText("ACTIVE CONTROLLER");
        const auto controller_choices = rocket::platform::controller_choices();
        const std::string preferred_controller = rocket::platform::preferred_controller_key();
        std::string controller_preview = preferred_controller.empty()
            ? "Automatic (first connected)"
            : rocket::platform::controller_name();
        ImGui::TextUnformatted("Controller");
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
            UiHint("You can use a keyboard and controller together.");
        } else if (!preferred_controller.empty()) {
            ImGui::TextColored(kWarm, "Selected controller disconnected.");
            UiHint("Reconnect your controller to use it again. Keyboard controls still work.");
        } else {
            ImGui::TextColored(kWarm, "No controller connected.");
            UiHint("Connect a controller, then select RESCAN CONTROLLERS. You can also use the keyboard.");
        }
        if (rocket::ui::theme::button("RESCAN CONTROLLERS", {std::min(width, 320.0F), 44.0F})) {
            rocket::platform::rescan_controller();
        }
        ImGui::Dummy({0.0F, 8.0F});
        bool background = rocket::input::background_input_enabled();
        if (ImGui::Checkbox("Allow background controller input", &background)) {
            rocket::input::set_background_input_enabled(background);
            SaveSettings();
        }
        UiHint("Allow controller input when the game window is inactive.");
        UiHint("Keyboard controls work only while the Rocket-R window is active.");
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
            if (rocket::ui::theme::slider_float("##rumble-strength", &strength, 0.0F, 100.0F, "%.0f%%")) {
                rocket::platform::set_rumble_strength(strength / 100.0F);
                SaveSettings();
            }
            ImGui::BeginDisabled(!rocket::platform::controller_connected());
            if (rocket::ui::theme::button("TEST RUMBLE", {220.0F, 42.0F})) {
                rocket::platform::test_rumble();
            }
            ImGui::EndDisabled();
        }
    } else if (g_controls_section == 1) {
        rocket::ui::controls::draw(width, {BeginBindingCapture, SaveSettings, rocket::platform::input_preview},
            rocket::platform::controller_name(), rocket::platform::controller_connected());
    } else if (g_controls_section == 2) {
        ImGui::SeparatorText("STICK SETTINGS");
        const auto slider = [&](const char* label, const char* id, float value,
                                float minimum, float maximum, const char* format,
                                auto setter) {
            ImGui::TextUnformatted(label);
            ImGui::SetNextItemWidth(std::min(width, 700.0F));
            if (rocket::ui::theme::slider_float(id, &value, minimum, maximum, format,
                                   ImGuiSliderFlags_AlwaysClamp)) {
                setter(value);
                SaveSettings();
            }
        };
        slider("Stick deadzone", "##stick-deadzone", rocket::input::stick_deadzone(),
               0.0F, 35.0F, "%.1f%%", rocket::input::set_stick_deadzone);
        UiHint("How far the stick must move before Rocket responds.");
        slider("Stick anti-deadzone", "##stick-antideadzone", rocket::input::stick_anti_deadzone(),
               0.0F, 50.0F, "%.1f%%", rocket::input::set_stick_anti_deadzone);
        UiHint("Sets the minimum output once the stick leaves the deadzone.");
        slider("Stick sensitivity", "##stick-sensitivity", rocket::input::stick_sensitivity(),
               50.0F, 150.0F, "%.0f%%", rocket::input::set_stick_sensitivity);
        slider("Response curve", "##stick-curve", rocket::input::stick_curve(),
               0.5F, 2.5F, "%.2f", rocket::input::set_stick_curve);
        slider("Trigger threshold", "##trigger-threshold", rocket::input::trigger_threshold(),
               0.05F, 0.95F, "%.2f", rocket::input::set_trigger_threshold);
        bool invert_x = rocket::input::stick_x_inverted();
        bool invert_y = rocket::input::stick_y_inverted();
        if (ImGui::Checkbox("Invert horizontal movement", &invert_x)) {
            rocket::input::set_stick_x_inverted(invert_x);
            SaveSettings();
        }
        if (ImGui::Checkbox("Invert vertical movement", &invert_y)) {
            rocket::input::set_stick_y_inverted(invert_y);
            SaveSettings();
        }
        UiHint("Adjust how your sticks and triggers respond.");
        ImGui::Spacing();
        if (rocket::ui::theme::button("RESET STICK SETTINGS", {std::min(width, 420.0F), 44.0F})) {
            rocket::input::reset_stick_settings();
            SaveSettings();
        }
    } else {
        ImGui::SeparatorText("IN-GAME SHORTCUTS");
        UiHint("Escape opens or closes settings. Alt+Enter switches fullscreen. These shortcuts always work during gameplay.");
        struct ShortcutRow {
            const char* label;
            rocket::input::ShortcutAction action;
        };
        constexpr std::array<ShortcutRow, 2> rows{{
            {"Open / close settings", rocket::input::ShortcutAction::ToggleOverlay},
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
            const std::string controller = "Controller: " + rocket::input::controller_binding_name(
                rocket::input::shortcut_controller_binding(row.action));
            if (rocket::ui::theme::button(keyboard.c_str(), {button_width, 40.0F})) {
                BeginShortcutCapture(CaptureDevice::Keyboard, row.action);
            }
            ImGui::SameLine();
            if (rocket::ui::theme::button(controller.c_str(), {button_width, 40.0F})) {
                BeginShortcutCapture(CaptureDevice::Controller, row.action);
            }
            ImGui::Spacing();
            ImGui::PopID();
        }
        if (rocket::ui::theme::button("RESET SHORTCUTS", {std::min(width, 420.0F), 44.0F})) {
            rocket::input::reset_shortcuts();
            SaveSettings();
        }
    }
}



enum class RomCardAction { None, Choose, Remove };

RomCardAction DrawRomCard(bool ready, const std::filesystem::path& rom, const std::string& status) {
    using namespace rocket::ui::theme;
    Card card("rom-card", nullptr, 0, ready ? green : border);
    auto* draw = ImGui::GetWindowDrawList();
    const auto p = ImGui::GetCursorScreenPos();
    draw->AddRectFilled(p, {p.x + 72, p.y + 24}, ImGui::GetColorU32(field), 12);
    for (int i = 0; i < 3; ++i)
        draw->AddCircleFilled({p.x + 14 + i * 22.0F, p.y + 12}, 6,
            ImGui::GetColorU32(i == 2 && ready ? green : i == 1 && !ready ? gold : border));
    ImGui::Dummy({72, 26});
    if (ImGui::GetContentRegionAvail().x >= 380) ImGui::SameLine();
    heading(ready ? "READY TO PLAY" : "CHOOSE YOUR ROM", 1.18F);
    ImGui::TextWrapped("%s", rom.empty() ? "No ROM selected" : PathUtf8(rom.filename()).c_str());
    UiHint("%s", status.c_str());
    ImGui::Spacing();
    const float width = ImGui::GetContentRegionAvail().x;
    RomCardAction action = RomCardAction::None;
    if (button("CHOOSE ROM...", {std::min(220.0F, width), 44})) action = RomCardAction::Choose;
    if (ready) {
        if (width >= 440) ImGui::SameLine();
        if (button("Remove selection", {std::min(170.0F, width), 44})) action = RomCardAction::Remove;
    }
    UiHint("Or drop a .z64, .n64 or .v64 file onto the launcher.");
    return action;
}

bool DrawLaunchProfile() {
    using namespace rocket::ui::theme;
    Card card("launch-profile-card");
    const float width = ImGui::GetContentRegionAvail().x;
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::BeginGroup();
    // Reserve the review button's column so long profile names wrap cleanly.
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (width >= 430 ? width - 170 : width));
    rocket::mods::ui::launch_summary();
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    if (width >= 430) {
        ImGui::SameLine();
        ImGui::SetCursorScreenPos({origin.x + width - 150, origin.y});
    }
    return button("Review mods", {std::min(width, 150.0F), 42});
}

void DrawAboutPage() {
    DrawPageHeader(Page::About);
    {
        rocket::ui::theme::Card card("about-project", "THE PROJECT");
        ImGui::TextWrapped("Recompilation project by ThatGuyMcd");
        ImGui::Spacing();
        ImGui::TextWrapped("Play Rocket: Robot on Wheels on Windows, Linux and Android, with widescreen, higher resolutions and custom controls.");
    }
    {
        rocket::ui::theme::Card card("about-tools", "BUILT WITH");
        ImGui::TextWrapped("N64Recomp, RSPRecomp, N64ModernRuntime and RT64.");
        ImGui::Spacing();
        ImGui::TextWrapped("Based on the Rocket: Robot on Wheels decompilation by RocketRet and contributors.");
    }
    {
        rocket::ui::theme::Card card("about-rom", "YOUR GAME");
        ImGui::TextWrapped("Requires your own unmodified US ROM. The game is not included.");
    }
    UiHint("Version %s", ROCKET_R_VERSION);
}

void DrawSidebar(int& page, float width, bool overlay, bool compact = false) {
    using namespace rocket::ui::theme;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {12, overlay ? 8.0F : 12.0F});
    width = ImGui::GetContentRegionAvail().x;
    if (!compact) {
        DrawRocketBrand(width);
        ImGui::SetWindowFontScale(0.72F);
        for (const char* caption : {"ROCKET: ROBOT ON WHEELS", "RECOMPILED"}) {
            const float indent = std::max(0.0F, (width - ImGui::CalcTextSize(caption).x) * .5F);
            ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + indent);
            UiHint("%s", caption);
        }
        ImGui::SetWindowFontScale(1.0F);
        ImGui::Dummy({0, 4});
    }
    constexpr std::array<const char*, 6> labels{"PLAY", "MODS", "GRAPHICS", "SOUND", "CONTROLS", "ABOUT"};
    constexpr std::array<int, 6> pages{0, 4, 1, 2, 3, 5};
    const int columns = compact ? (width >= 850 ? 6 : width >= 440 ? 3 : 2) : 1;
    const float button_width = (width - ImGui::GetStyle().ItemSpacing.x * (columns - 1)) / columns;
    const float button_height = compact ? 46 : std::clamp((ImGui::GetContentRegionAvail().y - 115) / (overlay ? 8 : 6) - 9, 30.0F, 47.0F);
    for (int i = 0; i < 6; ++i) {
        if (i % columns != 0) ImGui::SameLine();
        if (button(labels[i], {button_width, button_height}, page == pages[i], i == 0)) page = pages[i];
    }
    if (overlay) {
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        if (button("RETURN TO GAME", {width, 48})) g_overlay_visible.store(false, std::memory_order_release);
        ImGui::PushStyleColor(ImGuiCol_Button, red);
#if defined(__ANDROID__)
        constexpr const char* exit_label = "EXIT GAME";
#else
        constexpr const char* exit_label = "EXIT TO DESKTOP";
#endif
        if (button(exit_label, {width, 48})) ultramodern::quit();
        ImGui::PopStyleColor();
    }
    if (!compact) {
        ImGui::Spacing();
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - 42.0F));
        ImGui::SetWindowFontScale(0.72F);
        UiHint("Rocket-R %s", ROCKET_R_VERSION);
        ImGui::SetWindowFontScale(1.0F);
    }
    ImGui::PopStyleVar();
}

// Launcher and overlay share the same responsive shell. The overlay's panel
// backgrounds composite to 75% opacity; text, controls and modals stay clear.
// Only the fullscreen root is square; the shared modal style stays rounded.
void BeginPageShell(int& page, bool overlay) {
    using namespace rocket::ui::theme;
    const auto root = ImGui::GetWindowPos();
    const float backdrop = overlay ? overlay_backdrop_opacity : 1.0F;
    const float panel_alpha = overlay ? overlay_panel_opacity : 1.0F;
    ImGui::GetWindowDrawList()->AddRectFilledMultiColor(root,
        {root.x + ImGui::GetWindowWidth(), root.y + ImGui::GetWindowHeight()},
        ImGui::GetColorU32(opacity(rgb(0x183a59), backdrop)), ImGui::GetColorU32(opacity(background, backdrop)),
        ImGui::GetColorU32(opacity(rgb(0x112a43), backdrop)), ImGui::GetColorU32(opacity(rgb(0x0c1d31), backdrop)));
    const float window_width = ImGui::GetWindowWidth();
    const float margin = std::round(std::clamp(window_width * 0.022F, 16.0F, 34.0F));
    const float gap = std::round(std::clamp(window_width * 0.018F, 14.0F, 28.0F));
    const float height = std::max(1.0F, ImGui::GetWindowHeight() - margin * 2);
    const bool compact = window_width < 780;
    const float sidebar = std::round(std::clamp(window_width * 0.235F, 190.0F, 330.0F));
    const float outer_width = std::max(1.0F, window_width - margin * 2);
    const float nav_width = compact ? outer_width : sidebar;
    const float nav_inner = std::max(1.0F, nav_width - 32);
    const int columns = nav_inner >= 850 ? 6 : nav_inner >= 440 ? 3 : 2;
    const float nav_height = compact ? 36 + (6 / columns) * 58 - 12 + (overlay ? 158 : 0) : height;
    ImGui::SetCursorPos({margin, margin});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16, 18});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 2);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, opacity(navigation, panel_alpha));
    ImGui::PushStyleColor(ImGuiCol_Border, gold);
    ImGui::BeginChild(overlay ? "overlay-nav" : "launcher-nav", {nav_width, nav_height}, true);
    DrawSidebar(page, ImGui::GetContentRegionAvail().x, overlay, compact);
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    if (page != 3) rocket::ui::controls::end_test();
    const float content_x = compact ? margin : margin + sidebar + gap;
    const float content_y = compact ? margin + nav_height + gap : margin;
    const float content_width = compact ? outer_width : std::max(1.0F, window_width - content_x - margin);
    const float content_height = std::max(1.0F, ImGui::GetWindowHeight() - content_y - margin);
    ImGui::SetCursorPos({content_x, content_y});
    const float padding = std::round(std::clamp(content_width * 0.045F, 22.0F, 43.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {padding, padding});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 2);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, opacity(panel, panel_alpha));
    ImGui::BeginChild(overlay ? "overlay-content" : "launcher-content", {content_width, content_height}, true);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void DrawOverlayPage(int page, float width) {
    if (page != 3) rocket::ui::controls::end_test();
    switch (page) {
        case 1: DrawGraphicsPage(width, true); break;
        case 2: DrawSoundPage(width); break;
        case 3: DrawControlsPage(width); break;
        case 4: DrawPageHeader(Page::Mods); rocket::mods::ui::draw(); break;
        case 5: DrawAboutPage(); break;
        default:
            DrawPageHeader(Page::Play);
            {
                rocket::ui::theme::Card card("return-card", "ADVENTURE IN PROGRESS");
                ImGui::TextWrapped("Change your settings, then return to the game.");
            }
            ImGui::Spacing();
            if (rocket::ui::theme::button("RETURN TO GAME", {width, 68.0F}, false, true)) {
                g_overlay_visible.store(false, std::memory_order_release);
            }
            ImGui::Spacing();
#if defined(__ANDROID__)
            UiHint("Tap RETURN TO GAME or press Android Back.");
#else
            UiHint("Press Escape to return to the game.");
#endif
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
        ImGui::Text("Target: %d Hz  |  Display: %d Hz", perf.target_rate, perf.display_rate);
        ImGui::Text("Resolution: %.2fx", perf.resolution_scale);
        ImGui::Text("Frames: %llu  |  Interpolated: %llu",
                    static_cast<unsigned long long>(perf.presents),
                    static_cast<unsigned long long>(perf.interpolated_presents));
    }
    if (settings.performance_overlay && settings.interpolation_overlay) {
        ImGui::Separator();
    }
    if (settings.interpolation_overlay) {
        const auto coverage = rocket::presentation::coverage_stats();
        ImGui::Text("Identified bindings %llu",
                    static_cast<unsigned long long>(coverage.semantic_bindings));
        ImGui::Text("Dynamic bindings %llu",
                    static_cast<unsigned long long>(coverage.dynamic_vertex_bindings));
        ImGui::Text("Uninterpolated bindings %llu",
                    static_cast<unsigned long long>(coverage.snapped_bindings));
        ImGui::Text("Metadata mismatches %llu",
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
    // v18.2: RT64 Inspector owns a fresh ImGui context after the startup
    // launcher context is destroyed. Load the same Comic Sans policy into
    // this context so the in-game Launcher Overlay keeps its typography.
    ConfigureUiFont();
    g_inspector = application.presentQueue->inspector.get();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard |
                                  ImGuiConfigFlags_NavEnableGamepad;
    std::fprintf(stderr, "[ui] in-game Rocket-R overlay attached (F1/Escape)\n");
}

} // namespace

void rocket::ui::draw_camera_mod_settings(float width) {
    DrawCameraModSettings(width);
}

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

    ApplyRocketWindowIcon(window);
    SDL_SetWindowTitle(window, "Rocket-R - Launcher");
#if defined(__ANDROID__)
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
    SDL_RendererInfo renderer_info{};
    SDL_GetRendererInfo(renderer, &renderer_info);
    std::fprintf(stderr, "[launcher] renderer=%s accelerated=%d; active cap=60 Hz, idle=30 Hz\n",
        renderer_info.name ? renderer_info.name : "unknown",
        (renderer_info.flags & SDL_RENDERER_ACCELERATED) != 0);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard |
                                  ImGuiConfigFlags_NavEnableGamepad;
    ConfigureUiFont();
    ApplyStyle();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    std::filesystem::path selected_rom;
    std::string rom_status = "No ROM selected.";
    bool rom_ready = false;
    auto try_rom = [&](const std::filesystem::path& path) {
        if (path.empty()) return;
        std::string error;
        if (rocket::select_rom(path, error)) {
            selected_rom = path;
            rom_ready = true;
            rom_status = "ROM verified. Ready to play.";
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
    using Clock = std::chrono::steady_clock;
    auto last_interaction = Clock::now();
    auto last_frame = last_interaction - std::chrono::milliseconds(17);
    auto next_frame = last_interaction;
    const char* trace_env = std::getenv("ROCKET_PERFORMANCE_TRACE");
    const bool trace = trace_env != nullptr && trace_env[0] != '\0' && trace_env[0] != '0';
    auto trace_start = last_interaction;
    unsigned trace_frames = 0;
    SDL_Event event{};
    bool waited_event = false;
    while (running) {
        bool had_event = false;
        while (std::exchange(waited_event, false) || SDL_PollEvent(&event)) {
            // Expose notifications and idle joystick noise can accompany each
            // software present. They must not keep an untouched launcher busy.
            had_event |= event.type == SDL_KEYDOWN || event.type == SDL_KEYUP ||
                event.type == SDL_TEXTINPUT || event.type == SDL_MOUSEMOTION ||
                event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP ||
                event.type == SDL_MOUSEWHEEL || event.type == SDL_DROPFILE ||
                event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP ||
                (event.type == SDL_CONTROLLERAXISMOTION && std::abs(int(event.caxis.value)) > 8000) ||
                (event.type == SDL_WINDOWEVENT && (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || event.window.event == SDL_WINDOWEVENT_RESTORED));
            rocket::input::mouse_event(event);
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
                const auto extension=dropped.extension().string();
                if(extension==".nrm"||extension==".rtz"||extension==".zip"||extension==".json") {
                    rocket::mods::ui::import_file(dropped);page=4;
                } else try_rom(dropped);
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
        if (!running) break;
        const auto now = Clock::now();
        if (had_event) {
            last_interaction = now;
            next_frame = std::min(next_frame, last_frame + std::chrono::milliseconds(17));
        }
        if (now < next_frame) {
            const auto wait_ms = std::chrono::ceil<std::chrono::milliseconds>(next_frame - now).count();
            // Waiting releases the CPU. Keep the returned event for the next
            // iteration, including clicks, drops and controller hotplug events.
            waited_event = SDL_WaitEventTimeout(&event, static_cast<int>(wait_ms)) == 1;
            continue;
        }
        const Uint32 flags = SDL_GetWindowFlags(window);
        if ((flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0) {
            next_frame = now + std::chrono::milliseconds(250);
            continue;
        }
        rocket::platform::sample_input();

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(window, &win_w, &win_h);
        ImGui::SetNextWindowPos({0.0F, 0.0F});
        ImGui::SetNextWindowSize({static_cast<float>(win_w), static_cast<float>(win_h)});
        ImGui::SetNextWindowBgAlpha(0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, kBackground);
        ImGui::Begin("Rocket-R Launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        BeginPageShell(page, false);
        const float inner = std::max(ImGui::GetContentRegionAvail().x, 1.0F);
        if (page == 0) {
            DrawPageHeader(Page::Play);
            const auto rom_action = DrawRomCard(rom_ready, selected_rom, rom_status);
            if (rom_action == RomCardAction::Choose) {
                const auto chosen = BrowseForRom();
                if (!chosen.empty()) try_rom(chosen);
            } else if (rom_action == RomCardAction::Remove) {
                selected_rom.clear(); rom_ready = false; rom_status = "Choose your unmodified US ROM.";
                std::error_code remove_error;
                std::filesystem::remove(LastRomPath(), remove_error);
            }
            if (DrawLaunchProfile()) page = 4;
            ImGui::BeginDisabled(!rom_ready);
            if (rocket::ui::theme::button("PLAY ROCKET-R", {inner, 68}, false, true) && rom_ready && rocket::mods::ui::prepare_launch()) {
                result.launch = true;
                result.rom_path = selected_rom;
                running = false;
            }
            ImGui::EndDisabled();
            ImGui::Spacing();
            UiHint("F1: In-game settings   |   Alt+Enter: Fullscreen");
        } else if (page == 1) {
            DrawGraphicsPage(inner, false);
        } else if (page == 2) {
            DrawSoundPage(inner);
        } else if (page == 3) {
            DrawControlsPage(inner);
        } else if (page == 4) {
            DrawPageHeader(Page::Mods); rocket::mods::ui::draw();
        } else {
            DrawAboutPage();
        }
        DrawCapturePopup();
        ImGui::EndChild();
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 5, 13, 22, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
        SDL_RenderPresent(renderer);
        if (trace && ++trace_frames != 0 && now - trace_start >= std::chrono::seconds(4)) {
            std::fprintf(stderr, "[performance][launcher] fps=%.1f focus=%d renderer=%s\n",
                trace_frames / std::chrono::duration<double>(now - trace_start).count(),
                (flags & SDL_WINDOW_INPUT_FOCUS) != 0, renderer_info.name ? renderer_info.name : "unknown");
            trace_frames = 0; trace_start = now;
        }
        const auto& io = ImGui::GetIO();
        const bool held_input = std::any_of(std::begin(io.KeysData), std::end(io.KeysData),
            [](const ImGuiKeyData& key) { return key.Down; });
        const bool active = had_event || held_input || ImGui::IsAnyItemActive() ||
            ImGui::IsAnyMouseDown() || input_capture_active() ||
            now - last_interaction < std::chrono::milliseconds(750);
        // Animate the cached logo while retaining event waits and background throttling.
        const int interval_ms = (flags & SDL_WINDOW_INPUT_FOCUS) == 0 ? 250 : active ? 17 : 33;
        last_frame = now;
        next_frame = now + std::chrono::milliseconds(interval_ms);
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    std::fprintf(stderr, "[launcher] handoff complete; SDL window remains owned by main thread\n");
    return result;
}

bool rocket::ui::write_layout_previews(const std::filesystem::path& output_directory) {
    // Run only with an explicitly supplied output/config folder. This uses the
    // actual native page functions and SDL software rasterizer, rather than an
    // approximation of the UI or automation of a visible desktop window.
    std::filesystem::create_directories(output_directory);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return false;
    SDL_Window* window = SDL_CreateWindow("Rocket-R UI check", 0, 0, 1280, 800, SDL_WINDOW_HIDDEN);
    if (!window) { SDL_Quit(); return false; }
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) { SDL_DestroyWindow(window); SDL_Quit(); return false; }
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    g_layout_preview = true;
    ConfigureUiFont(); ApplyStyle();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);
    bool ok = true;
    std::ofstream report(output_directory / "native-layout.txt");
    constexpr const char* names[]{"play", "graphics", "sound", "controls", "mods", "about"};
    report << "modal-rounding=" << ImGui::GetStyle().WindowRounding << " popup-rounding=" << ImGui::GetStyle().PopupRounding
           << " overlay-panel-opacity=" << theme::overlay_backdrop_opacity +
              (1 - theme::overlay_backdrop_opacity) * theme::overlay_panel_opacity << '\n';
    for (const bool overlay : {false, true}) {
    for (const ImVec2 size : {ImVec2{1280, 720}, ImVec2{1280, 800}, ImVec2{800, 600}, ImVec2{640, 960}}) {
        SDL_SetWindowSize(window, static_cast<int>(size.x), static_cast<int>(size.y));
        for (int page = 0; page < 6; ++page) {
            g_controls_section = 1;
            for (int frame = 0; frame < 4; ++frame) {
                ImGui_ImplSDLRenderer2_NewFrame();
                ImGui_ImplSDL2_NewFrame();
                auto& io = ImGui::GetIO();
                io.DisplaySize = size;
                io.DisplayFramebufferScale = {1, 1};
                io.DeltaTime = 1.0F / 60;
                ImGui::NewFrame();
                ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(size);
                ImGui::SetNextWindowBgAlpha(0);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
                ImGui::PushStyleColor(ImGuiCol_WindowBg, kBackground);
                ImGui::Begin("Rocket-R native preview", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
                ImGui::PopStyleColor(); ImGui::PopStyleVar();
                BeginPageShell(page, overlay);
                const float width = ImGui::GetContentRegionAvail().x;
                if (page == 0 && overlay) DrawOverlayPage(page, width);
                else if (page == 0) {
                    DrawPageHeader(Page::Play);
                    DrawRomCard(true, "Rocket - Robot on Wheels (USA).z64",
                        "Example filename; no ROM is loaded by this check.");
                    DrawLaunchProfile();
                    theme::button("PLAY ROCKET-R", {width, 68}, false, true);
                } else if (page == 1) DrawGraphicsPage(width, overlay);
                else if (page == 2) DrawSoundPage(width);
                else if (page == 3) DrawControlsPage(width);
                else if (page == 4) { DrawPageHeader(Page::Mods); rocket::mods::ui::draw(true); }
                else DrawAboutPage();
                DrawCapturePopup();
                ImGui::EndChild(); ImGui::End(); ImGui::Render();
                SDL_SetRenderDrawColor(renderer, 9, 22, 38, 255); SDL_RenderClear(renderer);
                ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
                if (frame == 3) {
                    const std::string stem = std::string(overlay ? "overlay-" : "") + names[page] + "-" + std::to_string(static_cast<int>(size.x)) + "x" + std::to_string(static_cast<int>(size.y));
                    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, static_cast<int>(size.x), static_cast<int>(size.y), 32, SDL_PIXELFORMAT_RGBA32);
                    if (!surface) ok = false;
                    else {
                        ok &= SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGBA32, surface->pixels, surface->pitch) == 0;
                        ok &= SDL_SaveBMP(surface, PathUtf8(output_directory / (stem + ".bmp")).c_str()) == 0;
                        SDL_FreeSurface(surface);
                    }
                    const auto* draw = ImGui::GetDrawData();
                    report << stem << " vertices=" << draw->TotalVtxCount << " indices=" << draw->TotalIdxCount << '\n';
                }
                SDL_RenderPresent(renderer);
            }
        }
    }
    }
    ImGui_ImplSDLRenderer2_Shutdown(); ImGui_ImplSDL2_Shutdown(); ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
    g_layout_preview = false;
    return ok;
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
    const auto mod_hud=rocket::mods::sdk::hud_snapshot();
    if (!visible && !diagnostics && mod_hud.empty()) {
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

    if(!mod_hud.empty()) {
        const auto display=ImGui::GetIO().DisplaySize;
        const float sx=display.x/320.0F,sy=display.y/240.0F;
        auto* draw=ImGui::GetBackgroundDrawList();
        for(const auto& item:mod_hud) {
            const auto rgba=item.rgba;
            const auto colour=IM_COL32((rgba>>24)&255,(rgba>>16)&255,(rgba>>8)&255,rgba&255);
            const ImVec2 position{item.x*sx,item.y*sy};
            if(item.kind==ROCKET_HUD_RECTANGLE)draw->AddRectFilled(position,{position.x+item.width*sx,position.y+item.height*sy},colour);
            else draw->AddText(ImGui::GetFont(),std::clamp(item.height,1.0F,64.0F)*sy,position,colour,item.text);
        }
    }

    if (visible) {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos({0.0F, 0.0F});
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::SetNextWindowBgAlpha(0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, kBackground);
        ImGui::Begin("Rocket-R Overlay", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        BeginPageShell(g_overlay_page, true);
        DrawOverlayPage(g_overlay_page, std::max(ImGui::GetContentRegionAvail().x, 1.0F));
        DrawCapturePopup();
        ImGui::EndChild();
        ImGui::End();
    } else if(diagnostics) {
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
    else {
        controls::end_test();
        std::lock_guard lock(g_capture_mutex);
        if (g_capture.active()) g_capture.cancel();
        g_capture_active.store(false);
    }
}

bool rocket::ui::overlay_visible() {
    return g_overlay_visible.load(std::memory_order_acquire);
}

bool rocket::ui::input_capture_active() {
    return g_capture_active.load() || controls::testing();
}

bool rocket::ui::bindings_test_active() { return controls::testing(); }
void rocket::ui::begin_mod_binding_capture(const std::string& name,bool keyboard,std::function<void(int)> commit) {
    std::lock_guard lock(g_capture_mutex);g_capture_camera=false;g_capture_shortcut=false;
    g_capture_mod_commit=std::move(commit);g_capture_mod_name=name;
    StartCapture(keyboard?CaptureDevice::Keyboard:CaptureDevice::Controller);
}

bool rocket::ui::n64_dithering_enabled() {
    return true;
}
