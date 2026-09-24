#include "runtime_input.hpp"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>

namespace {

using rocket::input::Action;
using rocket::input::BindingSlot;
using rocket::input::ShortcutAction;

constexpr int kAxisSourceBase = 1000;
constexpr std::uint16_t kButtonA = 0x8000;
constexpr std::uint16_t kButtonB = 0x4000;
constexpr std::uint16_t kButtonZ = 0x2000;
constexpr std::uint16_t kButtonStart = 0x1000;
constexpr std::uint16_t kDpadUp = 0x0800;
constexpr std::uint16_t kDpadDown = 0x0400;
constexpr std::uint16_t kDpadLeft = 0x0200;
constexpr std::uint16_t kDpadRight = 0x0100;
constexpr std::uint16_t kButtonL = 0x0020;
constexpr std::uint16_t kButtonR = 0x0010;
constexpr std::uint16_t kCUp = 0x0008;
constexpr std::uint16_t kCDown = 0x0004;
constexpr std::uint16_t kCLeft = 0x0002;
constexpr std::uint16_t kCRight = 0x0001;

struct BindingSet {
    int keyboard_primary = rocket::input::kUnbound;
    int keyboard_secondary = rocket::input::kUnbound;
    int controller_primary = rocket::input::kUnbound;
    int controller_secondary = rocket::input::kUnbound;
};

constexpr std::array<BindingSet, static_cast<std::size_t>(Action::Count)> kDefaults{{
    {SDL_SCANCODE_W, rocket::input::kUnbound, kAxisSourceBase + SDL_CONTROLLER_AXIS_LEFTY * 2, rocket::input::kUnbound},
    {SDL_SCANCODE_S, rocket::input::kUnbound, kAxisSourceBase + SDL_CONTROLLER_AXIS_LEFTY * 2 + 1, rocket::input::kUnbound},
    {SDL_SCANCODE_A, rocket::input::kUnbound, kAxisSourceBase + SDL_CONTROLLER_AXIS_LEFTX * 2, rocket::input::kUnbound},
    {SDL_SCANCODE_D, rocket::input::kUnbound, kAxisSourceBase + SDL_CONTROLLER_AXIS_LEFTX * 2 + 1, rocket::input::kUnbound},
    {SDL_SCANCODE_SPACE, SDL_SCANCODE_X, SDL_CONTROLLER_BUTTON_A, rocket::input::kUnbound},
    {SDL_SCANCODE_Z, SDL_SCANCODE_LCTRL, SDL_CONTROLLER_BUTTON_B, rocket::input::kUnbound},
    {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT, kAxisSourceBase + SDL_CONTROLLER_AXIS_TRIGGERLEFT * 2 + 1, rocket::input::kUnbound},
    {SDL_SCANCODE_RETURN, rocket::input::kUnbound, SDL_CONTROLLER_BUTTON_START, rocket::input::kUnbound},
    {SDL_SCANCODE_UP, rocket::input::kUnbound, SDL_CONTROLLER_BUTTON_DPAD_UP, rocket::input::kUnbound},
    {SDL_SCANCODE_DOWN, rocket::input::kUnbound, SDL_CONTROLLER_BUTTON_DPAD_DOWN, rocket::input::kUnbound},
    {SDL_SCANCODE_LEFT, rocket::input::kUnbound, SDL_CONTROLLER_BUTTON_DPAD_LEFT, rocket::input::kUnbound},
    {SDL_SCANCODE_RIGHT, rocket::input::kUnbound, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, rocket::input::kUnbound},
    {SDL_SCANCODE_Q, rocket::input::kUnbound, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, rocket::input::kUnbound},
    {SDL_SCANCODE_E, rocket::input::kUnbound, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, rocket::input::kUnbound},
    {SDL_SCANCODE_I, rocket::input::kUnbound, SDL_CONTROLLER_BUTTON_Y, kAxisSourceBase + SDL_CONTROLLER_AXIS_RIGHTY * 2},
    {SDL_SCANCODE_K, rocket::input::kUnbound, SDL_CONTROLLER_BUTTON_RIGHTSTICK, kAxisSourceBase + SDL_CONTROLLER_AXIS_RIGHTY * 2 + 1},
    {SDL_SCANCODE_J, rocket::input::kUnbound, SDL_CONTROLLER_BUTTON_X, kAxisSourceBase + SDL_CONTROLLER_AXIS_RIGHTX * 2},
    {SDL_SCANCODE_L, rocket::input::kUnbound, kAxisSourceBase + SDL_CONTROLLER_AXIS_RIGHTX * 2 + 1, rocket::input::kUnbound},
}};

std::array<BindingSet, static_cast<std::size_t>(Action::Count)> g_bindings = kDefaults;
std::mutex g_binding_mutex;

constexpr std::array<const char*, static_cast<std::size_t>(Action::Count)> kIdentifiers{{
    "stick_up", "stick_down", "stick_left", "stick_right", "a", "b", "z",
    "start", "dpad_up", "dpad_down", "dpad_left", "dpad_right", "l", "r",
    "c_up", "c_down", "c_left", "c_right",
}};

constexpr std::array<const char*, static_cast<std::size_t>(Action::Count)> kLabels{{
    "Analogue up", "Analogue down", "Analogue left", "Analogue right", "A button",
    "B button", "Z trigger", "Start", "D-pad up", "D-pad down", "D-pad left",
    "D-pad right", "L shoulder", "R shoulder", "C-up", "C-down", "C-left", "C-right",
}};

std::array<std::atomic<int>, static_cast<std::size_t>(ShortcutAction::Count)> g_shortcut_keyboard{{
    SDL_SCANCODE_F1, SDL_SCANCODE_F11,
}};
std::array<std::atomic<int>, static_cast<std::size_t>(ShortcutAction::Count)> g_shortcut_controller{{
    SDL_CONTROLLER_BUTTON_BACK, rocket::input::kUnbound,
}};
std::array<std::atomic<bool>, static_cast<std::size_t>(ShortcutAction::Count)> g_shortcut_held{};
std::array<std::atomic<bool>, static_cast<std::size_t>(ShortcutAction::Count)> g_shortcut_requested{};

std::atomic<float> g_stick_deadzone{21.363F};
std::atomic<float> g_stick_anti_deadzone{0.0F};
std::atomic<float> g_stick_sensitivity{100.0F};
std::atomic<float> g_stick_curve{1.0F};
std::atomic<bool> g_stick_x_inverted{false};
std::atomic<bool> g_stick_y_inverted{false};
std::atomic<float> g_trigger_threshold{0.36624F};
std::atomic<bool> g_background_input_enabled{false};

std::size_t Index(Action action) {
    return std::min(static_cast<std::size_t>(action),
                    static_cast<std::size_t>(Action::Count) - 1U);
}

std::size_t ShortcutIndex(ShortcutAction action) {
    return std::min(static_cast<std::size_t>(action),
                    static_cast<std::size_t>(ShortcutAction::Count) - 1U);
}

float NormaliseAxis(Sint16 value, bool apply_stick_deadzone) {
    const int magnitude = std::min(
        std::abs(static_cast<int>(value)), 32767);
    const int deadzone = apply_stick_deadzone
        ? static_cast<int>(std::lround(
              std::clamp(g_stick_deadzone.load(std::memory_order_relaxed),
                         0.0F, 35.0F) * 32767.0F / 100.0F))
        : 0;
    if (magnitude <= deadzone) return 0.0F;
    const int range = std::max(32767 - deadzone, 1);
    const float scaled = static_cast<float>(magnitude - deadzone) /
                         static_cast<float>(range);
    return std::copysign(std::min(scaled, 1.0F), static_cast<float>(value));
}

float SourceValue(SDL_GameController* controller, int source,
                  bool apply_stick_deadzone = false) {
    if (controller == nullptr || source < 0) return 0.0F;
    if (source < SDL_CONTROLLER_BUTTON_MAX) {
        return SDL_GameControllerGetButton(
                   controller, static_cast<SDL_GameControllerButton>(source)) != 0
            ? 1.0F : 0.0F;
    }
    if (source < kAxisSourceBase) return 0.0F;
    const int encoded = source - kAxisSourceBase;
    const int axis = encoded / 2;
    if (axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX) return 0.0F;
    const bool positive = (encoded & 1) != 0;
    const float value = NormaliseAxis(SDL_GameControllerGetAxis(
        controller, static_cast<SDL_GameControllerAxis>(axis)),
        apply_stick_deadzone);
    return positive ? std::max(value, 0.0F) : std::max(-value, 0.0F);
}

bool KeyboardHeld(const Uint8* keys, int source) {
    return keys != nullptr && source >= 0 && source < SDL_NUM_SCANCODES &&
           keys[source] != 0;
}

float ShapeStick(float value, bool inverted) {
    const float magnitude = std::fabs(value);
    if (magnitude <= 0.0F) return 0.0F;
    const float anti = std::clamp(
        g_stick_anti_deadzone.load(std::memory_order_relaxed) / 100.0F,
        0.0F, 0.5F);
    const float curve = std::clamp(
        g_stick_curve.load(std::memory_order_relaxed), 0.5F, 2.5F);
    const float sensitivity = std::clamp(
        g_stick_sensitivity.load(std::memory_order_relaxed) / 100.0F,
        0.5F, 1.5F);
    float shaped = anti + (1.0F - anti) * std::pow(magnitude, curve);
    shaped = std::clamp(shaped * sensitivity, 0.0F, 1.0F);
    return std::copysign(shaped, inverted ? -value : value);
}

void UpdateShortcutRequests(SDL_GameController* controller, const Uint8* keys,
                            bool allow_requests) {
    for (std::size_t i = 0; i < g_shortcut_held.size(); ++i) {
        const int key = g_shortcut_keyboard[i].load(std::memory_order_relaxed);
        const int pad = g_shortcut_controller[i].load(std::memory_order_relaxed);
        const bool held = KeyboardHeld(keys, key) || SourceValue(controller, pad) > 0.5F;
        const bool was_held = g_shortcut_held[i].exchange(held, std::memory_order_acq_rel);
        if (allow_requests && held && !was_held) {
            g_shortcut_requested[i].store(true, std::memory_order_release);
        }
    }
}

} // namespace

