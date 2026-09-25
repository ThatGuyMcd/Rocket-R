#include "crash_handler.hpp"
#include "game_registration.hpp"
#include "platform.hpp"
#include "rt64_renderer.hpp"
#include "runtime_ui.hpp"
#include "widescreen_culling.hpp"

#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"
#include "ultramodern/ultramodern.hpp"

#if defined(__ANDROID__)
#include <SDL_system.h>
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

#ifndef ROCKET_R_VERSION
#define ROCKET_R_VERSION "0.1.0-dev"
#endif

extern RspUcodeFunc rocketAspMain;

namespace {

RspExitReason empty_audio_task(std::uint8_t*, std::uint32_t) {
    return RspExitReason::Broke;
}

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    if (task == nullptr) return nullptr;
    if (task->t.type == M_AUDTASK) {
        if (task->t.ucode != rocket::kAudioUcodeVram) {
            std::fprintf(stderr,
                         "[rsp] unexpected audio ucode: 0x%08X (expected 0x%08X)\n",
                         task->t.ucode, rocket::kAudioUcodeVram);
            return nullptr;
        }
        if (task->t.data_size == 0) return empty_audio_task;
        return rocketAspMain;
    }
    std::fprintf(stderr,
                 "[rsp] unsupported non-graphics task type=%u ucode=0x%08X data=0x%08X size=%u\n",
                 task->t.type, task->t.ucode, task->t.data_ptr, task->t.data_size);
    return nullptr;
}

void message_box(const char* message) {
    std::fprintf(stderr, "[runtime-error] %s\n", message != nullptr ? message : "(null)");
}

std::string thread_name(const OSThread* thread) {
    if (thread == nullptr) return "Rocket";
    return "Rocket-" + std::to_string(thread->id);
}

std::filesystem::path default_config_directory() {
#if defined(__ANDROID__)
    if (const char* internal = SDL_AndroidGetInternalStoragePath(); internal && *internal) {
        return std::filesystem::path(internal) / "rocket-r";
    }
#elif defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata) {
        return std::filesystem::path(appdata) / "Rocket-R";
    }
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "rocket-r";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config" / "rocket-r";
    }
#endif
    return std::filesystem::current_path() / "rocket-r-data";
}

struct Options {
    std::filesystem::path rom;
    std::filesystem::path config = default_config_directory();
    bool help = false;
};

bool parse_options(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--rom" && i + 1 < argc) {
            out.rom = std::filesystem::u8path(argv[++i]);
        } else if (arg == "--config" && i + 1 < argc) {
            out.config = std::filesystem::u8path(argv[++i]);
        } else if (!arg.empty() && arg.front() != '-' && out.rom.empty()) {
            out.rom = std::filesystem::u8path(argv[i]);
        } else if (arg == "--help" || arg == "-h") {
            out.help = true;
        } else {
            std::fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]);
            return false;
        }
    }
    return true;
}

bool valid_window_handle(const ultramodern::renderer::WindowHandle& handle) {
#if defined(_WIN32)
    return handle.window != nullptr;
#elif defined(__APPLE__)
    return handle.window != nullptr && handle.view != nullptr;
#else
    return handle != nullptr;
#endif
}

} // namespace

int rocket_main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::fprintf(stderr, "Rocket: Robot on Wheels - Recompiled %s (FIXED34 runtime; FIXED27 interpolation baseline)\n",
                 ROCKET_R_VERSION);

    Options options{};
    if (!parse_options(argc, argv, options) || options.help) {
        std::fprintf(stderr,
                     "Usage: Rocket-R [--rom <your Rocket US ROM>] [--config <folder>]\n"
                     "Without --rom, the Rocket-R launcher will ask for your ROM.\n");
        return options.help ? 0 : 2;
    }

    std::error_code ec;
    std::filesystem::create_directories(options.config, ec);
    if (ec) {
        std::fprintf(stderr, "Could not create config directory: %s\n", ec.message().c_str());
        return 3;
    }
    rocket::diagnostics::install(options.config);

    std::string error;
    if (!rocket::register_game(options.config, error)) {
        std::fprintf(stderr, "[boot] %s\n", error.c_str());
        return 4;
    }

    if (!rocket::platform::initialise()) return 5;
    rocket::ui::configure(options.config);

    auto window_handle = rocket::platform::create_window();
    if (!valid_window_handle(window_handle)) {
        rocket::platform::shutdown();
        return 5;
    }

    rocket::ui::StartupResult startup{};
#if defined(__ANDROID__)
    if (options.rom.empty()) {
        std::fprintf(stderr, "[android] no private ROM path was supplied by RocketActivity\n");
        rocket::platform::shutdown();
        return 5;
    }
    std::string android_rom_error;
    if (!rocket::select_rom(options.rom, android_rom_error)) {
        std::fprintf(stderr, "[android] private ROM validation failed: %s\n", android_rom_error.c_str());
        rocket::platform::shutdown();
        return 5;
    }
    startup.launch = true;
    startup.rom_path = options.rom;
    std::fprintf(stderr, "[android] private ROM verified; bypassing desktop launcher and starting Rocket directly\n");
