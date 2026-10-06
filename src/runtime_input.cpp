#include "runtime_input.hpp"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <map>

namespace {
std::atomic<bool> g_camera_input_owned{false};
std::atomic<bool> g_camera_runtime_enabled{true};
std::atomic<bool> g_camera_actions_active{false};
std::atomic<bool> g_camera_mouse_enabled{true};
std::atomic<bool> g_camera_mouse_supported{false};
std::atomic<float> g_camera_mouse_sensitivity{0.15F};
std::atomic<int> g_camera_mouse_recenter{SDL_BUTTON_MIDDLE};

using rocket::input::Action;
using rocket::input::BindingSlot;
using rocket::input::ShortcutAction;

constexpr int kAxisSourceBase = 1000;
constexpr int kMouseButtonBase = 2000, kMouseWheelBase = 2100, kMouseMotionBase = 2200;
Uint32 g_mouse_buttons = 0;
std::array<Uint64, 5> g_mouse_click_until{};
std::array<Uint64, 4> g_mouse_wheel_until{};
float g_mouse_x = 0, g_mouse_y = 0;
Uint64 g_mouse_motion_until = 0;
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

bool KeyboardSlot(BindingSlot slot) {
    return slot == BindingSlot::KeyboardPrimary || slot == BindingSlot::KeyboardSecondary;
}
int& SlotValue(BindingSet& set, BindingSlot slot) {
    switch (slot) {
        case BindingSlot::KeyboardPrimary: return set.keyboard_primary;
        case BindingSlot::KeyboardSecondary: return set.keyboard_secondary;
        case BindingSlot::ControllerPrimary: return set.controller_primary;
        case BindingSlot::ControllerSecondary: return set.controller_secondary;
    }
    return set.keyboard_primary;
}

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
struct ModActionEntry {rocket::input::ModAction binding;rocket::input::ModActionState state;bool held=false;};
struct ModActions {bool enabled=false;std::vector<ModActionEntry> entries;};
std::map<std::string,ModActions> g_mod_actions;
void PublishModAction(ModActionEntry& entry,float value) {
    const bool held=value>0.5F;
    if(held&&!entry.held)++entry.state.presses;
    if(!held&&entry.held)++entry.state.releases;
    entry.held=held;entry.state.value=value;
}
struct CameraBinding { int keyboard; int controller; };
using CameraBindings = std::array<CameraBinding, static_cast<std::size_t>(rocket::input::CameraAction::Count)>;
constexpr CameraBindings kCameraDefaults{{
    {SDL_SCANCODE_I, kAxisSourceBase + SDL_CONTROLLER_AXIS_RIGHTY * 2},
    {SDL_SCANCODE_K, kAxisSourceBase + SDL_CONTROLLER_AXIS_RIGHTY * 2 + 1},
    {SDL_SCANCODE_J, kAxisSourceBase + SDL_CONTROLLER_AXIS_RIGHTX * 2},
    {SDL_SCANCODE_L, kAxisSourceBase + SDL_CONTROLLER_AXIS_RIGHTX * 2 + 1},
    {SDL_SCANCODE_U, SDL_CONTROLLER_BUTTON_RIGHTSTICK},
    {SDL_SCANCODE_O, SDL_CONTROLLER_BUTTON_LEFTSTICK},
    {SDL_SCANCODE_P, SDL_CONTROLLER_BUTTON_Y},
}};
auto g_camera_bindings = kCameraDefaults;
constexpr std::array<const char*, kCameraDefaults.size()> kCameraIdentifiers{{"up", "down", "left", "right", "recenter", "cycle_zoom", "first_person"}};
constexpr std::array<const char*, kCameraDefaults.size()> kCameraLabels{{"Look up", "Look down", "Look left", "Look right", "Recenter camera", "Cycle zoom", "Toggle first person"}};

bool CameraUses(const CameraBindings& bindings, int source, bool keyboard) {
    if (source < 0) return false;
    for (std::size_t i=0; i<bindings.size(); ++i) {
        const bool mode_button = i >= static_cast<std::size_t>(rocket::input::CameraAction::CycleZoom);
        if (!(mode_button ? g_camera_actions_active.load() && g_camera_runtime_enabled.load() : rocket::input::camera_input_owned())) continue;
        const auto& b = bindings[i];
        if ((keyboard ? b.keyboard : b.controller) == source) return true;
    }
    return false;
}

constexpr std::array<const char*, static_cast<std::size_t>(Action::Count)> kIdentifiers{{
    "stick_up", "stick_down", "stick_left", "stick_right", "a", "b", "z",
    "start", "dpad_up", "dpad_down", "dpad_left", "dpad_right", "l", "r",
    "c_up", "c_down", "c_left", "c_right",
}};

constexpr std::array<const char*, static_cast<std::size_t>(Action::Count)> kLabels{{
    "Stick up", "Stick down", "Stick left", "Stick right", "A button",
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

float DesktopValue(const Uint8* keys, int source) {
    if (!keys) return 0;
    if (source >= 0 && source < SDL_NUM_SCANCODES) return keys[source] ? 1.0F : 0.0F;
    const auto now = SDL_GetTicks64();
    const int button = source - kMouseButtonBase;
    if (button >= 1 && button <= 5)
        return (g_mouse_buttons & SDL_BUTTON(button)) || now < g_mouse_click_until[button-1] ? 1.0F : 0.0F;
    const int wheel = source - kMouseWheelBase;
    if (wheel >= 0 && wheel < 4) return now < g_mouse_wheel_until[wheel] ? 1.0F : 0.0F;
    const int motion = source - kMouseMotionBase;
    if (motion >= 0 && motion < 4 && now < g_mouse_motion_until) {
        const float values[] = {-g_mouse_y, g_mouse_y, -g_mouse_x, g_mouse_x};
        return std::clamp(values[motion] / 24.0F, 0.0F, 1.0F);
    }
    return 0;
}
bool KeyboardHeld(const Uint8* keys, int source) {
    return DesktopValue(keys, source) > 0.5F;
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

std::vector<rocket::input::BindingLocation> rocket::input::binding_conflicts(Action action, BindingSlot slot, int source) {
    std::vector<BindingLocation> result;
    if (source == kUnbound) return result;
    std::scoped_lock lock(g_binding_mutex);
    for (std::size_t i = 0; i < g_bindings.size(); ++i) {
        if (i == Index(action)) continue;
        for (auto other : {BindingSlot::KeyboardPrimary, BindingSlot::KeyboardSecondary,
                           BindingSlot::ControllerPrimary, BindingSlot::ControllerSecondary}) {
            if (KeyboardSlot(slot) == KeyboardSlot(other) && SlotValue(g_bindings[i], other) == source) {
                result.push_back({static_cast<Action>(i), other});
            }
        }
    }
    return result;
}

bool rocket::input::swap_binding(Action action, BindingSlot slot, int source, BindingLocation conflict) {
    std::scoped_lock lock(g_binding_mutex);
    if (action == conflict.action || KeyboardSlot(slot) != KeyboardSlot(conflict.slot)) return false;
    int& target = SlotValue(g_bindings[Index(action)], slot);
    int& other = SlotValue(g_bindings[Index(conflict.action)], conflict.slot);
    if (source == kUnbound || other != source) return false;
    std::swap(target, other);
    return true;
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
    const int button = scancode - kMouseButtonBase;
    if (button >= 1 && button <= 5) {
        constexpr const char* names[] = {"Mouse left click", "Mouse middle click", "Mouse right click", "Mouse side button 1", "Mouse side button 2"};
        return names[button-1];
    }
    constexpr const char* directions[] = {"up", "down", "left", "right"};
    if (scancode >= kMouseWheelBase && scancode < kMouseWheelBase + 4)
        return std::string("Mouse wheel ") + directions[scancode-kMouseWheelBase];
    if (is_mouse_motion(scancode)) return std::string("Mouse move ") + directions[scancode-kMouseMotionBase];
    if (scancode < 0 || scancode >= SDL_NUM_SCANCODES) return "Not assigned";
    const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
    return name != nullptr && *name != '\0' ? name : "Unknown key";
}

std::string rocket::input::controller_binding_name(int source) {
    static constexpr std::array<const char*, SDL_CONTROLLER_BUTTON_MAX> names{
        "Bottom face button", "Right face button", "Left face button", "Top face button",
        "Back / Select", "Guide / Home", "Menu / Start", "Left stick click", "Right stick click",
        "Left bumper", "Right bumper", "D-pad up", "D-pad down", "D-pad left", "D-pad right",
        "Extra button", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad click"};
    if (source >= 0 && source < SDL_CONTROLLER_BUTTON_MAX) {
        return names[source] != nullptr ? names[source] : "Controller button";
    }
    if (source >= kAxisSourceBase) {
        const int encoded = source - kAxisSourceBase;
        const int axis = encoded / 2;
        if (axis >= 0 && axis < SDL_CONTROLLER_AXIS_MAX) {
            const bool positive = (encoded & 1) != 0;
            switch (axis) {
                case SDL_CONTROLLER_AXIS_LEFTX: return positive ? "Left stick right" : "Left stick left";
                case SDL_CONTROLLER_AXIS_LEFTY: return positive ? "Left stick down" : "Left stick up";
                case SDL_CONTROLLER_AXIS_RIGHTX: return positive ? "Right stick right" : "Right stick left";
                case SDL_CONTROLLER_AXIS_RIGHTY: return positive ? "Right stick down" : "Right stick up";
                case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return positive ? "Left trigger" : "Left trigger (negative axis)";
                case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return positive ? "Right trigger" : "Right trigger (negative axis)";
            }
        }
    }
    return "Not assigned";
}

rocket::input::State rocket::input::poll(SDL_GameController* controller,
                                          bool include_keyboard,
                                          bool include_controller,
                                          bool blocked,
                                          bool allow_shortcuts, State* preview) {
    State state{};
    const Uint8* keys = include_keyboard ? SDL_GetKeyboardState(nullptr) : nullptr;
    SDL_GameController* pad = include_controller ? controller : nullptr;
    UpdateShortcutRequests(pad, keys, allow_shortcuts);
    if (blocked && preview == nullptr) {sample_mod_n64(0,true);return state;}

    std::array<BindingSet, static_cast<std::size_t>(Action::Count)> bindings;
    CameraBindings camera_bindings;
    {
        std::scoped_lock lock(g_binding_mutex);
        bindings = g_bindings;
        camera_bindings = g_camera_bindings;
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
        if (preview == nullptr && is_mouse_motion(source) && camera_input_owned() && camera_mouse_enabled() && camera_mouse_supported()) return true;
        if (preview == nullptr && CameraUses(camera_bindings, source, true)) return true;
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
        const auto suppressed = [&](int source) {
            return controller_suppressed(source) || (
                CameraUses(camera_bindings, source, false) && preview == nullptr);
        };
        float result = 0.0F;
        if (!suppressed(b.controller_primary)) {
            result = SourceValue(pad, b.controller_primary, analogue_stick);
        }
        if (!suppressed(b.controller_secondary)) {
            result = std::max(result, SourceValue(
                pad, b.controller_secondary, analogue_stick));
        }
        if (!keyboard_suppressed(b.keyboard_primary)) result = std::max(result, DesktopValue(keys, b.keyboard_primary));
        if (!keyboard_suppressed(b.keyboard_secondary)) result = std::max(result, DesktopValue(keys, b.keyboard_secondary));
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
    if (preview == nullptr && g_camera_actions_active.load() && g_camera_runtime_enabled.load()) {
        // Feed the original held-button state. Rocket detects presses/releases,
        // cycles its own zoom presets, and handles first-person transitions.
        const auto camera = poll_camera(controller, include_keyboard, include_controller, blocked);
        if (camera.cycle_zoom) state.buttons |= kCDown;
        if (camera.first_person) state.buttons |= kCUp;
    }

    state.stick_x = std::clamp(value(Action::StickRight) - value(Action::StickLeft), -1.0F, 1.0F);
    state.stick_y = std::clamp(value(Action::StickUp) - value(Action::StickDown), -1.0F, 1.0F);
    const float magnitude = std::sqrt(state.stick_x * state.stick_x + state.stick_y * state.stick_y);
    if (magnitude > 1.0F) {
        state.stick_x /= magnitude;
        state.stick_y /= magnitude;
    }
    state.stick_x = ShapeStick(state.stick_x, stick_x_inverted());
    state.stick_y = ShapeStick(state.stick_y, stick_y_inverted());
    if (preview != nullptr) *preview = state;
    if(preview==nullptr) {
        std::lock_guard lock(g_binding_mutex);
        for(auto& [owner,mod]:g_mod_actions) for(auto& action:mod.entries) {
            float amount=0;
            if(mod.enabled&&!blocked) {
                const auto& binding=action.binding;
                if(!keyboard_suppressed(binding.keyboard)) amount=DesktopValue(keys,binding.keyboard);
                if(!controller_suppressed(binding.controller)&&!CameraUses(camera_bindings,binding.controller,false))amount=std::max(amount,SourceValue(pad,binding.controller));
                if(binding.n64&&(state.buttons&binding.n64))amount=1;
            }
            PublishModAction(action,amount);
        }
    }
    return blocked ? State{} : state;
}

void rocket::input::clear_mod_actions(){std::lock_guard lock(g_binding_mutex);g_mod_actions.clear();}
void rocket::input::register_mod_actions(const std::string& owner,const std::vector<ModAction>& actions){std::lock_guard lock(g_binding_mutex);auto& target=g_mod_actions[owner];target={};for(const auto& action:actions)target.entries.push_back({action,{}});}
void rocket::input::set_mod_actions_enabled(const std::string& owner,bool enabled){std::lock_guard lock(g_binding_mutex);auto found=g_mod_actions.find(owner);if(found==g_mod_actions.end())return;found->second.enabled=enabled;if(!enabled)for(auto& action:found->second.entries)PublishModAction(action,0);}
void rocket::input::set_mod_action_binding(const std::string& owner,const std::string& name,bool keyboard,int source){std::lock_guard lock(g_binding_mutex);auto found=g_mod_actions.find(owner);if(found==g_mod_actions.end())return;for(auto& action:found->second.entries)if(action.binding.id==name){(keyboard?action.binding.keyboard:action.binding.controller)=source;PublishModAction(action,0);}}
void rocket::input::set_mod_action_touch(const std::string& owner,const std::string& name,std::uint16_t mask){std::lock_guard lock(g_binding_mutex);auto found=g_mod_actions.find(owner);if(found==g_mod_actions.end())return;for(auto& action:found->second.entries)if(action.binding.id==name){action.binding.n64=mask;PublishModAction(action,0);}}
rocket::input::ModActionState rocket::input::mod_action_state(const std::string& owner,const std::string& name){std::lock_guard lock(g_binding_mutex);auto found=g_mod_actions.find(owner);if(found!=g_mod_actions.end())for(const auto& action:found->second.entries)if(action.binding.id==name)return action.state;return {};}
void rocket::input::sample_mod_n64(std::uint16_t buttons,bool blocked){std::lock_guard lock(g_binding_mutex);for(auto& [owner,mod]:g_mod_actions)for(auto& action:mod.entries)if(blocked||action.binding.n64)PublishModAction(action,!blocked&&mod.enabled&&(buttons&action.binding.n64)?1.0F:0.0F);}

void rocket::input::set_camera_input_owned(bool value) { g_camera_input_owned.store(value); }
bool rocket::input::camera_input_owned() { return g_camera_input_owned.load() && g_camera_runtime_enabled.load(); }
void rocket::input::set_camera_runtime_enabled(bool enabled) { g_camera_runtime_enabled.store(enabled); }
void rocket::input::set_camera_actions_active(bool active) { g_camera_actions_active.store(active); }
bool rocket::input::camera_mouse_enabled() { return g_camera_mouse_enabled.load(); }
bool rocket::input::camera_mouse_supported() { return g_camera_mouse_supported.load(); }
void rocket::input::set_camera_mouse_supported(bool value) { g_camera_mouse_supported.store(value); }
void rocket::input::set_camera_mouse_enabled(bool value) { g_camera_mouse_enabled.store(value); }
float rocket::input::camera_mouse_sensitivity() { return g_camera_mouse_sensitivity.load(); }
void rocket::input::set_camera_mouse_sensitivity(float value) {
    if (std::isfinite(value)) g_camera_mouse_sensitivity.store(std::clamp(value, 0.01F, 1.0F));
}
int rocket::input::camera_mouse_recenter_button() { return g_camera_mouse_recenter.load(); }
void rocket::input::set_camera_mouse_recenter_button(int button) {
    if (button >= 0 && button <= SDL_BUTTON_X2) g_camera_mouse_recenter.store(button);
}

std::size_t rocket::input::camera_action_count() { return kCameraDefaults.size(); }
const char* rocket::input::camera_action_identifier(CameraAction action) {
    return kCameraIdentifiers.at(static_cast<std::size_t>(action));
}
const char* rocket::input::camera_action_label(CameraAction action) {
    return kCameraLabels.at(static_cast<std::size_t>(action));
}
int rocket::input::camera_binding(CameraAction action, bool keyboard) {
    std::scoped_lock lock(g_binding_mutex);
    const auto& b = g_camera_bindings.at(static_cast<std::size_t>(action));
    return keyboard ? b.keyboard : b.controller;
}
void rocket::input::set_camera_binding(CameraAction action, bool keyboard, int source) {
    const bool valid = source == kUnbound || (keyboard
        ? (source >= 0 && source < SDL_NUM_SCANCODES) || is_mouse_source(source)
        : (source >= 0 && source < SDL_CONTROLLER_BUTTON_MAX) ||
          (source >= kAxisSourceBase && source < kAxisSourceBase + SDL_CONTROLLER_AXIS_MAX * 2));
    if (!valid) return;
    std::scoped_lock lock(g_binding_mutex);
    auto& b = g_camera_bindings.at(static_cast<std::size_t>(action));
    (keyboard ? b.keyboard : b.controller) = source;
    if (keyboard && action != CameraAction::Recenter && source == encode_mouse_button(camera_mouse_recenter_button()))
        set_camera_mouse_recenter_button(0);
}
void rocket::input::reset_camera_bindings() {
    std::scoped_lock lock(g_binding_mutex);
    g_camera_bindings = kCameraDefaults;
}
rocket::input::CameraState rocket::input::poll_camera(SDL_GameController* controller,
    bool include_keyboard, bool include_controller, bool blocked) {
    if (blocked) return {};
    CameraBindings bindings;
    { std::scoped_lock lock(g_binding_mutex); bindings = g_camera_bindings; }
    const Uint8* keys = include_keyboard ? SDL_GetKeyboardState(nullptr) : nullptr;
    auto* pad = include_controller ? controller : nullptr;
    const auto reserved = [&](int source, bool keyboard) {
        if (source < 0) return true;
        for (std::size_t i=0; i<g_shortcut_keyboard.size(); ++i)
            if (source == (keyboard ? g_shortcut_keyboard[i].load() : g_shortcut_controller[i].load())) return true;
        return keyboard && source == SDL_SCANCODE_ESCAPE;
    };
    const auto value = [&](CameraAction action) {
        const auto& b = bindings[static_cast<std::size_t>(action)];
        const bool direct_mouse = is_mouse_motion(b.keyboard) && camera_mouse_enabled() && camera_mouse_supported() &&
            static_cast<std::size_t>(action) < static_cast<std::size_t>(CameraAction::Recenter);
        const float key = !reserved(b.keyboard, true) && !direct_mouse ? DesktopValue(keys, b.keyboard) : 0.0F;
        return std::max(key, reserved(b.controller, false) ? 0.0F : SourceValue(pad, b.controller));
    };
    return {value(CameraAction::Right)-value(CameraAction::Left),
            value(CameraAction::Down)-value(CameraAction::Up), value(CameraAction::Recenter)>0.5F,
            value(CameraAction::CycleZoom)>0.5F, value(CameraAction::FirstPerson)>0.5F};
}

int rocket::input::encode_mouse_button(int button) { return button >= 1 && button <= 5 ? kMouseButtonBase + button : kUnbound; }
int rocket::input::encode_mouse_wheel(MouseDirection direction) { return kMouseWheelBase + static_cast<int>(direction); }
int rocket::input::encode_mouse_motion(MouseDirection direction) { return kMouseMotionBase + static_cast<int>(direction); }
bool rocket::input::is_mouse_motion(int source) { return source >= kMouseMotionBase && source < kMouseMotionBase + 4; }
bool rocket::input::is_mouse_source(int source) {
    return (source > kMouseButtonBase && source <= kMouseButtonBase + 5) ||
        (source >= kMouseWheelBase && source < kMouseWheelBase + 4) || is_mouse_motion(source);
}
bool rocket::input::mouse_motion_bound() {
    std::scoped_lock lock(g_binding_mutex);
    for (const auto& b : g_bindings)
        if (is_mouse_motion(b.keyboard_primary) || is_mouse_motion(b.keyboard_secondary)) return true;
    if (camera_input_owned()) for (const auto& b : g_camera_bindings) if (is_mouse_motion(b.keyboard)) return true;
    for(const auto& [owner,mod]:g_mod_actions)if(mod.enabled)for(const auto& action:mod.entries)if(is_mouse_motion(action.binding.keyboard))return true;
    return false;
}
void rocket::input::clear_mouse_transients() {
    g_mouse_click_until.fill(0); g_mouse_wheel_until.fill(0);
    g_mouse_x = g_mouse_y = 0; g_mouse_motion_until = 0;
}
void rocket::input::mouse_event(const SDL_Event& e) {
    const auto now = SDL_GetTicks64();
    if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
        if (e.button.which == SDL_TOUCH_MOUSEID || e.button.button < 1 || e.button.button > 5) return;
        const auto mask = SDL_BUTTON(e.button.button);
        if (e.type == SDL_MOUSEBUTTONDOWN) {
            g_mouse_buttons |= mask;
            // A quick click must survive until the next 30 Hz game update.
            g_mouse_click_until[e.button.button-1] = now + 50;
        } else g_mouse_buttons &= ~mask;
    } else if (e.type == SDL_MOUSEWHEEL && e.wheel.which != SDL_TOUCH_MOUSEID) {
        const int sign = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
        if (e.wheel.y) g_mouse_wheel_until[e.wheel.y * sign > 0 ? 0 : 1] = now + 50;
        if (e.wheel.x) g_mouse_wheel_until[e.wheel.x * sign < 0 ? 2 : 3] = now + 50;
    } else if (e.type == SDL_MOUSEMOTION && e.motion.which != SDL_TOUCH_MOUSEID) {
        if (now >= g_mouse_motion_until) g_mouse_x = g_mouse_y = 0;
        if (g_mouse_x * e.motion.xrel < 0) g_mouse_x = 0;
        if (g_mouse_y * e.motion.yrel < 0) g_mouse_y = 0;
        g_mouse_x = std::clamp(g_mouse_x + e.motion.xrel, -240.0F, 240.0F);
        g_mouse_y = std::clamp(g_mouse_y + e.motion.yrel, -240.0F, 240.0F);
        g_mouse_motion_until = now + 50;
    } else if ((e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) || e.type == SDL_APP_WILLENTERBACKGROUND) {
        g_mouse_buttons = 0; clear_mouse_transients();
    }
}