std::size_t rocket::input::action_count() {
    return static_cast<std::size_t>(Action::Count);
}

const char* rocket::input::action_identifier(Action action) {
    return kIdentifiers[Index(action)];
}

const char* rocket::input::action_label(Action action) {
    return kLabels[Index(action)];
}

int rocket::input::binding(Action action, BindingSlot slot) {
    std::scoped_lock lock(g_binding_mutex);
    const BindingSet& b = g_bindings[Index(action)];
    switch (slot) {
        case BindingSlot::KeyboardPrimary: return b.keyboard_primary;
        case BindingSlot::KeyboardSecondary: return b.keyboard_secondary;
        case BindingSlot::ControllerPrimary: return b.controller_primary;
        case BindingSlot::ControllerSecondary: return b.controller_secondary;
    }
    return kUnbound;
}

void rocket::input::set_binding(Action action, BindingSlot slot, int source) {
    std::scoped_lock lock(g_binding_mutex);
    BindingSet& b = g_bindings[Index(action)];
    switch (slot) {
        case BindingSlot::KeyboardPrimary: b.keyboard_primary = source; break;
        case BindingSlot::KeyboardSecondary: b.keyboard_secondary = source; break;
        case BindingSlot::ControllerPrimary: b.controller_primary = source; break;
        case BindingSlot::ControllerSecondary: b.controller_secondary = source; break;
    }
}

