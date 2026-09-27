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
SDL_Event mouse_button(Uint32 type, Uint8 button) {
    SDL_Event e{}; e.type=type; e.button.button=button; return e;
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
    SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_BACK, 0);
    SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTX, 32767);
    SDL_JoystickUpdate();
    check((poll(pad,false,true,false,true).buttons & 0x0001)!=0,"right stick still drives C-right without a camera mod");
    set_camera_input_owned(true);
    check((poll(pad,false,true,false,true).buttons & 0x0001)==0,"camera input does not also generate C-right");
    SDL_JoystickSetVirtualButton(joystick,SDL_CONTROLLER_BUTTON_Y,1);
    SDL_JoystickUpdate();
    check((poll(pad,false,true,false,true).buttons & 0x0008)!=0,"physical C-up binding remains available with camera ownership");
    poll(pad,false,true,true,false,&preview);
    check((preview.buttons & 0x0001)!=0,"Controller Studio preview still sees the full binding");
    auto camera = poll_camera(pad, false, true, false);
    check(camera.x > 0.99F && !camera.recenter, "default camera binding preserves analogue right-stick output");
    set_camera_binding(CameraAction::Recenter, false, SDL_CONTROLLER_BUTTON_A);
    camera = poll_camera(pad, false, true, false);
    check(camera.recenter, "recenter can be remapped to a face button");
    check((poll(pad,false,true,false,true).buttons & 0x8000)==0,
        "remapped recenter does not also jump while the camera owns input");
    set_camera_binding(CameraAction::Right, false, encode_controller_axis(SDL_CONTROLLER_AXIS_LEFTX, true));
    camera = poll_camera(pad, false, true, false);
    check(camera.x > 0.9F && camera.x < 0.95F, "remapped axis keeps analogue magnitude without movement-stick shaping");
    check(poll(pad,false,true,false,true).stick_x == 0,
        "an axis assigned to the mod camera no longer also moves Rocket");
    check((poll(pad,false,true,false,true).buttons & 0x0001)!=0,
        "unassigned right-stick direction returns to its N64 mapping");
    set_camera_binding(CameraAction::Left, false, SDL_CONTROLLER_BUTTON_Y);
    check(poll_camera(pad,false,true,false).x < 0,
        "buttons work as full camera directions and opposite inputs combine");
    set_camera_binding(CameraAction::Recenter, false, SDL_CONTROLLER_BUTTON_BACK);
    SDL_JoystickSetVirtualButton(joystick,SDL_CONTROLLER_BUTTON_BACK,1);
    SDL_JoystickUpdate();
    check(!poll_camera(pad,false,true,false).recenter, "settings shortcut always takes priority over camera bindings");
    check(poll_camera(pad,false,false,false).x == 0 &&
          poll_camera(pad,false,true,true).x == 0 && !poll_camera(pad,false,true,true).recenter,
        "camera respects focus, disabled devices and overlay capture");
    set_camera_binding(CameraAction::Right, false, kUnbound);
    check(camera_binding(CameraAction::Right,false)==kUnbound, "camera assignment can be cleared");
    set_camera_binding(CameraAction::Right, false, 90000);
    check(camera_binding(CameraAction::Right,false)==kUnbound, "invalid saved source is ignored");
    set_camera_binding(CameraAction::Recenter, true, SDL_SCANCODE_P);
    check(camera_binding(CameraAction::Recenter,true)==SDL_SCANCODE_P, "keyboard camera assignments are independent");
    set_camera_input_owned(false);
    check((poll(pad,false,true,false,true).buttons & 0x8000)!=0 && poll(pad,false,true,false,true).stick_x > 0.8F,
        "original gameplay inputs return when the mod releases the camera");
    reset_camera_bindings();
    set_camera_mouse_sensitivity(99);
    check(camera_mouse_sensitivity()==1.0F,"mouse sensitivity has a safe upper bound");
    set_camera_mouse_sensitivity(0);
    check(camera_mouse_sensitivity()==0.01F,"mouse sensitivity has a nonzero lower bound");
    set_camera_mouse_sensitivity(0.15F);
    set_camera_mouse_recenter_button(SDL_BUTTON_X1);
    check(camera_mouse_recenter_button()==SDL_BUTTON_X1,"mouse recenter button is remappable");
    set_camera_mouse_recenter_button(0);
    check(camera_mouse_recenter_button()==0,"mouse recenter can be disabled");
    set_camera_mouse_recenter_button(SDL_BUTTON_MIDDLE);
    check(camera_binding(CameraAction::Recenter,false)==SDL_CONTROLLER_BUTTON_RIGHTSTICK &&
        camera_binding(CameraAction::Recenter,true)==SDL_SCANCODE_U,
        "camera reset restores controller and keyboard defaults");
    SDL_JoystickSetVirtualButton(joystick,SDL_CONTROLLER_BUTTON_A,0);
    SDL_JoystickSetVirtualButton(joystick,SDL_CONTROLLER_BUTTON_Y,0);
    SDL_JoystickSetVirtualButton(joystick,SDL_CONTROLLER_BUTTON_BACK,0);
    SDL_JoystickSetVirtualAxis(joystick,SDL_CONTROLLER_AXIS_LEFTX,0);
    SDL_JoystickSetVirtualAxis(joystick,SDL_CONTROLLER_AXIS_RIGHTX,0);
    set_camera_actions_active(true);
    set_camera_input_owned(true);
    SDL_JoystickSetVirtualButton(joystick,SDL_CONTROLLER_BUTTON_LEFTSTICK,1);
    SDL_JoystickUpdate();
    check((poll(pad,false,true,false).buttons & 0x000C)==0x0004,
        "cycle zoom produces the native C-down input");
    check((poll(pad,false,true,false).buttons & 0x0004)!=0,
        "holding zoom remains held so native edge detection cannot repeatedly cycle");
    SDL_JoystickSetVirtualButton(joystick,SDL_CONTROLLER_BUTTON_LEFTSTICK,0);
    SDL_JoystickUpdate();
    check((poll(pad,false,true,false).buttons & 0x0004)==0,"zoom release reaches the game");
    SDL_JoystickSetVirtualButton(joystick,SDL_CONTROLLER_BUTTON_RIGHTSTICK,1);
    SDL_JoystickUpdate();
    check(poll_camera(pad,false,true,false).recenter && (poll(pad,false,true,false).buttons & 0x0004)==0,
        "recenter and zoom stay separate despite the original C-down assignment");
    SDL_JoystickSetVirtualButton(joystick,SDL_CONTROLLER_BUTTON_RIGHTSTICK,0);
    set_camera_binding(CameraAction::FirstPerson,false,SDL_CONTROLLER_BUTTON_A);
    SDL_JoystickSetVirtualButton(joystick,SDL_CONTROLLER_BUTTON_A,1);
    SDL_JoystickUpdate();
    check((poll(pad,false,true,false).buttons & 0x8008)==0x0008,
        "first person emits C-up without also jumping");
    set_camera_input_owned(false);
    check((poll(pad,false,true,false).buttons & 0x8008)==0x0008,
        "first-person exit remains bound when the orbit path releases input");
    check(poll(pad,false,true,true).buttons==0 && poll(pad,false,false,false).buttons==0,
        "mode actions respect overlay blocking and disabled devices");
    poll(pad,false,true,true,false,&preview);
    check((preview.buttons & 0x8000)!=0 && (preview.buttons & 0x0008)==0,
        "Controller Studio previews N64 inputs rather than camera mode actions");
    set_camera_binding(CameraAction::FirstPerson,false,SDL_CONTROLLER_BUTTON_BACK);
    SDL_JoystickSetVirtualButton(joystick,SDL_CONTROLLER_BUTTON_BACK,1);
    SDL_JoystickUpdate();
    check(!poll_camera(pad,false,true,false).first_person,
        "reserved overlay shortcut cannot become a first-person action");
    set_camera_actions_active(false);
    check((poll(pad,false,true,false).buttons & 0x8000)!=0,
        "turning off the mod restores the original action source");
    reset_camera_bindings();
    capture.finish();
    auto click=mouse_button(SDL_MOUSEBUTTONDOWN,SDL_BUTTON_LEFT);
    capture.event(click,17,now);
    capture.begin(Device::Keyboard,17,now);
    capture.event(click,17,settled);
    check(capture.phase==Phase::Waiting,"held activation click cannot become a binding");
    capture.event(mouse_button(SDL_MOUSEBUTTONUP,SDL_BUTTON_LEFT),17,settled);
    capture.event(click,17,settled,true);
    check(capture.phase==Phase::Waiting,"clicking a popup control cannot bind the left button");
    capture.event(mouse_button(SDL_MOUSEBUTTONUP,SDL_BUTTON_LEFT),17,settled);
    capture.event(click,17,settled);
    check(capture.phase==Phase::Ready && capture.source==encode_mouse_button(SDL_BUTTON_LEFT),"fresh mouse click binds in a desktop slot");
    capture.finish();capture.begin(Device::Keyboard,17,now);
    SDL_Event wheel{};wheel.type=SDL_MOUSEWHEEL;wheel.wheel.y=1;wheel.wheel.direction=SDL_MOUSEWHEEL_FLIPPED;
    capture.event(wheel,17,settled);
    check(capture.source==encode_mouse_wheel(MouseDirection::Down),"wheel capture respects SDL's scroll direction");
    capture.finish();capture.begin(Device::Keyboard,17,now);
    SDL_Event motion{};motion.type=SDL_MOUSEMOTION;motion.motion.xrel=80;
    capture.event(motion,17,settled);
    check(capture.phase==Phase::Waiting,"moving towards the binding prompt cannot capture accidentally");
    capture.capture_mouse_motion(true,now);capture.event(motion,17,settled);
    check(capture.source==encode_mouse_motion(MouseDirection::Right),"explicit movement capture records a direction");
    capture.finish();capture.begin(Device::Controller,17,now);capture.event(wheel,17,settled);
    check(capture.phase==Phase::Waiting,"mouse cannot fill a controller slot");
    capture.finish();

    reset_bindings();reset_shortcuts();set_camera_input_owned(false);set_camera_actions_active(false);set_camera_runtime_enabled(true);
    clear_mouse_transients();
    set_binding(Action::A,BindingSlot::KeyboardPrimary,encode_mouse_button(SDL_BUTTON_LEFT));
    mouse_event(click);mouse_event(mouse_button(SDL_MOUSEBUTTONUP,SDL_BUTTON_LEFT));
    check((poll(nullptr,true,false,false).buttons&0x8000)!=0,"quick mouse click reaches the N64 button even between host polls");
    check(poll(nullptr,false,false,false).buttons==0 && poll(nullptr,true,false,true).buttons==0,"mouse respects device and overlay blocking");
    check(binding_conflicts(Action::B,BindingSlot::KeyboardPrimary,encode_mouse_button(SDL_BUTTON_LEFT)).size()==1,"mouse participates in binding conflicts");
    clear_mouse_transients();
    set_binding(Action::Z,BindingSlot::KeyboardPrimary,encode_mouse_wheel(MouseDirection::Down));mouse_event(wheel);
    check((poll(nullptr,true,false,false).buttons&0x2000)!=0,"wheel input reaches N64 bindings");
    clear_mouse_transients();
    set_binding(Action::StickRight,BindingSlot::KeyboardPrimary,encode_mouse_motion(MouseDirection::Right));
    set_binding(Action::StickLeft,BindingSlot::KeyboardPrimary,encode_mouse_motion(MouseDirection::Left));
    mouse_event(motion);
    check(mouse_motion_bound() && poll(nullptr,true,false,false).stick_x>0,"mouse movement drives an assigned N64 stick direction");
    motion.motion.xrel=-80;mouse_event(motion);
    check(poll(nullptr,true,false,false).stick_x<0,"mouse movement can reverse without fighting accumulated input");
    clear_mouse_transients();
    set_camera_binding(CameraAction::CycleZoom,true,encode_mouse_button(SDL_BUTTON_LEFT));
    set_camera_actions_active(true);set_camera_input_owned(true);mouse_event(click);
    check((poll(nullptr,true,false,false).buttons&0x8004)==0x0004,"mouse camera mode binding overrides its matching N64 button");
    set_camera_runtime_enabled(false);
    check(!camera_input_owned() && (poll(nullptr,true,false,false).buttons&0x8004)==0x8000,"runtime disable immediately releases mouse/N64 ownership");
    set_camera_runtime_enabled(true);set_camera_actions_active(false);set_camera_input_owned(false);
    mouse_event(mouse_button(SDL_MOUSEBUTTONUP,SDL_BUTTON_LEFT));clear_mouse_transients();
    set_camera_binding(CameraAction::FirstPerson,true,encode_mouse_button(SDL_BUTTON_MIDDLE));
    check(camera_mouse_recenter_button()==0,"remapping middle click to another camera action removes its legacy recenter alias");
    set_shortcut_keyboard_binding(ShortcutAction::ToggleOverlay,encode_mouse_button(SDL_BUTTON_X1));
    mouse_event(mouse_button(SDL_MOUSEBUTTONDOWN,SDL_BUTTON_X1));poll(nullptr,true,false,false);
    check(consume_shortcut_request(ShortcutAction::ToggleOverlay),"mouse button can open the overlay through a shortcut binding");
    mouse_event(focus);
    check(poll(nullptr,true,false,false).buttons==0,"focus loss clears held mouse input");
    check(keyboard_binding_name(encode_mouse_button(SDL_BUTTON_X2))=="Mouse side button 2" &&
          keyboard_binding_name(encode_mouse_motion(MouseDirection::Up))=="Mouse move up","mouse bindings have readable names for persisted values");
    SDL_GameControllerClose(pad);
    SDL_JoystickDetachVirtual(index);
    SDL_Quit();
    std::printf("Passed %d controls checks.\n", checks);
    return 0;
}
