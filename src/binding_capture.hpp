#pragma once

#include <array>
#include <chrono>
#include <SDL.h>

namespace rocket::input {

// The UI serializes this state between SDL's owner thread and the renderer.
// Capture only proposes a source; the UI commits it after conflict resolution.
class BindingCapture {
public:
    enum class Device { Keyboard, Controller };
    enum class Phase { Idle, Waiting, Ready, Cancelled };
    using Clock = std::chrono::steady_clock;
    void begin(Device device, SDL_JoystickID controller, Clock::time_point now);
    void capture_mouse_motion(bool enabled, Clock::time_point now);
    bool mouse_motion = false;
    bool event(const SDL_Event& event, SDL_JoystickID controller, Clock::time_point now, bool over_prompt_control = false);
    void cancel() { phase = Phase::Cancelled; }
    void finish() { phase = Phase::Idle; }
    bool active() const { return phase == Phase::Waiting || phase == Phase::Ready; }

    Phase phase = Phase::Idle;
    Device device = Device::Keyboard;
    int source = -1;
private:
    SDL_JoystickID controller_ = -1;
    Clock::time_point started_{};
    std::array<bool, SDL_NUM_SCANCODES> keys_{};
    std::array<bool, 6> mouse_buttons_{};
    int mouse_x_ = 0, mouse_y_ = 0;
    std::array<bool, SDL_CONTROLLER_BUTTON_MAX> buttons_{};
    std::array<int, SDL_CONTROLLER_AXIS_MAX> axes_{};
    std::array<bool, SDL_CONTROLLER_AXIS_MAX> armed_{};
};
}
