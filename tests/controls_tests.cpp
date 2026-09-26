#define SDL_MAIN_HANDLED
#include "binding_capture.hpp"
#include "controls_studio.hpp"
#include <SDL.h>
#include <bitset>
#include <cstdio>
#include <cstdlib>

using namespace rocket::input;
using Capture = BindingCapture;
using Phase = Capture::Phase;
using Device = Capture::Device;
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s (%s)\n", message, SDL_GetError()); std::exit(1); }
}
SDL_Event key(Uint32 type, SDL_Scancode code, bool repeat = false) {
    SDL_Event e{}; e.type = type; e.key.keysym.scancode = code; e.key.repeat = repeat; return e;
}
SDL_Event button(Uint32 type, int id, Uint8 value) {
    SDL_Event e{}; e.type = type; e.cbutton.which = id; e.cbutton.button = value; return e;
}
SDL_Event axis(int id, Uint8 value, Sint16 amount) {
    SDL_Event e{}; e.type = SDL_CONTROLLERAXISMOTION; e.caxis.which = id;
    e.caxis.axis = value; e.caxis.value = amount; return e;
}
int main() {
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    check(SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) == 0, "SDL input initialization");
    const auto now = Capture::Clock::now();
    const auto settled = now + std::chrono::milliseconds(200);
    Capture capture;
    capture.event(key(SDL_KEYDOWN, SDL_SCANCODE_RETURN), 17, now);
    capture.begin(Device::Keyboard, 17, now);
    capture.event(key(SDL_KEYDOWN, SDL_SCANCODE_RETURN), 17, settled);
    check(capture.phase == Phase::Waiting, "held activation key cannot bind");
    capture.event(key(SDL_KEYUP, SDL_SCANCODE_RETURN), 17, settled);
    capture.event(key(SDL_KEYDOWN, SDL_SCANCODE_RETURN, true), 17, settled);
    check(capture.phase == Phase::Waiting, "key repeat cannot bind");
    capture.event(key(SDL_KEYUP, SDL_SCANCODE_RETURN), 17, settled);
    capture.event(key(SDL_KEYDOWN, SDL_SCANCODE_RETURN), 17, settled);
    check(capture.phase == Phase::Ready && capture.source == SDL_SCANCODE_RETURN, "fresh key proposes input");
    capture.finish();
    capture.event(axis(17, SDL_CONTROLLER_AXIS_LEFTX, 30000), 17, now);
    capture.begin(Device::Controller, 17, now);
    capture.event(axis(17, SDL_CONTROLLER_AXIS_LEFTX, 31000), 17, settled);
    check(capture.phase == Phase::Waiting, "held axis waits for neutral");
    capture.event(axis(17, SDL_CONTROLLER_AXIS_LEFTX, 0), 17, settled);
    capture.event(axis(18, SDL_CONTROLLER_AXIS_LEFTX, -32000), 17, settled);
    check(capture.phase == Phase::Waiting, "another controller ignored");
    capture.event(axis(17, SDL_CONTROLLER_AXIS_LEFTX, -32000), 17, settled);
    check(capture.phase == Phase::Ready && capture.source == encode_controller_axis(SDL_CONTROLLER_AXIS_LEFTX, false), "axis direction captured");
    capture.finish();
    capture.event(button(SDL_CONTROLLERBUTTONDOWN, 17, SDL_CONTROLLER_BUTTON_A), 17, now);
    capture.begin(Device::Controller, 17, now);
    capture.event(button(SDL_CONTROLLERBUTTONDOWN, 17, SDL_CONTROLLER_BUTTON_A), 17, settled);
    check(capture.phase == Phase::Waiting, "held gamepad activation cannot bind");
    capture.event(button(SDL_CONTROLLERBUTTONUP, 17, SDL_CONTROLLER_BUTTON_A), 17, settled);
    capture.event(button(SDL_CONTROLLERBUTTONDOWN, 17, SDL_CONTROLLER_BUTTON_A), 17, settled);
    check(capture.phase == Phase::Ready && capture.source == SDL_CONTROLLER_BUTTON_A, "released button can bind");
    capture.begin(Device::Keyboard, 17, now);
    capture.event(button(SDL_CONTROLLERBUTTONDOWN, 17, SDL_CONTROLLER_BUTTON_Y), 17, settled);
    check(capture.phase == Phase::Waiting, "gamepad cannot fill keyboard slot");
    capture.event(key(SDL_KEYDOWN, SDL_SCANCODE_ESCAPE), 17, settled);
    check(capture.phase == Phase::Cancelled, "Escape cancels");
    capture.begin(Device::Keyboard, 17, now);
    capture.event(key(SDL_KEYDOWN, SDL_SCANCODE_DELETE), 17, settled);
    check(capture.phase == Phase::Ready && capture.source == kUnbound, "Delete proposes clear");
    capture.begin(Device::Controller, 17, now);
    SDL_Event empty{};
    capture.event(empty, -1, settled);
    check(capture.phase == Phase::Cancelled, "controller disconnect cancels");
    capture.begin(Device::Keyboard, 17, now);
    SDL_Event focus{}; focus.type = SDL_WINDOWEVENT; focus.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    capture.event(focus, 17, settled);
    check(capture.phase == Phase::Cancelled, "focus loss cancels");

    std::bitset<18> order;
    for (const auto action : rocket::ui::controls::kSetupOrder) {
        check(!order.test(static_cast<int>(action)), "guided setup has no duplicate actions");
        order.set(static_cast<int>(action));
    }
    check(order.all(), "guided setup includes every N64 input");
    reset_bindings();
    const int original_a = binding(Action::A, BindingSlot::KeyboardPrimary);
    const int original_b = binding(Action::B, BindingSlot::KeyboardPrimary);
    const int alternate_a = binding(Action::A, BindingSlot::KeyboardSecondary);
    auto conflicts = binding_conflicts(Action::A, BindingSlot::KeyboardPrimary, original_b);
    check(conflicts.size() == 1 && conflicts[0].action == Action::B, "find keyboard conflict");
    check(swap_binding(Action::A, BindingSlot::KeyboardPrimary, original_b, conflicts[0]), "swap accepted");
    check(binding(Action::A, BindingSlot::KeyboardPrimary) == original_b &&
          binding(Action::B, BindingSlot::KeyboardPrimary) == original_a, "swap preserves both assignments");
    check(binding(Action::A, BindingSlot::KeyboardSecondary) == alternate_a, "alternate preserved");
    check(!swap_binding(Action::A, BindingSlot::KeyboardPrimary, original_b, conflicts[0]), "stale conflict rejected");
    set_binding(Action::L, BindingSlot::KeyboardSecondary, original_a);
    check(binding_conflicts(Action::A, BindingSlot::KeyboardPrimary, original_a).size() == 2, "multiple conflicts found");
    check(binding_conflicts(Action::A, BindingSlot::KeyboardPrimary, kUnbound).empty(), "unbound has no conflicts");
    check(binding_conflicts(Action::A, BindingSlot::KeyboardPrimary, alternate_a).empty(), "same action is not a conflict");
    check(!swap_binding(Action::A, BindingSlot::KeyboardPrimary, 0, {Action::B, BindingSlot::ControllerPrimary}), "device families cannot swap");
    reset_bindings();

    // Exercise the real SDL polling path: preview receives translated controls,
    // while the game's returned state remains neutral and shortcuts stay quiet.
    const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
    check(index >= 0 && SDL_IsGameController(index), "virtual gamepad attached");
    SDL_GameController* pad = SDL_GameControllerOpen(index);
    check(pad != nullptr, "virtual gamepad opened");
    SDL_Joystick* joystick = SDL_GameControllerGetJoystick(pad);
    SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_A, 1);
    SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, 30000);
    SDL_JoystickUpdate();
    State preview{};
    auto game = poll(pad, false, true, true, false, &preview);
    check((preview.buttons & 0x8000) != 0 && preview.stick_x > 0.8F, "test mode sees button and analogue input");
    check(game.buttons == 0 && game.stick_x == 0 && game.stick_y == 0, "test mode cannot move Rocket");
    game = poll(pad, false, true, false, true);
    check((game.buttons & 0x8000) != 0 && game.stick_x > 0.8F, "normal gameplay polling unchanged");
    SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_BACK, 1);
    SDL_JoystickUpdate();
    poll(pad, false, true, true, false, &preview);
    check(!consume_shortcut_request(ShortcutAction::ToggleOverlay), "test mode suppresses shortcuts");
    poll(pad, false, true, true, true);
    check(!consume_shortcut_request(ShortcutAction::ToggleOverlay), "held shortcut does not fire when test ends");
    SDL_GameControllerClose(pad);
    SDL_JoystickDetachVirtual(index);
    SDL_Quit();
    std::printf("Passed %d controls checks.\n", checks);
    return 0;
}
