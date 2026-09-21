#include "rt64_renderer.hpp"

#include "game_registration.hpp"
#include "platform.hpp"
#include "renderer_snapshot.hpp"
#include "runtime_ui.hpp"
#include "vi_presentation_policy.hpp"

#if defined(_WIN32)
#include <Unknwn.h>
#include <oaidl.h>
#endif

#include "common/rt64_enhancement_configuration.h"
#include "common/rt64_user_configuration.h"
#include "hle/rt64_application.h"
#include "hle/rt64_present_queue.h"
#include "hle/rt64_state.h"
#include "ultramodern/config.hpp"
#include "ultramodern/ultramodern.hpp"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <utility>

namespace {

class CanonicalViPresentationScope {
public:
    explicit CanonicalViPresentationScope(RT64::Application& application)
        : v_start_(application.core.VI_V_START_REG) {
        const auto* width = application.core.VI_WIDTH_REG;
        const auto* y_scale = application.core.VI_Y_SCALE_REG;
        if (v_start_ == nullptr || width == nullptr || y_scale == nullptr ||
            *width != rocket::renderer::presentation::kCanonicalViWidth) {
            return;
        }

        original_v_start_ = *v_start_;
        canonical_v_start_ =
            rocket::renderer::presentation::canonicalise_rocket_v_region(
                original_v_start_, *y_scale);
        if (canonical_v_start_ == original_v_start_) {
            return;
        }

        *v_start_ = canonical_v_start_;
        active_ = true;
        if (!logged_.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr,
                         "[rt64][vi] canonical present 320x240 "
                         "v-start=%08X->%08X inferred=%u->%u\n",
                         original_v_start_, canonical_v_start_,
                         rocket::renderer::presentation::inferred_vi_height(
                             original_v_start_, *y_scale),
                         rocket::renderer::presentation::inferred_vi_height(
                             canonical_v_start_, *y_scale));
        }
    }

    ~CanonicalViPresentationScope() {
        if (active_) {
            *v_start_ = original_v_start_;
        }
    }

    CanonicalViPresentationScope(const CanonicalViPresentationScope&) = delete;
    CanonicalViPresentationScope& operator=(
        const CanonicalViPresentationScope&) = delete;

private:
    inline static std::atomic<bool> logged_{false};
    std::uint32_t* v_start_ = nullptr;
    std::uint32_t original_v_start_ = 0U;
    std::uint32_t canonical_v_start_ = 0U;
    bool active_ = false;
};

std::array<std::uint8_t, 0x40> g_rom_header{};
std::array<std::uint8_t, 0x1000> g_dmem{};
std::array<std::uint8_t, 0x1000> g_imem{};
std::uint32_t g_mi_interrupt = 0;
std::array<std::uint32_t, 8> g_dpc{};

constexpr int kAuthoredPresentationRate = 30;
constexpr int kMinimumPresentationRate = 30;
constexpr int kMaximumPresentationRate = 500;
int g_detected_display_rate = 60;
int g_effective_presentation_rate = kAuthoredPresentationRate;

int clamp_presentation_rate(int value) {
    return std::clamp(value, kMinimumPresentationRate, kMaximumPresentationRate);
}

int detect_display_rate() {
#if defined(__ANDROID__)
    // SDL video/window queries remain on the Activity/main thread.
    return clamp_presentation_rate(rocket::platform::android_display_refresh_rate());
#else
    SDL_Window* window = rocket::platform::sdl_window();
    if (window == nullptr) return 60;
    const int display = SDL_GetWindowDisplayIndex(window);
    SDL_DisplayMode mode{};
    if (display < 0 || SDL_GetCurrentDisplayMode(display, &mode) != 0 ||
        mode.refresh_rate <= 0) {
        return 60;
    }
    return clamp_presentation_rate(mode.refresh_rate);
#endif
}

bool interpolation_requested(const ultramodern::renderer::GraphicsConfig& config) {
    return config.rr_option != ultramodern::renderer::RefreshRate::Original;
}

void check_interrupts() {}

