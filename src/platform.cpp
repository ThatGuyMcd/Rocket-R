#include "platform.hpp"
#include "runtime_ui.hpp"
#include "widescreen_culling.hpp"

#define SDL_MAIN_HANDLED
#include <SDL.h>
#if defined(_WIN32) || defined(__APPLE__) || defined(__ANDROID__)
#include <SDL_syswm.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <vector>

namespace {

SDL_Window* g_window = nullptr;
#if defined(__ANDROID__)
void* g_android_native_window = nullptr;
std::atomic<int> g_android_display_rate{60};
#endif
SDL_GameController* g_controller = nullptr;
std::atomic<bool> g_controller_connected{false};
std::mutex g_input_mutex;
std::uint16_t g_buttons = 0;
float g_stick_x = 0.0F;
float g_stick_y = 0.0F;
std::atomic<bool> g_rumble_requested{false};
std::atomic<bool> g_rumble_dirty{false};
std::atomic<bool> g_rumble_enabled{true};

SDL_AudioDeviceID g_audio_device = 0;
std::mutex g_audio_mutex;
std::uint32_t g_audio_frequency = 0;
std::uint32_t g_audio_callback_frames = 0;
bool g_audio_playback_started = false;
std::deque<std::size_t> g_ai_fifo_frames;
using AudioClock = std::chrono::steady_clock;
AudioClock::time_point g_ai_last_update{};
bool g_ai_clock_running = false;
double g_ai_fractional_frames = 0.0;
std::size_t g_audio_nominal_block_frames = 0;
std::size_t g_audio_prime_target_frames = 0;
std::uint64_t g_audio_buffer_count = 0;
std::uint64_t g_audio_underrun_count = 0;
std::atomic<float> g_master_volume{0.65F};
std::vector<std::int16_t> g_audio_swap;

constexpr std::uint32_t AI_STATUS_FIFO_FULL = 0x80000000U;
constexpr std::uint32_t AI_STATUS_DMA_BUSY = 0x40000000U;

constexpr std::uint16_t N64_A = 0x8000;
constexpr std::uint16_t N64_B = 0x4000;
constexpr std::uint16_t N64_Z = 0x2000;
constexpr std::uint16_t N64_START = 0x1000;
constexpr std::uint16_t N64_DU = 0x0800;
constexpr std::uint16_t N64_DD = 0x0400;
constexpr std::uint16_t N64_DL = 0x0200;
constexpr std::uint16_t N64_DR = 0x0100;
constexpr std::uint16_t N64_L = 0x0020;
constexpr std::uint16_t N64_R = 0x0010;
constexpr std::uint16_t N64_CU = 0x0008;
constexpr std::uint16_t N64_CD = 0x0004;
constexpr std::uint16_t N64_CL = 0x0002;
constexpr std::uint16_t N64_CR = 0x0001;

float normalize_axis(Sint16 value, Sint16 deadzone = 7000) {
    const int v = static_cast<int>(value);
    const int magnitude = std::abs(v);
    if (magnitude <= deadzone) return 0.0F;
    const float scaled = static_cast<float>(magnitude - deadzone) /
                         static_cast<float>(32767 - deadzone);
    return std::copysign(std::min(scaled, 1.0F), static_cast<float>(v));
}

void close_controller() {
    if (g_controller != nullptr) {
        SDL_GameControllerClose(g_controller);
        g_controller = nullptr;
    }
    g_controller_connected.store(false, std::memory_order_release);
}

void open_first_controller() {
    if (g_controller != nullptr && SDL_GameControllerGetAttached(g_controller)) {
        g_controller_connected.store(true, std::memory_order_release);
        return;
    }
    close_controller();
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) continue;
        g_controller = SDL_GameControllerOpen(i);
        if (g_controller != nullptr) {
            g_controller_connected.store(true, std::memory_order_release);
            std::fprintf(stderr, "[input] controller: %s\n",
                         SDL_GameControllerName(g_controller));
            break;
        }
    }
}