#else
    startup = rocket::ui::run_launcher(
        rocket::platform::sdl_window(), options.rom);
#endif
    if (!startup.launch || startup.exit_requested) {
        rocket::platform::shutdown();
        return startup.exit_requested ? 0 : 5;
    }
#if defined(__linux__) && !defined(__ANDROID__)
    // v19: validate the launcher-owned SDL window for Vulkan only after
    // the software ImGui renderer has released it.
    window_handle = rocket::platform::prepare_window_for_game();
    if (!valid_window_handle(window_handle)) {
        std::fprintf(stderr,
            "[boot][linux] launcher-to-Vulkan handoff failed; game was not started\n");
        rocket::platform::shutdown();
        return 5;
    }
#endif
    // Publish the launcher-selected aspect mode/window size before the guest
    // thread can execute its first object frustum test.
    rocket::widescreen::update_window_aspect(rocket::platform::sdl_window());

    recomp::rsp::callbacks_t rsp_callbacks{};
    rsp_callbacks.get_rsp_microcode = get_rsp_microcode;

    ultramodern::renderer::callbacks_t renderer_callbacks{};
    renderer_callbacks.create_render_context = rocket::renderer::create_rt64_context;

    ultramodern::audio_callbacks_t audio_callbacks{};
    audio_callbacks.queue_samples = rocket::platform::queue_samples;
    audio_callbacks.get_frames_remaining = rocket::platform::frames_remaining;
    audio_callbacks.set_frequency = rocket::platform::set_frequency;
    audio_callbacks.get_status = rocket::platform::audio_status;

    ultramodern::input::callbacks_t input_callbacks{};
    input_callbacks.poll_input = rocket::platform::poll_input;
    input_callbacks.get_input = rocket::platform::get_input;
    input_callbacks.set_rumble = rocket::platform::set_rumble;
    input_callbacks.get_connected_device_info = rocket::platform::get_connected_device_info;

    // The launcher owns SDL/window lifetime. Passing the real native handle
    // means N64ModernRuntime must not create or pump a second host window.
    ultramodern::gfx_callbacks_t gfx_callbacks{};

    ultramodern::events::callbacks_t events_callbacks{};
    events_callbacks.vi_callback = nullptr;
    events_callbacks.gfx_init_callback = nullptr;

    ultramodern::error_handling::callbacks_t error_callbacks{};
    error_callbacks.message_box = message_box;

    ultramodern::threads::callbacks_t thread_callbacks{};
    thread_callbacks.get_game_thread_name = thread_name;

    recomp::Version project_version{};
    if (!recomp::Version::from_string(ROCKET_R_VERSION, project_version)) {
        std::fprintf(stderr, "[fatal] invalid Rocket-R VERSION string: %s\n",
                     ROCKET_R_VERSION);
        rocket::platform::shutdown();
        return 6;
    }

    recomp::Configuration config{};
    config.project_version = project_version;
    config.window_handle = window_handle;
    config.rsp_callbacks = rsp_callbacks;
    config.renderer_callbacks = renderer_callbacks;
    config.audio_callbacks = audio_callbacks;
    config.input_callbacks = input_callbacks;
    config.gfx_callbacks = gfx_callbacks;
    config.events_callbacks = events_callbacks;
    config.error_handling_callbacks = error_callbacks;
    config.threads_callbacks = thread_callbacks;
    config.message_queue_control = {};

    std::fprintf(stderr,
                 "[boot] ROM validated; launching N64ModernRuntime on worker thread\n"
                 "[boot] SDL/window/input stay on main thread; Rocket starts on RT64's first safe VI present\n"
                 "[input] F1/Escape overlay | F11/Alt+Enter fullscreen\n");

    std::atomic<bool> runtime_done{false};
    std::exception_ptr runtime_exception;
    std::thread runtime_thread([&]() {
        try {
            recomp::start(config);
        } catch (...) {
            runtime_exception = std::current_exception();
        }
        runtime_done.store(true, std::memory_order_release);
    });

    while (!runtime_done.load(std::memory_order_acquire)) {
        rocket::platform::pump_runtime_events();
        ultramodern::sleep_milliseconds(1);
    }
    runtime_thread.join();

    int result = 0;
    if (runtime_exception) {
        try {
            std::rethrow_exception(runtime_exception);
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "[fatal] runtime exception: %s\n", ex.what());
        } catch (...) {
            std::fprintf(stderr, "[fatal] unknown runtime exception\n");
        }
        result = 6;
    }

    rocket::platform::shutdown();
    return result;
}

#if defined(__ANDROID__)
extern "C" int SDL_main(int argc, char** argv) {
    return rocket_main(argc, argv);
}
#else
int main(int argc, char** argv) {
    return rocket_main(argc, argv);
}
#endif
