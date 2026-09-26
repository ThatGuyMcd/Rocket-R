#pragma once

#include "ultramodern/ultramodern.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

union SDL_Event;
struct SDL_Window;

namespace rocket::input { struct State; }

namespace rocket::platform {

bool initialise();
void shutdown();
ultramodern::renderer::WindowHandle create_window();
// Platform-only launcher -> renderer handoff. Windows stays unchanged.
ultramodern::renderer::WindowHandle prepare_window_for_game();
SDL_Window* sdl_window();
#if defined(__ANDROID__)
void* android_native_window();
int android_display_refresh_rate();
#endif
void pump_runtime_events();
void sample_input();
void toggle_fullscreen();

struct ControllerChoice {
    std::string key;
    std::string name;
};

bool controller_connected();
std::int32_t controller_instance_id();
input::State input_preview();
std::string controller_name();
std::vector<ControllerChoice> controller_choices();
std::string preferred_controller_key();
void set_preferred_controller_key(const std::string& key);
void rescan_controller();
bool rumble_enabled();
void set_rumble_enabled(bool enabled);
float rumble_strength();
void set_rumble_strength(float strength);
void test_rumble();

void queue_samples(std::int16_t* samples, std::size_t sample_count);
std::size_t frames_remaining();
std::uint32_t audio_status();
void set_frequency(std::uint32_t frequency);
float master_volume();
void set_master_volume(float volume);

// N64ModernRuntime callbacks. poll_input intentionally does not touch SDL;
// the main/UI thread owns event pumping and publishes a protected snapshot.
void poll_input();
bool get_input(int controller_num, std::uint16_t* buttons, float* x, float* y);
void set_rumble(int controller_num, bool rumble);
ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num);

} // namespace rocket::platform
