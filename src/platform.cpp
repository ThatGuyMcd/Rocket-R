#include "platform.hpp"
#include "runtime_ui.hpp"
#include "runtime_input.hpp"
#include "widescreen_culling.hpp"

#define SDL_MAIN_HANDLED
#include <SDL.h>
#if defined(__ANDROID__)
#include "plume_render_interface.h"
#include <jni.h>
#endif
#if defined(__linux__) && !defined(__ANDROID__)
#include <SDL_vulkan.h>
#endif
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
#include <utility>

namespace {

SDL_Window* g_window = nullptr;
#if defined(__ANDROID__)
void* g_android_native_window = nullptr;
std::atomic<int> g_android_display_rate{60};
std::mutex g_touch_mutex;
rocket::input::State g_touch_state{};
std::atomic<bool> g_touch_overlay_request{false};
std::atomic<int> g_android_output_rate{48000};
std::atomic<float> g_android_focus_gain{1.0F};
SDL_AudioStream* g_android_audio_stream = nullptr;
std::uint32_t g_android_device_rate = 0;
std::vector<std::int16_t> g_android_output_samples;
std::chrono::steady_clock::time_point g_android_audio_retry_after{};
#endif
SDL_GameController* g_controller = nullptr;
std::atomic<bool> g_controller_connected{false};
std::atomic<SDL_JoystickID> g_controller_instance{-1};
rocket::input::State g_input_preview{};
std::string g_preferred_controller_key;
std::string g_active_controller_key;
std::mutex g_input_mutex;
std::uint16_t g_buttons = 0;
float g_stick_x = 0.0F;
float g_stick_y = 0.0F;
std::atomic<bool> g_rumble_requested{false};
std::atomic<bool> g_rumble_dirty{false};
std::atomic<bool> g_rumble_enabled{true};
std::atomic<float> g_rumble_strength{1.0F};

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

struct ControllerDeviceEntry {
    int device_index = -1;
    std::string key;
    std::string name;
};

std::string GuidString(SDL_JoystickGUID guid) {
    char text[33]{};
    SDL_JoystickGetGUIDString(guid, text, static_cast<int>(sizeof(text)));
    return text;
}

std::vector<ControllerDeviceEntry> EnumerateControllers() {
    std::vector<ControllerDeviceEntry> result;
    std::vector<std::pair<std::string, int>> occurrences;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) continue;
        const std::string guid = GuidString(SDL_JoystickGetDeviceGUID(i));
        int ordinal = 0;
        auto it = std::find_if(occurrences.begin(), occurrences.end(),
            [&](const auto& value) { return value.first == guid; });
        if (it == occurrences.end()) {
            occurrences.emplace_back(guid, 1);
        } else {
            ordinal = it->second;
            ++it->second;
        }
        const char* raw_name = SDL_GameControllerNameForIndex(i);
        std::string name = raw_name != nullptr && *raw_name != '\0'
            ? raw_name : "Controller";
        if (ordinal > 0) name += " #" + std::to_string(ordinal + 1);
        result.push_back({i, guid + "#" + std::to_string(ordinal), std::move(name)});
    }
    return result;
}

void close_controller() {
    g_controller_instance.store(-1);
    if (g_controller != nullptr) {
        SDL_GameControllerClose(g_controller);
        g_controller = nullptr;
    }
    g_active_controller_key.clear();
    g_controller_connected.store(false, std::memory_order_release);
}