void rocket::input::reset_bindings() {
    std::scoped_lock lock(g_binding_mutex);
    g_bindings = kDefaults;
}

int rocket::input::shortcut_keyboard_binding(ShortcutAction action) {
    return g_shortcut_keyboard[ShortcutIndex(action)].load(std::memory_order_relaxed);
}

int rocket::input::shortcut_controller_binding(ShortcutAction action) {
    return g_shortcut_controller[ShortcutIndex(action)].load(std::memory_order_relaxed);
}

void rocket::input::set_shortcut_keyboard_binding(ShortcutAction action, int source) {
    g_shortcut_keyboard[ShortcutIndex(action)].store(source, std::memory_order_relaxed);
}

void rocket::input::set_shortcut_controller_binding(ShortcutAction action, int source) {
    g_shortcut_controller[ShortcutIndex(action)].store(source, std::memory_order_relaxed);
}

bool rocket::input::consume_shortcut_request(ShortcutAction action) {
    return g_shortcut_requested[ShortcutIndex(action)].exchange(false, std::memory_order_acq_rel);
}

void rocket::input::reset_shortcuts() {
    g_shortcut_keyboard[ShortcutIndex(ShortcutAction::ToggleOverlay)].store(SDL_SCANCODE_F1, std::memory_order_relaxed);
    g_shortcut_controller[ShortcutIndex(ShortcutAction::ToggleOverlay)].store(SDL_CONTROLLER_BUTTON_BACK, std::memory_order_relaxed);
    g_shortcut_keyboard[ShortcutIndex(ShortcutAction::ToggleFullscreen)].store(SDL_SCANCODE_F11, std::memory_order_relaxed);
    g_shortcut_controller[ShortcutIndex(ShortcutAction::ToggleFullscreen)].store(kUnbound, std::memory_order_relaxed);
}