void reset_audio_timing_locked() {
    g_audio_playback_started = false;
    g_ai_fifo_frames.clear();
    g_ai_clock_running = false;
    g_ai_fractional_frames = 0.0;
    g_audio_nominal_block_frames = 0;
    g_audio_prime_target_frames = 0;
    g_audio_buffer_count = 0;
    g_audio_underrun_count = 0;
}

std::size_t queued_audio_frames_locked() {
    if (g_audio_device == 0) return 0;
    return SDL_GetQueuedAudioSize(g_audio_device) /
           (2U * sizeof(std::int16_t));
}

// Model the N64 AI DMA engine independently from the host audio backend.
// Rocket's n_audio manager polls osAiGetLength()/osAiGetStatus() every 60 Hz
// retrace and its smallest authored DMA is only 368 stereo frames. SDL/WASAPI
// drains queued audio in host callback quanta, so deriving guest AI progress
// from SDL_GetQueuedAudioSize() makes the two-slot FIFO jump in 256/512-frame
// steps and changes Rocket's synthesis decisions. Advance the emulated AI at
// the exact game-requested sample clock instead; SDL is output buffering only.
void advance_virtual_ai_locked() {
    if (!g_ai_clock_running || g_audio_frequency == 0U ||
        g_ai_fifo_frames.empty()) {
        return;
    }

    const auto now = AudioClock::now();
    const double elapsed_seconds =
        std::chrono::duration<double>(now - g_ai_last_update).count();
    g_ai_last_update = now;
    if (elapsed_seconds <= 0.0) return;

    const double exact_frames =
        elapsed_seconds * static_cast<double>(g_audio_frequency) +
        g_ai_fractional_frames;
    std::uint64_t frames_to_consume =
        static_cast<std::uint64_t>(exact_frames);
    g_ai_fractional_frames =
        exact_frames - static_cast<double>(frames_to_consume);

    while (frames_to_consume != 0U && !g_ai_fifo_frames.empty()) {
        const std::size_t active_frames = g_ai_fifo_frames.front();
        if (frames_to_consume >= active_frames) {
            frames_to_consume -= active_frames;
            g_ai_fifo_frames.pop_front();
        } else {
            g_ai_fifo_frames.front() = active_frames -
                static_cast<std::size_t>(frames_to_consume);
            frames_to_consume = 0U;
        }
    }

    if (g_ai_fifo_frames.empty()) {
        g_ai_clock_running = false;
        g_ai_fractional_frames = 0.0;
    }
}

void start_virtual_ai_clock_locked() {
    if (g_ai_clock_running || g_ai_fifo_frames.empty() ||
        g_audio_frequency == 0U) {
        return;
    }
    g_ai_last_update = AudioClock::now();
    g_ai_fractional_frames = 0.0;
    g_ai_clock_running = true;
}

void maybe_start_audio_locked() {
    if (g_audio_device == 0 || g_audio_playback_started) return;
    const std::size_t queued_frames = queued_audio_frames_locked();
    if (g_audio_prime_target_frames != 0U &&
        queued_frames >= g_audio_prime_target_frames) {
        SDL_PauseAudioDevice(g_audio_device, 0);
        g_audio_playback_started = true;
        std::fprintf(stderr,
                     "[audio] host playback started: queued=%zu target=%zu callback=%u; guest AI clock is independent\n",
                     queued_frames, g_audio_prime_target_frames,
                     static_cast<unsigned>(g_audio_callback_frames));
    }
}