void open_first_controller() {
    if (g_controller != nullptr && SDL_GameControllerGetAttached(g_controller) &&
        (g_preferred_controller_key.empty() ||
         g_active_controller_key == g_preferred_controller_key)) {
        g_controller_connected.store(true, std::memory_order_release);
        return;
    }
    close_controller();
    const auto devices = EnumerateControllers();
    for (const auto& device : devices) {
        if (!g_preferred_controller_key.empty() &&
            device.key != g_preferred_controller_key) {
            continue;
        }
        g_controller = SDL_GameControllerOpen(device.device_index);
        if (g_controller != nullptr) {
            g_active_controller_key = device.key;
            g_controller_instance.store(SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_controller)));
            g_controller_connected.store(true, std::memory_order_release);
            std::fprintf(stderr, "[input] controller: %s (%s)\n",
                         device.name.c_str(), device.key.c_str());
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
#if defined(__ANDROID__)
    // Report output buffering in guest frames. It never drives the AI clock.
    const std::uint64_t frames = SDL_GetQueuedAudioSize(g_audio_device) /
        (2U * sizeof(std::int16_t));
    return g_android_device_rate == 0 ? 0 :
        static_cast<std::size_t>(frames * g_audio_frequency / g_android_device_rate);
#else
    return SDL_GetQueuedAudioSize(g_audio_device) /
           (2U * sizeof(std::int16_t));
#endif
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
#if defined(__ANDROID__)
    SDL_FreeAudioStream(g_android_audio_stream);
    g_android_audio_stream = nullptr;
#endif
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
#if defined(__ANDROID__)
    // AudioTrack implementations need not accept Rocket's unusual 22,500 Hz
    // rate. Convert only the Android output, continuously across DMA blocks.
    // Guest AI frequency, FIFO progression and desktop output stay unchanged.
    desired.freq = g_android_output_rate.load(std::memory_order_acquire);
    desired.samples = 512;
#endif
    SDL_AudioSpec obtained{};
    g_audio_device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (g_audio_device == 0) {
#if defined(__ANDROID__)
        g_audio_frequency = frequency;
        g_android_audio_retry_after = AudioClock::now() + std::chrono::seconds(2);
#else
        g_audio_frequency = 0;
#endif
        g_audio_callback_frames = 0;
        std::fprintf(stderr, "[audio] SDL_OpenAudioDevice failed at %u Hz: %s\n",
                     frequency, SDL_GetError());
        return;
    }

    // allowed_changes=0 above requires the requested format/rate. Keep the
    // guest AI clock anchored to the rate Rocket asked osAiSetFrequency for.
    g_audio_frequency = frequency;
    g_audio_callback_frames = obtained.samples;
#if defined(__ANDROID__)
    g_android_device_rate = static_cast<std::uint32_t>(obtained.freq);
    g_audio_callback_frames = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(obtained.samples) * frequency + obtained.freq - 1) / obtained.freq);
    if (frequency != g_android_device_rate) {
        g_android_audio_stream = SDL_NewAudioStream(AUDIO_S16SYS, 2, static_cast<int>(frequency),
                                                   AUDIO_S16SYS, 2, obtained.freq);
        if (g_android_audio_stream == nullptr) {
            std::fprintf(stderr, "[audio] Android output conversion failed: %s\n", SDL_GetError());
            SDL_CloseAudioDevice(g_audio_device);
            g_audio_device = 0;
            g_android_audio_retry_after = AudioClock::now() + std::chrono::seconds(2);
            return;
        }
    }
    std::fprintf(stderr, "[audio][android] guest=%u output=%d Hz driver=%s; output conversion only\n",
                 frequency, obtained.freq, SDL_GetCurrentAudioDriver());
#endif
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
    const float strength = std::clamp(
        g_rumble_strength.load(std::memory_order_acquire), 0.0F, 1.0F);
    const Uint16 magnitude = enabled
        ? static_cast<Uint16>(std::lround(strength * 65535.0F)) : 0;
    SDL_GameControllerRumble(g_controller, magnitude, magnitude,
                             enabled ? 250 : 0);
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
#elif defined(__ANDROID__)
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    // The pinned SDL AAudio backend races timestamp polling with device
    // open/close. AudioTrack avoids that Android launch crash.
    SDL_setenv("SDL_AUDIODRIVER", "android", 0);
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
#if defined(__ANDROID__)
        SDL_FreeAudioStream(g_android_audio_stream);
        g_android_audio_stream = nullptr;
#endif
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
    plume::setAndroidNativeWindow(nullptr);
    g_android_native_window = nullptr;
    g_android_display_rate.store(60, std::memory_order_release);
#endif
    SDL_Quit();
}

