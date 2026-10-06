#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct _SDL_GameController;
union SDL_Event;
typedef struct _SDL_GameController SDL_GameController;

namespace rocket::input {

struct ModAction {
    std::string id;
    int keyboard=-1,controller=-1;
    std::uint16_t n64=0;
};
struct ModActionState {float value=0;std::uint32_t presses=0,releases=0;};
void clear_mod_actions();
void register_mod_actions(const std::string& owner,const std::vector<ModAction>& actions);
void set_mod_actions_enabled(const std::string& owner,bool enabled);
void set_mod_action_binding(const std::string& owner,const std::string& action,bool keyboard,int source);
void set_mod_action_touch(const std::string& owner,const std::string& action,std::uint16_t mask);
ModActionState mod_action_state(const std::string& owner,const std::string& action);
// Android's mapped touch buttons are published through this same fallback.
void sample_mod_n64(std::uint16_t buttons,bool blocked);

constexpr int kUnbound = -1;
void set_camera_input_owned(bool owned);
bool camera_input_owned();
void set_camera_runtime_enabled(bool enabled);
void set_camera_actions_active(bool active);
enum class CameraAction : std::uint8_t { Up, Down, Left, Right, Recenter, CycleZoom, FirstPerson, Count };
struct CameraState { float x=0, y=0; bool recenter=false, cycle_zoom=false, first_person=false; };
std::size_t camera_action_count();
const char* camera_action_identifier(CameraAction action);
const char* camera_action_label(CameraAction action);
int camera_binding(CameraAction action, bool keyboard);
void set_camera_binding(CameraAction action, bool keyboard, int source);
void reset_camera_bindings();
bool camera_mouse_enabled();
bool camera_mouse_supported();
void set_camera_mouse_supported(bool supported);
void set_camera_mouse_enabled(bool enabled);
float camera_mouse_sensitivity();
void set_camera_mouse_sensitivity(float degrees_per_pixel);
int camera_mouse_recenter_button();
void set_camera_mouse_recenter_button(int button);
CameraState poll_camera(SDL_GameController* controller, bool include_keyboard,
                        bool include_controller, bool blocked);

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
struct BindingLocation { Action action; BindingSlot slot; };
std::vector<BindingLocation> binding_conflicts(Action action, BindingSlot slot, int source);
bool swap_binding(Action action, BindingSlot slot, int source, BindingLocation conflict);

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
enum class MouseDirection : std::uint8_t { Up, Down, Left, Right };
int encode_mouse_button(int button);
int encode_mouse_wheel(MouseDirection direction);
int encode_mouse_motion(MouseDirection direction);
bool is_mouse_source(int source);
bool is_mouse_motion(int source);
bool mouse_motion_bound();
// SDL owner thread only, before polling controls.
void mouse_event(const SDL_Event& event);
void clear_mouse_transients();
std::string keyboard_binding_name(int scancode);
std::string controller_binding_name(int source);

State poll(SDL_GameController* controller, bool include_keyboard,
           bool include_controller, bool blocked,
           bool allow_shortcuts = true, State* preview = nullptr);

} // namespace rocket::input