void open_audio_locked(std::uint32_t frequency) {
    if (g_audio_device != 0) {
        SDL_ClearQueuedAudio(g_audio_device);
        SDL_CloseAudioDevice(g_audio_device);
        g_audio_device = 0;
    }
    reset_audio_timing_locked();

    SDL_AudioSpec desired{};
    desired.freq = static_cast<int>(frequency);
    desired.format = AUDIO_S16SYS;
    desired.channels = 2;
    // Rocket's smallest authored n_audio DMA is 368 stereo frames. Keep the
    // host callback below that size so one host wake cannot consume more than
    // a complete minimum guest DMA in a single scheduling quantum.
    desired.samples = 256;
    SDL_AudioSpec obtained{};
    g_audio_device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (g_audio_device == 0) {
        g_audio_frequency = 0;
        g_audio_callback_frames = 0;
        std::fprintf(stderr, "[audio] SDL_OpenAudioDevice failed at %u Hz: %s\n",
                     frequency, SDL_GetError());
        return;
    }

    // allowed_changes=0 above requires the requested format/rate. Keep the
    // guest AI clock anchored to the rate Rocket asked osAiSetFrequency for.
    g_audio_frequency = frequency;
    g_audio_callback_frames = obtained.samples;
    SDL_PauseAudioDevice(g_audio_device, 1);
    std::fprintf(stderr,
                 "[audio] device opened requested=%u actual=%d Hz stereo S16 callback=%u; host paused for cushion prime\n",
                 frequency, obtained.freq, static_cast<unsigned>(obtained.samples));
}

void apply_rumble_request() {
    if (!g_rumble_dirty.exchange(false, std::memory_order_acq_rel)) return;
    if (g_controller == nullptr || !SDL_GameControllerGetAttached(g_controller)) return;
    const bool enabled = g_rumble_enabled.load(std::memory_order_acquire) &&
                         g_rumble_requested.load(std::memory_order_acquire);
    SDL_GameControllerRumble(g_controller, enabled ? 0xFFFF : 0,
                             enabled ? 0xFFFF : 0, enabled ? 250 : 0);
}

void keyboard_input(std::uint16_t& buttons, float& x, float& y) {
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    if (keys == nullptr) return;
    if (keys[SDL_SCANCODE_X] || keys[SDL_SCANCODE_SPACE]) buttons |= N64_A;
    if (keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_LCTRL]) buttons |= N64_B;
    if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) buttons |= N64_Z;
    if (keys[SDL_SCANCODE_RETURN]) buttons |= N64_START;
    if (keys[SDL_SCANCODE_UP]) buttons |= N64_DU;
    if (keys[SDL_SCANCODE_DOWN]) buttons |= N64_DD;
    if (keys[SDL_SCANCODE_LEFT]) buttons |= N64_DL;
    if (keys[SDL_SCANCODE_RIGHT]) buttons |= N64_DR;
    if (keys[SDL_SCANCODE_Q]) buttons |= N64_L;
    if (keys[SDL_SCANCODE_E]) buttons |= N64_R;
    if (keys[SDL_SCANCODE_I]) buttons |= N64_CU;
    if (keys[SDL_SCANCODE_K]) buttons |= N64_CD;
    if (keys[SDL_SCANCODE_J]) buttons |= N64_CL;
    if (keys[SDL_SCANCODE_L]) buttons |= N64_CR;

    x = (keys[SDL_SCANCODE_D] ? 1.0F : 0.0F) - (keys[SDL_SCANCODE_A] ? 1.0F : 0.0F);
    y = (keys[SDL_SCANCODE_W] ? 1.0F : 0.0F) - (keys[SDL_SCANCODE_S] ? 1.0F : 0.0F);
    if (x != 0.0F && y != 0.0F) {
        constexpr float kDiagonal = 0.70710678F;
        x *= kDiagonal;
        y *= kDiagonal;
    }
}