RT64::UserConfiguration::GraphicsAPI to_rt64(ultramodern::renderer::GraphicsApi api) {
    using UM = ultramodern::renderer::GraphicsApi;
    using RT = RT64::UserConfiguration::GraphicsAPI;
    switch (api) {
        case UM::D3D12: return RT::D3D12;
        case UM::Vulkan: return RT::Vulkan;
        case UM::Metal: return RT::Metal;
        default: return RT::Automatic;
    }
}

RT64::UserConfiguration::AspectRatio to_rt64(ultramodern::renderer::AspectRatio ratio) {
    using UM = ultramodern::renderer::AspectRatio;
    using RT = RT64::UserConfiguration::AspectRatio;
    switch (ratio) {
        case UM::Expand: return RT::Expand;
        case UM::Manual: return RT::Manual;
        default: return RT::Original;
    }
}

RT64::UserConfiguration::Antialiasing to_rt64(ultramodern::renderer::Antialiasing aa) {
    using UM = ultramodern::renderer::Antialiasing;
    using RT = RT64::UserConfiguration::Antialiasing;
    switch (aa) {
        case UM::MSAA2X: return RT::MSAA2X;
        case UM::MSAA4X: return RT::MSAA4X;
        case UM::MSAA8X: return RT::MSAA8X;
        default: return RT::None;
    }
}

ultramodern::renderer::SetupResult map_setup(RT64::Application::SetupResult value) {
    using UM = ultramodern::renderer::SetupResult;
    using RT = RT64::Application::SetupResult;
    switch (value) {
        case RT::Success: return UM::Success;
        case RT::DynamicLibrariesNotFound: return UM::DynamicLibrariesNotFound;
        case RT::InvalidGraphicsAPI: return UM::InvalidGraphicsAPI;
        case RT::GraphicsAPINotFound: return UM::GraphicsAPINotFound;
        case RT::GraphicsDeviceNotFound: return UM::GraphicsDeviceNotFound;
    }
    return UM::GraphicsDeviceNotFound;
}

ultramodern::renderer::GraphicsApi map_api(RT64::UserConfiguration::GraphicsAPI api) {
    using UM = ultramodern::renderer::GraphicsApi;
    using RT = RT64::UserConfiguration::GraphicsAPI;
    switch (api) {
        case RT::D3D12: return UM::D3D12;
        case RT::Vulkan: return UM::Vulkan;
        case RT::Metal: return UM::Metal;
        default: return UM::Auto;
    }
}

void apply_config(RT64::Application& app,
                  const ultramodern::renderer::GraphicsConfig& config) {
    app.userConfig.graphicsAPI = to_rt64(config.api_option);
    app.userConfig.aspectRatio = to_rt64(config.ar_option);
    app.userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Original;
    app.userConfig.antialiasing = to_rt64(config.msaa_option);
    app.userConfig.resolution = config.res_option == ultramodern::renderer::Resolution::Original
        ? RT64::UserConfiguration::Resolution::Original
        : config.res_option == ultramodern::renderer::Resolution::Original2x
            ? RT64::UserConfiguration::Resolution::Manual
            : RT64::UserConfiguration::Resolution::WindowIntegerScale;
    app.userConfig.resolutionMultiplier = config.res_option == ultramodern::renderer::Resolution::Original2x
        ? 2.0
        : static_cast<double>(std::max(config.ds_option, 1));
    app.userConfig.downsampleMultiplier = std::max(config.ds_option, 1);

    // Rocket authors one new visual frame every two NTSC retraces (30 Hz).
    // High-refresh output must therefore be presentation-only interpolation;
    // the guest simulation, controller polling and audio remain on the retail
    // timeline. As in DKR-R, resolve Match Display through SDL rather than
    // trusting a newly-created swap chain's provisional refresh estimate.
    g_detected_display_rate = detect_display_rate();
    if (!interpolation_requested(config)) {
        g_effective_presentation_rate = kAuthoredPresentationRate;
        app.userConfig.refreshRate = RT64::UserConfiguration::RefreshRate::Original;
        app.userConfig.refreshRateTarget = kAuthoredPresentationRate;
    } else {
        const int requested = config.rr_option == ultramodern::renderer::RefreshRate::Manual
            ? clamp_presentation_rate(config.rr_manual_value)
            : g_detected_display_rate;
        g_effective_presentation_rate = requested;
        if (requested <= kAuthoredPresentationRate) {
            app.userConfig.refreshRate = RT64::UserConfiguration::RefreshRate::Original;
            app.userConfig.refreshRateTarget = kAuthoredPresentationRate;
        } else {
            app.userConfig.refreshRate = RT64::UserConfiguration::RefreshRate::Manual;
            app.userConfig.refreshRateTarget = requested;
        }
    }
    app.userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;
    switch (config.hpfb_option) {
        case ultramodern::renderer::HighPrecisionFramebuffer::On:
            app.userConfig.internalColorFormat = RT64::UserConfiguration::InternalColorFormat::High;
            break;
        case ultramodern::renderer::HighPrecisionFramebuffer::Off:
            app.userConfig.internalColorFormat = RT64::UserConfiguration::InternalColorFormat::Standard;
            break;
        default:
            app.userConfig.internalColorFormat = RT64::UserConfiguration::InternalColorFormat::Automatic;
            break;
    }
}

} // namespace

