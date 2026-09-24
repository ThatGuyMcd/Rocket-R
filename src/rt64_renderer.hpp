#pragma once

#include "ultramodern/renderer_context.hpp"
#include "graphics_enhancements.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <memory>
#include <mutex>

namespace RT64 { struct Application; }

namespace rocket::renderer {

class RT64Context final : public ultramodern::renderer::RendererContext {
public:
    RT64Context(std::uint8_t* rdram,
                ultramodern::renderer::WindowHandle window_handle,
                bool developer_mode);
    ~RT64Context() override;

    bool valid() override { return static_cast<bool>(application_); }
    bool update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                       const ultramodern::renderer::GraphicsConfig& new_config) override;
    void enable_instant_present() override;
    void send_dl(const OSTask* task, std::uint8_t* rdram_snapshot) override;
    void update_screen() override;
    void shutdown() override;
    std::uint32_t get_display_framerate() const override;
    float get_resolution_scale() const override;

private:
    void apply_extra_graphics(bool force);
    void update_performance_stats();

    std::unique_ptr<RT64::Application> application_;
    mutable std::mutex presentation_mutex_;
    std::uint64_t present_count_ = 0;
    std::uint64_t interpolated_present_count_ = 0;
    bool interpolation_confirmed_logged_ = false;

    std::uint64_t graphics_revision_ = 0U;
    int startup_anisotropy_ = 16;
    rocket::graphics::DisplayBuffering startup_buffering_ =
        rocket::graphics::DisplayBuffering::Triple;
    std::string startup_custom_shader_;
    bool restart_notice_logged_ = false;
    std::chrono::steady_clock::time_point performance_window_started_{};
    std::uint64_t performance_present_base_ = 0U;
};

std::unique_ptr<ultramodern::renderer::RendererContext> create_rt64_context(
    std::uint8_t* rdram,
    ultramodern::renderer::WindowHandle window_handle,
    bool developer_mode);

} // namespace rocket::renderer