void controller_input(SDL_GameController* controller, std::uint16_t& buttons,
                      float& x, float& y) {
    if (controller == nullptr || !SDL_GameControllerGetAttached(controller)) return;
    auto pressed = [controller](SDL_GameControllerButton button) {
        return SDL_GameControllerGetButton(controller, button) != 0;
    };
    if (pressed(SDL_CONTROLLER_BUTTON_A)) buttons |= N64_A;
    if (pressed(SDL_CONTROLLER_BUTTON_B)) buttons |= N64_B;
    if (pressed(SDL_CONTROLLER_BUTTON_START)) buttons |= N64_START;
    if (pressed(SDL_CONTROLLER_BUTTON_DPAD_UP)) buttons |= N64_DU;
    if (pressed(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) buttons |= N64_DD;
    if (pressed(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) buttons |= N64_DL;
    if (pressed(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) buttons |= N64_DR;
    if (pressed(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) buttons |= N64_L;
    if (pressed(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) buttons |= N64_R;
    if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000) buttons |= N64_Z;

    if (pressed(SDL_CONTROLLER_BUTTON_Y)) buttons |= N64_CU;
    if (pressed(SDL_CONTROLLER_BUTTON_X)) buttons |= N64_CL;
    if (pressed(SDL_CONTROLLER_BUTTON_RIGHTSTICK)) buttons |= N64_CD;
    const float rx = normalize_axis(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX), 12000);
    const float ry = normalize_axis(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY), 12000);
    if (rx < -0.45F) buttons |= N64_CL;
    if (rx > 0.45F) buttons |= N64_CR;
    if (ry < -0.45F) buttons |= N64_CU;
    if (ry > 0.45F) buttons |= N64_CD;

    const float pad_x = normalize_axis(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX));
    const float pad_y = -normalize_axis(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY));
    if (std::abs(pad_x) > std::abs(x)) x = pad_x;
    if (std::abs(pad_y) > std::abs(y)) y = pad_y;
}

} // namespace

bool rocket::platform::initialise() {
    SDL_SetMainReady();
#ifdef SDL_HINT_WINDOWS_DPI_AWARENESS
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#if defined(_WIN32)
    SDL_setenv("SDL_AUDIODRIVER", "wasapi", 0);
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER |
                 SDL_INIT_HAPTIC | SDL_INIT_SENSOR) < 0) {
        std::fprintf(stderr, "[platform] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_version version{};
    SDL_GetVersion(&version);
    std::fprintf(stderr, "[platform] SDL %u.%u.%u initialized on main thread\n",
                 version.major, version.minor, version.patch);
    open_first_controller();
    // Do not open a placeholder-rate device here. N64ModernRuntime first calls
    // set_frequency with a dummy rate and Rocket then requests its authored
    // 22,500 Hz rate before submitting audio. Opening synchronously from that
    // callback guarantees no Rocket samples can ever be played at 32/48 kHz.
    sample_input();
    return true;
}

void rocket::platform::shutdown() {
    close_controller();
    {
        std::lock_guard lock(g_audio_mutex);
        if (g_audio_device != 0) {
            SDL_ClearQueuedAudio(g_audio_device);
            SDL_CloseAudioDevice(g_audio_device);
            g_audio_device = 0;
            g_audio_frequency = 0;
            g_audio_callback_frames = 0;
            reset_audio_timing_locked();
        }
    }
    if (g_window != nullptr) {
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
    }
#if defined(__ANDROID__)
    g_android_native_window = nullptr;
    g_android_display_rate.store(60, std::memory_order_release);
#endif
    SDL_Quit();
}

ultramodern::renderer::WindowHandle rocket::platform::create_window() {
    if (g_window == nullptr) {
        Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#if defined(__APPLE__)
        flags |= SDL_WINDOW_METAL;
#elif defined(__linux__) || defined(__ANDROID__)
        flags |= SDL_WINDOW_VULKAN;
#endif
        g_window = SDL_CreateWindow("Rocket-R - Rocket: Robot on Wheels Recompiled",
                                    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    1440, 900, flags);
        if (g_window == nullptr) {
            std::fprintf(stderr, "[platform] SDL_CreateWindow failed: %s\n", SDL_GetError());
            return {};
        }
        SDL_SetWindowMinimumSize(g_window, 800, 600);
        SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
        rocket::widescreen::update_window_aspect(g_window);
#if defined(__ANDROID__)
        // Video/window APIs stay on the SDL owner thread. Capture the native
        // surface here so the RT64 context worker never calls SDL_GetWindowWMInfo.
        SDL_SysWMinfo android_info{};
        SDL_VERSION(&android_info.version);
        if (SDL_GetWindowWMInfo(g_window, &android_info) != SDL_TRUE ||
            android_info.subsystem != SDL_SYSWM_ANDROID ||
            android_info.info.android.window == nullptr) {
            std::fprintf(stderr, "[platform][android] SDL_GetWindowWMInfo failed: %s\n", SDL_GetError());
            SDL_DestroyWindow(g_window);
            g_window = nullptr;
            g_android_native_window = nullptr;
            return {};
        }
        g_android_native_window = android_info.info.android.window;
        const int display_index = SDL_GetWindowDisplayIndex(g_window);
        SDL_DisplayMode display_mode{};
        if (display_index >= 0 &&
            SDL_GetCurrentDisplayMode(display_index, &display_mode) == 0 &&
            display_mode.refresh_rate > 0) {
            g_android_display_rate.store(display_mode.refresh_rate, std::memory_order_release);
        } else {
            g_android_display_rate.store(60, std::memory_order_release);
        }
#endif
    }

#if defined(_WIN32)
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(g_window, &info) != SDL_TRUE) {
        std::fprintf(stderr, "[platform] SDL_GetWindowWMInfo failed: %s\n", SDL_GetError());
        return {};
    }
    return ultramodern::renderer::WindowHandle{info.info.win.window, GetCurrentThreadId()};
#elif defined(__linux__) || defined(__ANDROID__)
    return g_window;
#elif defined(__APPLE__)
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(g_window, &info) != SDL_TRUE) return {};
    static SDL_MetalView metal_view = nullptr;
    if (metal_view == nullptr) metal_view = SDL_Metal_CreateView(g_window);
    return ultramodern::renderer::WindowHandle{info.info.cocoa.window, SDL_Metal_GetLayer(metal_view)};