float rocket::input::stick_deadzone() { return g_stick_deadzone.load(std::memory_order_relaxed); }
void rocket::input::set_stick_deadzone(float percent) { g_stick_deadzone.store(std::clamp(percent, 0.0F, 35.0F), std::memory_order_relaxed); }
float rocket::input::stick_anti_deadzone() { return g_stick_anti_deadzone.load(std::memory_order_relaxed); }
void rocket::input::set_stick_anti_deadzone(float percent) { g_stick_anti_deadzone.store(std::clamp(percent, 0.0F, 50.0F), std::memory_order_relaxed); }
float rocket::input::stick_sensitivity() { return g_stick_sensitivity.load(std::memory_order_relaxed); }
void rocket::input::set_stick_sensitivity(float percent) { g_stick_sensitivity.store(std::clamp(percent, 50.0F, 150.0F), std::memory_order_relaxed); }
float rocket::input::stick_curve() { return g_stick_curve.load(std::memory_order_relaxed); }
void rocket::input::set_stick_curve(float exponent) { g_stick_curve.store(std::clamp(exponent, 0.5F, 2.5F), std::memory_order_relaxed); }
bool rocket::input::stick_x_inverted() { return g_stick_x_inverted.load(std::memory_order_relaxed); }
void rocket::input::set_stick_x_inverted(bool inverted) { g_stick_x_inverted.store(inverted, std::memory_order_relaxed); }
bool rocket::input::stick_y_inverted() { return g_stick_y_inverted.load(std::memory_order_relaxed); }
void rocket::input::set_stick_y_inverted(bool inverted) { g_stick_y_inverted.store(inverted, std::memory_order_relaxed); }
float rocket::input::trigger_threshold() { return g_trigger_threshold.load(std::memory_order_relaxed); }
void rocket::input::set_trigger_threshold(float threshold) { g_trigger_threshold.store(std::clamp(threshold, 0.05F, 0.95F), std::memory_order_relaxed); }
bool rocket::input::background_input_enabled() { return g_background_input_enabled.load(std::memory_order_relaxed); }
void rocket::input::set_background_input_enabled(bool enabled) { g_background_input_enabled.store(enabled, std::memory_order_relaxed); }

void rocket::input::reset_stick_settings() {
    set_stick_deadzone(21.363F);
    set_stick_anti_deadzone(0.0F);
    set_stick_sensitivity(100.0F);
    set_stick_curve(1.0F);
    set_stick_x_inverted(false);
    set_stick_y_inverted(false);
    set_trigger_threshold(0.36624F);
}

int rocket::input::encode_controller_button(int button) {
    return button >= 0 && button < SDL_CONTROLLER_BUTTON_MAX ? button : kUnbound;
}

int rocket::input::encode_controller_axis(int axis, bool positive) {
    return axis >= 0 && axis < SDL_CONTROLLER_AXIS_MAX
        ? kAxisSourceBase + axis * 2 + (positive ? 1 : 0)
        : kUnbound;
}

std::string rocket::input::keyboard_binding_name(int scancode) {
    if (scancode < 0 || scancode >= SDL_NUM_SCANCODES) return "Unbound";
    const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
    return name != nullptr && *name != '\0' ? name : "Unknown key";
}

std::string rocket::input::controller_binding_name(int source) {
    if (source >= 0 && source < SDL_CONTROLLER_BUTTON_MAX) {
        const char* name = SDL_GameControllerGetStringForButton(
            static_cast<SDL_GameControllerButton>(source));
        return name != nullptr ? name : "Unknown button";
    }
    if (source >= kAxisSourceBase) {
        const int encoded = source - kAxisSourceBase;
        const int axis = encoded / 2;
        if (axis >= 0 && axis < SDL_CONTROLLER_AXIS_MAX) {
            const char* name = SDL_GameControllerGetStringForAxis(
                static_cast<SDL_GameControllerAxis>(axis));
            return std::string(name != nullptr ? name : "axis") +
                   ((encoded & 1) != 0 ? " +" : " -");
        }
    }
    return "Unbound";
}

