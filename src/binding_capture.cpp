#include "binding_capture.hpp"
#include "runtime_input.hpp"
#include <cstdlib>

void rocket::input::BindingCapture::begin(Device target, SDL_JoystickID controller, Clock::time_point now) {
    if (controller != controller_) {
        controller_ = controller;
        buttons_.fill(false);
        axes_.fill(0);
    }
    device = target;
    phase = Phase::Waiting;
    source = kUnbound;
    started_ = now;
    mouse_motion = false;
    mouse_x_ = mouse_y_ = 0;
    for (std::size_t i = 0; i < axes_.size(); ++i) armed_[i] = std::abs(axes_[i]) < 12000;
}

void rocket::input::BindingCapture::capture_mouse_motion(bool enabled, Clock::time_point now) {
    mouse_motion = enabled; mouse_x_ = mouse_y_ = 0; started_ = now;
}

bool rocket::input::BindingCapture::event(const SDL_Event& e, SDL_JoystickID controller, Clock::time_point now, bool over_prompt_control) {
    if (controller != controller_) {
        if (active() && device == Device::Controller) cancel();
        controller_ = controller;
        buttons_.fill(false);
        axes_.fill(0);
    }
    if ((e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) ||
        e.type == SDL_APP_WILLENTERBACKGROUND) {
        if (active()) cancel();
        keys_.fill(false);
        buttons_.fill(false);
        axes_.fill(0);
        mouse_buttons_.fill(false);
        mouse_x_ = mouse_y_ = 0;
        return false;
    }
    const bool waiting = phase == Phase::Waiting;
    const bool settled = now - started_ >= std::chrono::milliseconds(150);
    if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
        if (e.button.which == SDL_TOUCH_MOUSEID || e.button.button >= mouse_buttons_.size()) return false;
        const bool held = mouse_buttons_[e.button.button];
        mouse_buttons_[e.button.button] = e.type == SDL_MOUSEBUTTONDOWN;
        if (waiting && device == Device::Keyboard && settled && !held && !over_prompt_control && e.type == SDL_MOUSEBUTTONDOWN) {
            source = encode_mouse_button(e.button.button);
            if (source != kUnbound) phase = Phase::Ready;
            return true;
        }
        return false; // Keep popup Cancel/Remove clickable while waiting.
    }
    if (waiting && device == Device::Keyboard && settled && e.type == SDL_MOUSEWHEEL && e.wheel.which != SDL_TOUCH_MOUSEID) {
        const int sign = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
        if (e.wheel.y) source = encode_mouse_wheel(e.wheel.y * sign > 0 ? MouseDirection::Up : MouseDirection::Down);
        else if (e.wheel.x) source = encode_mouse_wheel(e.wheel.x * sign > 0 ? MouseDirection::Right : MouseDirection::Left);
        if (source != kUnbound) phase = Phase::Ready;
        return true;
    }
    if (waiting && device == Device::Keyboard && mouse_motion && settled && e.type == SDL_MOUSEMOTION && e.motion.which != SDL_TOUCH_MOUSEID) {
        mouse_x_ += e.motion.xrel; mouse_y_ += e.motion.yrel;
        if (std::abs(mouse_x_) >= 60 || std::abs(mouse_y_) >= 60) {
            source = encode_mouse_motion(std::abs(mouse_x_) > std::abs(mouse_y_)
                ? (mouse_x_ > 0 ? MouseDirection::Right : MouseDirection::Left)
                : (mouse_y_ > 0 ? MouseDirection::Down : MouseDirection::Up));
            phase = Phase::Ready;
        }
        return true;
    }
    if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
        const int key = e.key.keysym.scancode;
        if (key < 0 || key >= SDL_NUM_SCANCODES) return false;
        const bool held = keys_[key];
        keys_[key] = e.type == SDL_KEYDOWN;
        if (!active()) return false;
        if (e.type == SDL_KEYDOWN && key == SDL_SCANCODE_ESCAPE) { cancel(); return true; }
        if (!waiting) return false;
        if (e.type == SDL_KEYDOWN && !e.key.repeat && !held && settled) {
            if (key == SDL_SCANCODE_BACKSPACE || key == SDL_SCANCODE_DELETE) {
                source = kUnbound;
                phase = Phase::Ready;
            } else if (device == Device::Keyboard) {
                source = key;
                phase = Phase::Ready;
            }
        }
        return true;
    }
    if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) {
        if (e.cbutton.which != controller_) return active();
        const unsigned button = e.cbutton.button;
        if (button >= buttons_.size()) return false;
        const bool held = buttons_[button];
        buttons_[button] = e.type == SDL_CONTROLLERBUTTONDOWN;
        if (waiting && device == Device::Controller && !held && settled && e.type == SDL_CONTROLLERBUTTONDOWN) {
            source = encode_controller_button(button);
            phase = Phase::Ready;
        }
        return waiting;
    }
    if (e.type == SDL_CONTROLLERAXISMOTION) {
        if (e.caxis.which != controller_) return active();
        const unsigned axis = e.caxis.axis;
        if (axis >= axes_.size()) return false;
        axes_[axis] = e.caxis.value;
        if (std::abs(axes_[axis]) < 12000) armed_[axis] = true;
        if (waiting && device == Device::Controller && settled && armed_[axis] && std::abs(axes_[axis]) >= 24000) {
            source = encode_controller_axis(axis, axes_[axis] > 0);
            phase = Phase::Ready;
        }
        return waiting;
    }
    return false;
}