#endif
}

SDL_Window* rocket::platform::sdl_window() {
    return g_window;
}

#if defined(__ANDROID__)
void* rocket::platform::android_native_window() {
    return g_android_native_window;
}

int rocket::platform::android_display_refresh_rate() {
    return g_android_display_rate.load(std::memory_order_acquire);
}
#endif

void rocket::platform::sample_input() {
    // This function must only run on the SDL/window owner thread.
    std::uint16_t buttons = 0;
    float x = 0.0F;
    float y = 0.0F;
    keyboard_input(buttons, x, y);
    controller_input(g_controller, buttons, x, y);
    std::lock_guard lock(g_input_mutex);
    g_buttons = buttons;
    g_stick_x = std::clamp(x, -1.0F, 1.0F);
    g_stick_y = std::clamp(y, -1.0F, 1.0F);
}

void rocket::platform::pump_runtime_events() {
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT ||
            (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)) {
            ultramodern::quit();
            continue;
        }
        if (event.type == SDL_CONTROLLERDEVICEADDED ||
            event.type == SDL_CONTROLLERDEVICEREMOVED) {
            open_first_controller();
        }
        const bool fullscreen_shortcut = event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
            (event.key.keysym.scancode == SDL_SCANCODE_F11 ||
             (event.key.keysym.scancode == SDL_SCANCODE_RETURN &&
              (event.key.keysym.mod & KMOD_ALT) != 0));
        if (fullscreen_shortcut) {
            toggle_fullscreen();
            continue;
        }
        if (rocket::ui::handle_runtime_event(&event)) {
            continue;
        }
    }
    sample_input();
    rocket::widescreen::update_window_aspect(g_window);
    apply_rumble_request();
}