ultramodern::renderer::WindowHandle rocket::platform::create_window() {
    if (g_window == nullptr) {
        Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#if defined(__ANDROID__)
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#endif
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
        plume::setAndroidNativeWindow(android_info.info.android.window);
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

// === ROCKET-R PLATFORM LAUNCH REPAIR V19 BEGIN ===
// DKR-R-proven launcher -> Vulkan handoff, isolated to desktop Linux.
// The software launcher can succeed even when Vulkan cannot. Validate the same
// SDL window after launcher teardown and recreate it only if required.
ultramodern::renderer::WindowHandle rocket::platform::prepare_window_for_game() {
#if defined(__linux__) && !defined(__ANDROID__)
    auto vulkan_ready = [](SDL_Window* window) -> bool {
        if (window == nullptr) return false;
        const Uint32 flags = SDL_GetWindowFlags(window);
        if ((flags & SDL_WINDOW_VULKAN) == 0U) return false;

        unsigned int extension_count = 0U;
        SDL_ClearError();
        if (SDL_Vulkan_GetInstanceExtensions(window, &extension_count, nullptr) != SDL_TRUE ||
            extension_count == 0U) {
            std::fprintf(stderr,
                "[platform][linux] Vulkan window preflight failed: %s\n",
                SDL_GetError());
            return false;
        }
        std::fprintf(stderr,
            "[platform][linux] Vulkan preflight PASS: flags=0x%08X extensions=%u driver=%s\n",
            static_cast<unsigned>(flags), extension_count,
            SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "unknown");
        return true;
    };

    if (vulkan_ready(g_window)) return create_window();
    if (g_window == nullptr) {
        std::fprintf(stderr, "[platform][linux] launcher handoff has no SDL window\n");
        return {};
    }

    const Uint32 old_flags = SDL_GetWindowFlags(g_window);
    int x=SDL_WINDOWPOS_CENTERED, y=SDL_WINDOWPOS_CENTERED, w=1440, h=900;
    SDL_GetWindowPosition(g_window,&x,&y);
    SDL_GetWindowSize(g_window,&w,&h);
    const bool was_hidden=(old_flags & SDL_WINDOW_HIDDEN)!=0U;
    const bool was_maximized=(old_flags & SDL_WINDOW_MAXIMIZED)!=0U;
    const Uint32 fullscreen_mode=old_flags &
        (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP);

    std::fprintf(stderr,
        "[platform][linux] recreating launcher window for Vulkan handoff; old-flags=0x%08X\n",
        static_cast<unsigned>(old_flags));

    SDL_DestroyWindow(g_window);
    g_window=nullptr;

    Uint32 flags=SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_VULKAN;
    if (was_hidden) flags |= SDL_WINDOW_HIDDEN;
    g_window=SDL_CreateWindow("Rocket-R - Rocket: Robot on Wheels Recompiled",
                              x,y,w,h,flags);
    if (g_window==nullptr) {
        const std::string detail=SDL_GetError();
        std::fprintf(stderr,"[platform][linux] Vulkan recreation failed: %s\n",detail.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
            "Rocket-R - Graphics startup failed",
            ("Rocket-R could not start Vulkan.\n\nDetails: "+detail+
             "\n\nCheck that your graphics driver supports Vulkan and is up to date."
             "").c_str(),
            nullptr);
        return {};
    }

    SDL_SetWindowMinimumSize(g_window,800,600);
    SDL_EventState(SDL_DROPFILE,SDL_ENABLE);
    if (was_maximized) SDL_MaximizeWindow(g_window);
    if (fullscreen_mode!=0U && SDL_SetWindowFullscreen(g_window,fullscreen_mode)!=0) {
        std::fprintf(stderr,"[platform][linux] fullscreen restore failed: %s\n",SDL_GetError());
    }
    rocket::widescreen::update_window_aspect(g_window);

    if (!vulkan_ready(g_window)) {
        const std::string detail=SDL_GetError();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
            "Rocket-R - Vulkan unavailable",
            ("Rocket-R needs a working Vulkan driver to run the game.\n\nDetails: "+
             detail+"").c_str(),
            g_window);
        SDL_DestroyWindow(g_window);
        g_window=nullptr;
        return {};
    }
#endif
    return create_window();
}
// === ROCKET-R PLATFORM LAUNCH REPAIR V19 END ===


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
    // This function must only run on the SDL/window owner thread. Gameplay
    // sees a protected snapshot; rebinding and UI capture never run on the
    // emulated game thread.
    const bool focused = g_window == nullptr ||
        (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    const bool include_keyboard = focused;
    const bool include_controller = focused || rocket::input::background_input_enabled();
    const bool testing = rocket::ui::bindings_test_active();
    rocket::input::State preview{};
    auto state = rocket::input::poll(
        g_controller, include_keyboard, include_controller,
        rocket::ui::overlay_visible() || testing, !rocket::ui::input_capture_active(),
        testing && focused ? &preview : nullptr);
#if defined(__ANDROID__)
    {
        std::lock_guard touch_lock(g_touch_mutex);
        if (focused && !rocket::ui::overlay_visible() && !testing) {
            state.buttons |= g_touch_state.buttons;
            if (g_touch_state.stick_x != 0.0F || g_touch_state.stick_y != 0.0F) {
                state.stick_x = g_touch_state.stick_x;
                state.stick_y = g_touch_state.stick_y;
            }
        } else {
            g_touch_state = {};
        }
    }
#endif
    std::lock_guard lock(g_input_mutex);
    g_input_preview = preview;
    g_buttons = state.buttons;
    g_stick_x = state.stick_x;
    g_stick_y = state.stick_y;
}

void rocket::platform::pump_runtime_events() {
#if defined(__ANDROID__)
    if (g_touch_overlay_request.exchange(false, std::memory_order_acq_rel)) {
        rocket::ui::toggle_overlay();
    }
#endif
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
#if defined(__ANDROID__)
        if (event.type == SDL_APP_WILLENTERBACKGROUND) {
            plume::setAndroidNativeWindow(nullptr);
        } else if (event.type == SDL_APP_DIDENTERFOREGROUND) {
            SDL_SysWMinfo info{};
            SDL_VERSION(&info.version);
            if (SDL_GetWindowWMInfo(g_window, &info) == SDL_TRUE &&
                info.subsystem == SDL_SYSWM_ANDROID && info.info.android.window != nullptr) {
                g_android_native_window = info.info.android.window;
                plume::setAndroidNativeWindow(info.info.android.window);
            }
        }
#endif
        if (event.type == SDL_QUIT ||
            (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)) {
            ultramodern::quit();
            continue;
        }
        if (event.type == SDL_CONTROLLERDEVICEADDED ||
            event.type == SDL_CONTROLLERDEVICEREMOVED) {
            open_first_controller();
        }
        const bool fullscreen_shortcut = !rocket::ui::input_capture_active() && event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
            event.key.keysym.scancode == SDL_SCANCODE_RETURN &&
            (event.key.keysym.mod & KMOD_ALT) != 0;
        if (fullscreen_shortcut) {
            toggle_fullscreen();
            continue;
        }
        if (rocket::ui::handle_runtime_event(&event)) {
            continue;
        }
    }
    sample_input();
    if (rocket::input::consume_shortcut_request(
            rocket::input::ShortcutAction::ToggleOverlay)) {
        rocket::ui::toggle_overlay();
    }
    if (rocket::input::consume_shortcut_request(
            rocket::input::ShortcutAction::ToggleFullscreen)) {
        toggle_fullscreen();
    }
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

bool rocket::platform::controller_connected() {
    return g_controller_connected.load(std::memory_order_acquire);
}

std::int32_t rocket::platform::controller_instance_id() { return g_controller_instance.load(); }

rocket::input::State rocket::platform::input_preview() {
    std::lock_guard lock(g_input_mutex);
    return g_input_preview;
}

std::string rocket::platform::controller_name() {
    if (g_controller == nullptr || !SDL_GameControllerGetAttached(g_controller)) {
        if (!g_preferred_controller_key.empty()) return "Selected controller disconnected.";
        return "No controller connected.";
    }
    const char* name = SDL_GameControllerName(g_controller);
    return name != nullptr && *name != '\0' ? name : "Controller";
}

std::vector<rocket::platform::ControllerChoice> rocket::platform::controller_choices() {
    std::vector<ControllerChoice> result;
    for (const auto& device : EnumerateControllers()) {
        result.push_back({device.key, device.name});
    }
    return result;
}

std::string rocket::platform::preferred_controller_key() {
    return g_preferred_controller_key;
}

void rocket::platform::set_preferred_controller_key(const std::string& key) {
    if (g_preferred_controller_key == key) return;
    g_preferred_controller_key = key;
    open_first_controller();
}

void rocket::platform::rescan_controller() {
    if (g_controller != nullptr && !SDL_GameControllerGetAttached(g_controller)) {
        close_controller();
    }
    open_first_controller();
}

bool rocket::platform::rumble_enabled() {
    return g_rumble_enabled.load(std::memory_order_acquire);
}

void rocket::platform::set_rumble_enabled(bool enabled) {
    g_rumble_enabled.store(enabled, std::memory_order_release);
    g_rumble_dirty.store(true, std::memory_order_release);
}

float rocket::platform::rumble_strength() {
    return g_rumble_strength.load(std::memory_order_acquire);
}

void rocket::platform::set_rumble_strength(float strength) {
    g_rumble_strength.store(std::clamp(strength, 0.0F, 1.0F),
                            std::memory_order_release);
    g_rumble_dirty.store(true, std::memory_order_release);
}

void rocket::platform::test_rumble() {
    if (g_controller == nullptr || !SDL_GameControllerGetAttached(g_controller) ||
        !g_rumble_enabled.load(std::memory_order_acquire)) return;
    const float strength = std::clamp(
        g_rumble_strength.load(std::memory_order_acquire), 0.0F, 1.0F);
    const Uint16 magnitude = static_cast<Uint16>(
        std::lround(strength * 65535.0F));
    SDL_GameControllerRumble(g_controller, magnitude, magnitude, 350);
}

void rocket::platform::queue_samples(std::int16_t* samples, std::size_t sample_count) {
    std::lock_guard lock(g_audio_mutex);
#if defined(__ANDROID__)
    // Open only when real PCM arrives, avoiding a placeholder-rate Android
    // AudioTrack open/close during startup. Desktop audio retains V47 timing.
    if (g_audio_device == 0 && g_audio_frequency != 0 && samples != nullptr && sample_count != 0 &&
        AudioClock::now() >= g_android_audio_retry_after) {
        open_audio_locked(g_audio_frequency);
    }
#endif
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
<<<<<<< Updated upstream
=======
    const float gain = volume * kMaximumOutputGain
#if defined(__ANDROID__)
        * g_android_focus_gain.load(std::memory_order_acquire)
#endif
        ;
>>>>>>> Stashed changes
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
    const void* output_data = g_audio_swap.data();
    Uint32 output_bytes = byte_count;
#if defined(__ANDROID__)
    if (g_android_audio_stream != nullptr) {
        if (SDL_AudioStreamPut(g_android_audio_stream, g_audio_swap.data(), static_cast<int>(byte_count)) < 0) {
            std::fprintf(stderr, "[audio] Android stream input failed: %s\n", SDL_GetError());
            return;
        }
        const int available = SDL_AudioStreamAvailable(g_android_audio_stream);
        if (available < 0) return;
        g_android_output_samples.resize(static_cast<std::size_t>(available) / sizeof(std::int16_t));
        const int received = available > 0 ? SDL_AudioStreamGet(g_android_audio_stream,
            g_android_output_samples.data(), available) : 0;
        if (received < 0) return;
        output_data = g_android_output_samples.data();
        output_bytes = static_cast<Uint32>(received);
    }
#endif
    if (output_bytes != 0 && SDL_QueueAudio(g_audio_device, output_data, output_bytes) != 0) {
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
#if defined(__ANDROID__)
    if (g_audio_device == 0) { g_audio_frequency = frequency; return; }
#endif
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

#if defined(__ANDROID__)
// Java UI callbacks publish data only. SDL and ImGui remain on their owner
// threads; holding a touch never blocks the game or opens an audio device.
extern "C" JNIEXPORT void JNICALL
Java_com_rocketret_rocketr_RocketActivity_nativeTouchState(JNIEnv*, jclass, jint buttons, jfloat x, jfloat y) {
    std::lock_guard lock(g_touch_mutex);
    g_touch_state.buttons = static_cast<std::uint16_t>(buttons);
    g_touch_state.stick_x = std::isfinite(x) ? std::clamp(x, -1.0F, 1.0F) : 0.0F;
    g_touch_state.stick_y = std::isfinite(y) ? std::clamp(y, -1.0F, 1.0F) : 0.0F;
}

extern "C" JNIEXPORT void JNICALL
Java_com_rocketret_rocketr_RocketActivity_nativeToggleOverlay(JNIEnv*, jclass) {
    g_touch_overlay_request.store(true, std::memory_order_release);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_rocketret_rocketr_RocketActivity_nativeOverlayVisible(JNIEnv*, jclass) {
    return rocket::ui::overlay_visible() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_rocketret_rocketr_RocketActivity_nativeOutputRate(JNIEnv*, jclass, jint rate) {
    if (rate >= 8000 && rate <= 96000) g_android_output_rate.store(rate, std::memory_order_release);
}

extern "C" JNIEXPORT void JNICALL
Java_com_rocketret_rocketr_RocketActivity_nativeAudioFocus(JNIEnv*, jclass, jfloat gain) {
    g_android_focus_gain.store(std::isfinite(gain) ? std::clamp(gain, 0.0F, 1.0F) : 0.0F,
                               std::memory_order_release);
}
#endif
