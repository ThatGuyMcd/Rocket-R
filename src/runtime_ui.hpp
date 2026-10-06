#pragma once

#include <filesystem>
#include <functional>
#include <string>

union SDL_Event;
struct SDL_Window;
namespace RT64 { struct Application; }

namespace rocket::ui {

struct StartupResult {
    bool launch = false;
    bool exit_requested = false;
    std::filesystem::path rom_path;
};

void configure(const std::filesystem::path& config_directory);
StartupResult run_launcher(SDL_Window* window,
                           const std::filesystem::path& preselected_rom = {});
// Offscreen native rendering for UI regression checks; no ROM or game window.
bool write_layout_previews(const std::filesystem::path& output_directory);
void draw(RT64::Application& application);
bool handle_runtime_event(SDL_Event* event);
void toggle_overlay();
bool overlay_visible();
bool input_capture_active();
bool bindings_test_active();
void draw_camera_mod_settings(float width);
void begin_mod_binding_capture(const std::string& name,bool keyboard,std::function<void(int)> commit);
bool n64_dithering_enabled();
void detach(RT64::Application& application);

} // namespace rocket::ui
