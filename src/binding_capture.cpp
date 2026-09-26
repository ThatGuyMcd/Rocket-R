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
    for (std::size_t i = 0; i < axes_.size(); ++i) armed_[i] = std::abs(axes_[i]) < 12000;
}

bool rocket::input::BindingCapture::event(const SDL_Event& e, SDL_JoystickID controller, Clock::time_point now) {
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
        return false;
    }
    const bool waiting = phase == Phase::Waiting;
    const bool settled = now - started_ >= std::chrono::milliseconds(150);
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