void rocket::platform::toggle_fullscreen() {
    // Once RT64 owns the swap chain, publish the change through its graphics
    // configuration instead of mutating SDL's native window underneath an
    // in-flight present. The launcher handles its pre-RT64 fullscreen changes
    // directly because no swap chain exists there yet.
    auto graphics = ultramodern::renderer::get_graphics_config();
    const bool fullscreen =
        graphics.wm_option == ultramodern::renderer::WindowMode::Fullscreen;
    graphics.wm_option = fullscreen
        ? ultramodern::renderer::WindowMode::Windowed
        : ultramodern::renderer::WindowMode::Fullscreen;
    ultramodern::renderer::set_graphics_config(graphics);
}

void rocket::platform::queue_samples(std::int16_t* samples, std::size_t sample_count) {
    std::lock_guard lock(g_audio_mutex);
    if (g_audio_device == 0 || samples == nullptr || sample_count == 0) return;
    if ((sample_count & 1U) != 0U) {
        std::fprintf(stderr, "[audio] rejected odd PCM sample count: %zu\n", sample_count);
        return;
    }

    // First advance the emulated AI to the exact instant this DMA is queued.
    // This state is intentionally unrelated to SDL's host queue depth.
    advance_virtual_ai_locked();
    const std::size_t submitted_frames = sample_count / 2U;
    const std::size_t maximum_frames =
        static_cast<std::size_t>(std::max(g_audio_frequency, 48000U));
    if (submitted_frames > maximum_frames) {
        std::fprintf(stderr, "[audio] rejected implausible PCM block: %zu frames\n",
                     submitted_frames);
        return;
    }

    if (g_audio_nominal_block_frames == 0U) {
        g_audio_nominal_block_frames = submitted_frames;
        // Keep roughly two authored blocks buffered on the host. This cushion
        // is deliberately invisible to osAiGetLength/osAiGetStatus.
        g_audio_prime_target_frames = std::max<std::size_t>(
            g_audio_nominal_block_frames * 2U,
            static_cast<std::size_t>(g_audio_callback_frames) * 4U);
    }

    const std::size_t host_queued_before = queued_audio_frames_locked();
    if (g_audio_playback_started && host_queued_before == 0U) {
        ++g_audio_underrun_count;
        if (g_audio_underrun_count <= 4U ||
            (g_audio_underrun_count % 60U) == 0U) {
            std::fprintf(stderr,
                         "[audio] host underrun #%llu; guest AI timing remains continuous\n",
                         static_cast<unsigned long long>(g_audio_underrun_count));
        }
    }

    g_audio_swap.resize(sample_count);
    const float gain = std::clamp(
        g_master_volume.load(std::memory_order_acquire), 0.0F, 1.0F);
    int peak = 0;
    for (std::size_t i = 0; i + 1 < sample_count; i += 2) {
        const auto scale = [gain, &peak](std::int16_t value) {
            peak = std::max(peak, std::abs(static_cast<int>(value)));
            const int scaled = static_cast<int>(
                std::lround(static_cast<float>(value) * gain));
            return static_cast<std::int16_t>(
                std::clamp(scaled, -32768, 32767));
        };
        // RDRAM's word layout leaves a native stereo pair in R,L order.
        g_audio_swap[i] = scale(samples[i + 1]);
        g_audio_swap[i + 1] = scale(samples[i]);
    }

    const auto byte_count = static_cast<Uint32>(
        g_audio_swap.size() * sizeof(std::int16_t));
    if (SDL_QueueAudio(g_audio_device, g_audio_swap.data(), byte_count) != 0) {
        std::fprintf(stderr, "[audio] SDL_QueueAudio failed: %s\n", SDL_GetError());
        return;
    }

    // The N64 AI exposes one active DMA plus one queued DMA. Rocket checks
    // FIFO_FULL before calling osAiSetNextBuffer, so >2 should never persist.
    g_ai_fifo_frames.push_back(submitted_frames);
    ++g_audio_buffer_count;
    if (g_ai_fifo_frames.size() > 2U) {
        std::fprintf(stderr,
                     "[audio] warning: virtual AI FIFO exceeded two slots (%zu)\n",
                     g_ai_fifo_frames.size());
    }
    start_virtual_ai_clock_locked();
    maybe_start_audio_locked();

    if (g_audio_buffer_count <= 8U || (g_audio_buffer_count % 120U) == 0U) {
        const std::size_t host_queued_after = queued_audio_frames_locked();
        std::fprintf(stderr,
                     "[audio] block=%llu frames=%zu host=%zu/%zu fifo=%zu active=%zu peak=%d volume=%.2f underruns=%llu\n",
                     static_cast<unsigned long long>(g_audio_buffer_count),
                     submitted_frames, host_queued_after,
                     g_audio_prime_target_frames, g_ai_fifo_frames.size(),
                     g_ai_fifo_frames.empty() ? 0U : g_ai_fifo_frames.front(),
                     peak, static_cast<double>(gain),
                     static_cast<unsigned long long>(g_audio_underrun_count));
    }
}

