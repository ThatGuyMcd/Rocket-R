#pragma once

#include "ultramodern/ultramodern.hpp"

#include <cstddef>
#include <cstdint>

union SDL_Event;
struct SDL_Window;

namespace rocket::platform {

bool initialise();
void shutdown();
ultramodern::renderer::WindowHandle create_window();
SDL_Window* sdl_window();
#if defined(__ANDROID__)
void* android_native_window();
int android_display_refresh_rate();
#endif
void pump_runtime_events();
void sample_input();
void toggle_fullscreen();

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
