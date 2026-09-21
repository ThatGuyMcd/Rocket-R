#pragma once

#include <filesystem>

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
void draw(RT64::Application& application);
bool handle_runtime_event(SDL_Event* event);
void toggle_overlay();
bool overlay_visible();
void detach(RT64::Application& application);

} // namespace rocket::ui