rocket::renderer::RT64Context::RT64Context(
    std::uint8_t* rdram,
    ultramodern::renderer::WindowHandle window_handle,
    bool developer_mode) {
    g_rom_header.fill(0);
    g_dmem.fill(0);
    g_imem.fill(0);
    g_mi_interrupt = 0;
    g_dpc.fill(0);

    RT64::Application::Core core{};
#if defined(_WIN32)
    core.window = window_handle.window;
#elif defined(__linux__) && !defined(__ANDROID__)
    core.window = window_handle;
#elif defined(__ANDROID__)
    core.window = static_cast<ANativeWindow*>(rocket::platform::android_native_window());
    if (core.window == nullptr) {
        std::fprintf(stderr, "[rt64][android] cached ANativeWindow is unavailable\n");
        setup_result = ultramodern::renderer::SetupResult::GraphicsAPINotFound;
        return;
    }
#elif defined(__APPLE__)
    core.window.window = window_handle.window;
    core.window.view = window_handle.view;
#endif
    core.checkInterrupts = check_interrupts;
    core.HEADER = g_rom_header.data();
    core.RDRAM = rdram;
    core.DMEM = g_dmem.data();
    core.IMEM = g_imem.data();
    core.MI_INTR_REG = &g_mi_interrupt;
    core.DPC_START_REG = &g_dpc[0];
    core.DPC_END_REG = &g_dpc[1];
    core.DPC_CURRENT_REG = &g_dpc[2];
    core.DPC_STATUS_REG = &g_dpc[3];
    core.DPC_CLOCK_REG = &g_dpc[4];
    core.DPC_BUFBUSY_REG = &g_dpc[5];
    core.DPC_PIPEBUSY_REG = &g_dpc[6];
    core.DPC_TMEM_REG = &g_dpc[7];

    auto* vi = ultramodern::renderer::get_vi_regs();
    core.VI_STATUS_REG = &vi->VI_STATUS_REG;
    core.VI_ORIGIN_REG = &vi->VI_ORIGIN_REG;
    core.VI_WIDTH_REG = &vi->VI_WIDTH_REG;
    core.VI_INTR_REG = &vi->VI_INTR_REG;
    core.VI_V_CURRENT_LINE_REG = &vi->VI_V_CURRENT_LINE_REG;
    core.VI_TIMING_REG = &vi->VI_TIMING_REG;
    core.VI_V_SYNC_REG = &vi->VI_V_SYNC_REG;
    core.VI_H_SYNC_REG = &vi->VI_H_SYNC_REG;
    core.VI_LEAP_REG = &vi->VI_LEAP_REG;
    core.VI_H_START_REG = &vi->VI_H_START_REG;
    core.VI_V_START_REG = &vi->VI_V_START_REG;
    core.VI_V_BURST_REG = &vi->VI_V_BURST_REG;
    core.VI_X_SCALE_REG = &vi->VI_X_SCALE_REG;
    core.VI_Y_SCALE_REG = &vi->VI_Y_SCALE_REG;

    auto graphics = ultramodern::renderer::get_graphics_config();
    RT64::ApplicationConfiguration app_config{};
    app_config.appId = "rocket-r";
    app_config.useConfigurationFile = false;
#if defined(__ANDROID__)
    // Android already supplies an app-private --config directory.
    // Never let RT64 probe the desktop Linux home (/data on Android).
    app_config.detectDataPath = false;
#else
    app_config.detectDataPath = true;
#endif

    const auto create_application = [&]() {
        application_ = std::make_unique<RT64::Application>(core, app_config);
        apply_config(*application_, graphics);
        application_->userConfig.developerMode = developer_mode;
        application_->enhancementConfig.f3dex.forceBranch = true;
        application_->enhancementConfig.presentation.removeBlackBorders = true;
        application_->enhancementConfig.rect.fixRectLR = true;
        application_->enhancementConfig.presentation.mode =
            RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
    };

    create_application();
    std::uint32_t thread_id = 0;
#if defined(_WIN32)
    thread_id = window_handle.thread_id;
#endif
    setup_result = map_setup(application_->setup(thread_id));
    chosen_api = map_api(application_->chosenGraphicsAPI);

    if (setup_result != ultramodern::renderer::SetupResult::Success &&
        graphics.api_option != ultramodern::renderer::GraphicsApi::Auto) {
        std::fprintf(stderr,
                     "[rt64] requested graphics API failed (%d); retrying Automatic\n",
                     static_cast<int>(setup_result));
        application_.reset();
        graphics.api_option = ultramodern::renderer::GraphicsApi::Auto;
        ultramodern::renderer::set_graphics_config(graphics);
        create_application();
        setup_result = map_setup(application_->setup(thread_id));
        chosen_api = map_api(application_->chosenGraphicsAPI);
    }

    if (setup_result != ultramodern::renderer::SetupResult::Success) {
        std::fprintf(stderr, "[rt64] setup failed (%d)\n", static_cast<int>(setup_result));
        application_.reset();
        return;
    }

    application_->setFullScreen(
        graphics.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
    std::fprintf(stderr,
                 "[rt64] renderer ready; game start deferred until first safe VI present\n");
    std::fprintf(stderr,
                 "[rt64][interpolation] authored=%d target=%d display=%d mode=%s\n",
                 kAuthoredPresentationRate, g_effective_presentation_rate,
                 g_detected_display_rate,
                 application_->userConfig.refreshRate ==
                         RT64::UserConfiguration::RefreshRate::Original
                     ? "original"
                     : "interpolated");
}

rocket::renderer::RT64Context::~RT64Context() {
    shutdown();
}

bool rocket::renderer::RT64Context::update_config(
    const ultramodern::renderer::GraphicsConfig& old_config,
    const ultramodern::renderer::GraphicsConfig& new_config) {
    std::scoped_lock presentation_lock(presentation_mutex_);
    if (!application_ || old_config == new_config) return false;
    if (old_config.wm_option != new_config.wm_option) {
        application_->setFullScreen(
            new_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
    }
    const bool framebuffer_change =
        old_config.res_option != new_config.res_option ||
        old_config.ar_option != new_config.ar_option ||
        old_config.ds_option != new_config.ds_option;
    const bool aa_change = old_config.msaa_option != new_config.msaa_option;
    apply_config(*application_, new_config);
    if (aa_change) application_->updateMultisampling();
    application_->updateUserConfig(framebuffer_change);
    if (old_config.rr_option != new_config.rr_option ||
        old_config.rr_manual_value != new_config.rr_manual_value) {
        interpolation_confirmed_logged_ = false;
        interpolated_present_count_ = 0;
        std::fprintf(stderr,
                     "[rt64][interpolation] live target=%d display=%d mode=%s\n",
                     g_effective_presentation_rate, g_detected_display_rate,
                     application_->userConfig.refreshRate ==
                             RT64::UserConfiguration::RefreshRate::Original
                         ? "original"
                         : "interpolated");
    }
    return true;
}

void rocket::renderer::RT64Context::enable_instant_present() {
    std::scoped_lock presentation_lock(presentation_mutex_);
    if (!application_) return;
    application_->enhancementConfig.presentation.mode =
        RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
    application_->updateEnhancementConfig();
}

void rocket::renderer::RT64Context::send_dl(
    const OSTask* task, std::uint8_t* rdram_snapshot) {
    std::scoped_lock presentation_lock(presentation_mutex_);
    if (!application_ || application_->state == nullptr || task == nullptr ||
        rdram_snapshot == nullptr) return;

    // Parse display lists against the immutable memory image captured when the
    // guest submitted this task. Rocket aggressively reuses display-list,
    // matrix and texture allocations after SP completion; decoding live RDRAM
    // here can otherwise turn a valid task into random host-side pointers.
    SnapshotScope snapshot(application_->core.RDRAM,
                           application_->state->RDRAM,
                           rdram_snapshot);
    // The scheduler renders every two 60 Hz retraces. Pin RT64's source
    // cadence to that authored 30 Hz contract whenever interpolation is
    // enabled so a delayed workload cannot be mistaken for 20/15 Hz and cause
    // a burst of catch-up frames. This is the same presentation rule DKR-R
    // uses; it does not alter Rocket's simulation clock.
    if (application_->userConfig.refreshRate !=
        RT64::UserConfiguration::RefreshRate::Original) {
        application_->state->setRefreshRate(kAuthoredPresentationRate);
    }

    application_->state->rsp->reset();
    application_->interpreter->loadUCodeGBI(task->t.ucode & 0x03FFFFFF,
                                             task->t.ucode_data & 0x03FFFFFF,
                                             true);
    application_->processDisplayLists(rdram_snapshot,
                                      task->t.data_ptr & 0x03FFFFFF, 0, true);
}

void rocket::renderer::RT64Context::update_screen() {
    std::scoped_lock presentation_lock(presentation_mutex_);
    if (!application_) return;
    if (application_->sharedQueueResources != nullptr &&
        application_->userConfig.refreshRate !=
            RT64::UserConfiguration::RefreshRate::Original) {
        const std::uint64_t total = application_->sharedQueueResources->
            totalInterpolatedPresentations.load(std::memory_order_relaxed);
        if (total >= interpolated_present_count_) {
            interpolated_present_count_ = total;
        }
        if (!interpolation_confirmed_logged_ && total > 0) {
            interpolation_confirmed_logged_ = true;
            std::fprintf(stderr,
                         "[rt64][interpolation] first interpolated presentation confirmed "
                         "(target=%d)\n",
                         g_effective_presentation_rate);
        }
    }
    ++present_count_;
    if (present_count_ == 1) {
        rocket::start_game_once();
    }
    {
        // Rocket deliberately renders inside an 18x14 N64 safe area. The
        // pinned RT64 presentation patch zooms that authored active region to
        // the output viewport; this scoped VI normalizer also prevents RT64's
        // generic guard-row inference from leaving a thin residual edge.
        CanonicalViPresentationScope vi_scope(*application_);
        application_->updateScreen();
    }
    rocket::ui::draw(*application_);
}

void rocket::renderer::RT64Context::shutdown() {
    std::scoped_lock presentation_lock(presentation_mutex_);
    if (!application_) return;
    rocket::ui::detach(*application_);
    application_->end();
    application_.reset();
}

std::uint32_t rocket::renderer::RT64Context::get_display_framerate() const {
    if (!application_ || !application_->presentQueue ||
        !application_->presentQueue->ext.sharedResources) return 60;
    const auto rate = application_->presentQueue->ext.sharedResources->swapChainRate;
    return rate > 0 ? rate : 60;
}

float rocket::renderer::RT64Context::get_resolution_scale() const {
    if (!application_) return 1.0F;
    if (application_->userConfig.resolution ==
        RT64::UserConfiguration::Resolution::Manual) {
        return static_cast<float>(application_->userConfig.resolutionMultiplier);
    }
    return 1.0F;
}

std::unique_ptr<ultramodern::renderer::RendererContext>
rocket::renderer::create_rt64_context(
    std::uint8_t* rdram,
    ultramodern::renderer::WindowHandle window_handle,
    bool developer_mode) {
    return std::make_unique<RT64Context>(rdram, window_handle, developer_mode);
}
