#pragma once

#include "runtime_input.hpp"
#include <array>
#include <string>

namespace rocket::ui::controls {
using input::Action;
using input::BindingSlot;
inline constexpr std::array<Action, 18> kSetupOrder{
    Action::StickUp, Action::StickDown, Action::StickLeft, Action::StickRight,
    Action::A, Action::B, Action::Z, Action::Start,
    Action::CUp, Action::CDown, Action::CLeft, Action::CRight,
    Action::DpadUp, Action::DpadDown, Action::DpadLeft, Action::DpadRight, Action::L, Action::R};

struct Callbacks {
    void (*capture)(Action, BindingSlot);
    void (*save)();
    input::State (*preview)();
};

void draw(float width, const Callbacks& callbacks, const std::string& controller_name, bool connected);
bool testing();
void end_test();
} // namespace rocket::ui::controls