std::size_t rocket::platform::frames_remaining() {
    std::lock_guard lock(g_audio_mutex);
    if (g_audio_device == 0) return 0;
    advance_virtual_ai_locked();
    if (g_ai_fifo_frames.empty()) return 0;

    // N64ModernRuntime subtracts half of one mono VI worth of bytes, which is
    // equivalent to one quarter of an NTSC VI in stereo frames. Compensate
    // here so Rocket sees the active N64 DMA itself, not our host cushion.
    const std::size_t samples_per_vi =
        std::max<std::size_t>(g_audio_frequency / 60U, 1U);
    const std::size_t runtime_offset_compensation =
        (samples_per_vi + 3U) / 4U;
    return g_ai_fifo_frames.front() + runtime_offset_compensation;
}

std::uint32_t rocket::platform::audio_status() {
    std::lock_guard lock(g_audio_mutex);
    if (g_audio_device == 0) return 0;
    advance_virtual_ai_locked();
    std::uint32_t status = 0;
    if (!g_ai_fifo_frames.empty()) status |= AI_STATUS_DMA_BUSY;
    if (g_ai_fifo_frames.size() >= 2U) status |= AI_STATUS_FIFO_FULL;
    return status;
}

void rocket::platform::set_frequency(std::uint32_t frequency) {
    if (frequency < 8000U || frequency > 96000U) return;
    std::lock_guard lock(g_audio_mutex);
    if (g_audio_device != 0 && g_audio_frequency == frequency) return;
    open_audio_locked(frequency);
}

float rocket::platform::master_volume() {
    return g_master_volume.load(std::memory_order_acquire);
}

void rocket::platform::set_master_volume(float volume) {
    g_master_volume.store(std::clamp(volume, 0.0F, 1.0F), std::memory_order_release);
}

void rocket::platform::poll_input() {
    // Deliberately empty. SDL_PumpEvents/GetKeyboardState/GameController reads
    // belong to the main thread. pump_runtime_events() publishes the snapshot.
}

bool rocket::platform::get_input(int controller_num, std::uint16_t* buttons,
                                 float* x, float* y) {
    if (controller_num != 0 || buttons == nullptr || x == nullptr || y == nullptr) return false;
    std::lock_guard lock(g_input_mutex);
    *buttons = g_buttons;
    *x = g_stick_x;
    *y = g_stick_y;
    return true;
}

void rocket::platform::set_rumble(int controller_num, bool rumble) {
    if (controller_num != 0) return;
    g_rumble_requested.store(rumble, std::memory_order_release);
    g_rumble_dirty.store(true, std::memory_order_release);
}

ultramodern::input::connected_device_info_t
rocket::platform::get_connected_device_info(int controller_num) {
    if (controller_num != 0) {
        return {ultramodern::input::Device::None, ultramodern::input::Pak::None};
    }
    const bool pad = g_controller_connected.load(std::memory_order_acquire);
    return {ultramodern::input::Device::Controller,
            pad ? ultramodern::input::Pak::RumblePak : ultramodern::input::Pak::None};
}