rocket::input::State rocket::input::poll(SDL_GameController* controller,
                                          bool include_keyboard,
                                          bool include_controller,
                                          bool blocked,
                                          bool allow_shortcuts) {
    State state{};
    const Uint8* keys = include_keyboard ? SDL_GetKeyboardState(nullptr) : nullptr;
    SDL_GameController* pad = include_controller ? controller : nullptr;
    UpdateShortcutRequests(pad, keys, allow_shortcuts);
    if (blocked) return state;

    std::array<BindingSet, static_cast<std::size_t>(Action::Count)> bindings;
    {
        std::scoped_lock lock(g_binding_mutex);
        bindings = g_bindings;
    }

    const auto controller_suppressed = [&](int source) {
        if (source == kUnbound) return false;
        for (std::size_t i = 0; i < g_shortcut_held.size(); ++i) {
            if (g_shortcut_held[i].load(std::memory_order_relaxed) &&
                g_shortcut_controller[i].load(std::memory_order_relaxed) == source) {
                return true;
            }
        }
        return false;
    };
    const auto keyboard_suppressed = [&](int source) {
        if (source == kUnbound) return false;
        for (std::size_t i = 0; i < g_shortcut_held.size(); ++i) {
            if (g_shortcut_held[i].load(std::memory_order_relaxed) &&
                g_shortcut_keyboard[i].load(std::memory_order_relaxed) == source) {
                return true;
            }
        }
        return false;
    };
    const auto value = [&](Action action) {
        const BindingSet& b = bindings[Index(action)];
        const bool analogue_stick = action == Action::StickUp ||
            action == Action::StickDown || action == Action::StickLeft ||
            action == Action::StickRight;
        float result = 0.0F;
        if (!controller_suppressed(b.controller_primary)) {
            result = SourceValue(pad, b.controller_primary, analogue_stick);
        }
        if (!controller_suppressed(b.controller_secondary)) {
            result = std::max(result, SourceValue(
                pad, b.controller_secondary, analogue_stick));
        }
        if ((!keyboard_suppressed(b.keyboard_primary) && KeyboardHeld(keys, b.keyboard_primary)) ||
            (!keyboard_suppressed(b.keyboard_secondary) && KeyboardHeld(keys, b.keyboard_secondary))) {
            result = 1.0F;
        }
        return result;
    };
    // Rocket's old right-stick C-button path used a 12,000 raw deadzone and a
    // 0.45 post-deadzone threshold (~65% of the SDL axis range). Preserve that
    // digital-axis feel while buttons still resolve to a full 1.0 immediately.
    const auto press = [&](Action action, std::uint16_t mask, float threshold = 0.6516F) {
        if (value(action) > threshold) state.buttons |= mask;
    };

    press(Action::A, kButtonA);
    press(Action::B, kButtonB);
    press(Action::Z, kButtonZ, trigger_threshold());
    press(Action::Start, kButtonStart);
    press(Action::DpadUp, kDpadUp);
    press(Action::DpadDown, kDpadDown);
    press(Action::DpadLeft, kDpadLeft);
    press(Action::DpadRight, kDpadRight);
    press(Action::L, kButtonL);
    press(Action::R, kButtonR);
    press(Action::CUp, kCUp);
    press(Action::CDown, kCDown);
    press(Action::CLeft, kCLeft);
    press(Action::CRight, kCRight);

    state.stick_x = std::clamp(value(Action::StickRight) - value(Action::StickLeft), -1.0F, 1.0F);
    state.stick_y = std::clamp(value(Action::StickUp) - value(Action::StickDown), -1.0F, 1.0F);
    const float magnitude = std::sqrt(state.stick_x * state.stick_x + state.stick_y * state.stick_y);
    if (magnitude > 1.0F) {
        state.stick_x /= magnitude;
        state.stick_y /= magnitude;
    }
    state.stick_x = ShapeStick(state.stick_x, stick_x_inverted());
    state.stick_y = ShapeStick(state.stick_y, stick_y_inverted());
    return state;
}
