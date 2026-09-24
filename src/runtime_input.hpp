#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct _SDL_GameController;
typedef struct _SDL_GameController SDL_GameController;

namespace rocket::input {

constexpr int kUnbound = -1;

enum class Action : std::uint8_t {
    StickUp,
    StickDown,
    StickLeft,
    StickRight,
    A,
    B,
    Z,
    Start,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    L,
    R,
    CUp,
    CDown,
    CLeft,
    CRight,
    Count,
};

enum class BindingSlot : std::uint8_t {
    KeyboardPrimary,
    KeyboardSecondary,
    ControllerPrimary,
    ControllerSecondary,
};

enum class ShortcutAction : std::uint8_t {
    ToggleOverlay,
    ToggleFullscreen,
    Count,
};

struct State {
    std::uint16_t buttons = 0;
    float stick_x = 0.0F;
    float stick_y = 0.0F;
};

std::size_t action_count();
const char* action_identifier(Action action);
const char* action_label(Action action);

int binding(Action action, BindingSlot slot);
void set_binding(Action action, BindingSlot slot, int source);
void reset_bindings();

int shortcut_keyboard_binding(ShortcutAction action);
int shortcut_controller_binding(ShortcutAction action);
void set_shortcut_keyboard_binding(ShortcutAction action, int source);
void set_shortcut_controller_binding(ShortcutAction action, int source);
bool consume_shortcut_request(ShortcutAction action);
void reset_shortcuts();

float stick_deadzone();
void set_stick_deadzone(float percent);
float stick_anti_deadzone();
void set_stick_anti_deadzone(float percent);
float stick_sensitivity();
void set_stick_sensitivity(float percent);
float stick_curve();
void set_stick_curve(float exponent);
bool stick_x_inverted();
void set_stick_x_inverted(bool inverted);
bool stick_y_inverted();
void set_stick_y_inverted(bool inverted);
float trigger_threshold();
void set_trigger_threshold(float threshold);
bool background_input_enabled();
void set_background_input_enabled(bool enabled);
void reset_stick_settings();

int encode_controller_button(int button);
int encode_controller_axis(int axis, bool positive);
std::string keyboard_binding_name(int scancode);
std::string controller_binding_name(int source);

State poll(SDL_GameController* controller, bool include_keyboard,
           bool include_controller, bool blocked,
           bool allow_shortcuts = true);

} // namespace rocket::input
