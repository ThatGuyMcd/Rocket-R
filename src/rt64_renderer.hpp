#pragma once

#include "ultramodern/renderer_context.hpp"

#include <cstdint>
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
    std::unique_ptr<RT64::Application> application_;
    mutable std::mutex presentation_mutex_;
    std::uint64_t present_count_ = 0;
    std::uint64_t interpolated_present_count_ = 0;
    bool interpolation_confirmed_logged_ = false;
};

std::unique_ptr<ultramodern::renderer::RendererContext> create_rt64_context(
    std::uint8_t* rdram,
    ultramodern::renderer::WindowHandle window_handle,
    bool developer_mode);

} // namespace rocket::renderer
