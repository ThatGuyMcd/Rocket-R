#include "controls_studio.hpp"

#include "imgui.h"
#include <algorithm>
#include <atomic>
#include <bitset>
#include <cstdio>

namespace rocket::ui::controls {
namespace {
constexpr ImVec4 kOrange{1.0F, 0.55F, 0.22F, 1.0F};
constexpr ImVec4 kSelected{0.24F, 0.15F, 0.105F, 1.0F};
constexpr ImVec4 kSurface{0.055F, 0.12F, 0.17F, 1.0F};
constexpr ImVec4 kQuiet{0.10F, 0.20F, 0.26F, 1.0F};
constexpr ImVec4 kLine{0.20F, 0.33F, 0.39F, 1.0F};
constexpr ImVec4 kPressed{0.12F, 0.48F, 0.36F, 1.0F};
constexpr std::array<const char*, 4> kGroupNames{"STICK", "MAIN BUTTONS", "C BUTTONS", "D-PAD / L / R"};
constexpr std::array<int, 5> kGroupStarts{0, 4, 8, 12, 18};
Action g_selected = Action::A;
bool g_guided = false;
bool g_review = false;
bool g_alternates = false;
int g_step = 0;
std::bitset<18> g_reviewed;
std::atomic<bool> g_testing{false};
std::atomic<bool> g_reset_test_input{false};

void StartTest() {
    ImGui::GetIO().ClearInputKeys();
    g_testing.store(true);
}

int OrderIndex(Action action) {
    const auto it = std::find(kSetupOrder.begin(), kSetupOrder.end(), action);
    return it == kSetupOrder.end() ? 0 : static_cast<int>(it - kSetupOrder.begin());
}
int Group(Action action) {
    const int index = OrderIndex(action);
    return index < 4 ? 0 : index < 8 ? 1 : index < 12 ? 2 : 3;
}
const char* ShortName(Action action) {
    switch (action) {
        case Action::StickUp: case Action::DpadUp: case Action::CUp: return "Up";
        case Action::StickDown: case Action::DpadDown: case Action::CDown: return "Down";
        case Action::StickLeft: case Action::DpadLeft: case Action::CLeft: return "Left";
        case Action::StickRight: case Action::DpadRight: case Action::CRight: return "Right";
        case Action::A: return "A";
        case Action::B: return "B";
        case Action::Z: return "Z";
        case Action::Start: return "Start";
        case Action::L: return "L";
        case Action::R: return "R";
        default: return "";
    }
}
bool IsPressed(Action action, const input::State& state) {
    switch (action) {
        case Action::StickUp: return state.stick_y > 0.15F;
        case Action::StickDown: return state.stick_y < -0.15F;
        case Action::StickLeft: return state.stick_x < -0.15F;
        case Action::StickRight: return state.stick_x > 0.15F;
        default: break;
    }
    constexpr std::array<std::uint16_t, 18> masks{
        0, 0, 0, 0, 0x8000, 0x4000, 0x2000, 0x1000,
        0x0800, 0x0400, 0x0200, 0x0100, 0x0020, 0x0010, 0x0008, 0x0004, 0x0002, 0x0001};
    return (state.buttons & masks[static_cast<std::size_t>(action)]) != 0;
}

bool Choice(const char* text, bool selected, ImVec2 size, bool pressed = false) {
    ImGui::PushStyleColor(ImGuiCol_Button, pressed ? kPressed : selected ? kSelected : kSurface);
    ImGui::PushStyleColor(ImGuiCol_Border, selected ? kOrange : kLine);
    ImGui::PushStyleColor(ImGuiCol_Text, selected ? kOrange : ImGui::GetStyleColorVec4(ImGuiCol_Text));
    const bool clicked = ImGui::Button(text, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

void Diagram(float width, const input::State& preview, bool test) {
    const auto origin = ImGui::GetCursorScreenPos();
    const float w = std::min(width, 480.0F);
    const float x = origin.x + (width - w) * 0.5F;
    const float y = origin.y;
    const auto p = [&](float px, float py) { return ImVec2{x + px * w, y + py}; };
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 body = IM_COL32(47, 73, 85, 255);
    const ImU32 rim = IM_COL32(62, 96, 109, 255);
    // Three overlapping convex grips and a rounded body avoid concave-fill
    // artifacts in the project's pinned ImGui version.
    const ImVec2 left[]{p(.10F, 106), p(.35F, 139), p(.23F, 274), p(.08F, 288), p(.02F, 259)};
    const ImVec2 middle[]{p(.40F, 135), p(.60F, 135), p(.57F, 306), p(.43F, 306)};
    const ImVec2 right[]{p(.65F, 139), p(.90F, 106), p(.98F, 259), p(.92F, 288), p(.77F, 274)};
    draw->AddConvexPolyFilled(left, 5, body);
    draw->AddConvexPolyFilled(middle, 4, body);
    draw->AddConvexPolyFilled(right, 5, body);
    draw->AddRectFilled(p(.10F, 43), p(.90F, 179), body, 35.0F);
    draw->AddLine(p(.23F, 54), p(.77F, 54), rim, 2.0F);
    const ImVec2 brand = ImGui::CalcTextSize("N64");
    draw->AddText({x + w * .5F - brand.x * .5F, y + 62}, IM_COL32(176, 194, 202, 255), "N64");
    const auto control = [&](const char* label, Action action, float px, float py,
                             float bw, bool group = false, ImVec4 color = kSurface) {
        bw = std::max(bw, ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0F);
        bool selected = g_selected == action;
        bool down = IsPressed(action, preview);
        if (group) {
            const int group_index = Group(action);
            selected = Group(g_selected) == group_index;
            for (int i = kGroupStarts[group_index]; i < kGroupStarts[group_index + 1]; ++i) {
                // L/R are individually represented rather than part of D-pad.
                if (group_index == 3 && i >= 16) continue;
                down |= IsPressed(kSetupOrder[i], preview);
            }
            if (group_index == 3 && (g_selected == Action::L || g_selected == Action::R)) selected = false;
        }
        ImGui::SetCursorScreenPos({x + w * px - bw * .5F, y + py - 24.0F});
        ImGui::PushID(static_cast<int>(action));
        ImGui::PushStyleColor(ImGuiCol_Button, test && down ? kPressed : selected ? kSelected : color);
        ImGui::PushStyleColor(ImGuiCol_Border, selected ? kOrange : kLine);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, bw <= 54.0F ? 24.0F : 10.0F);
        if (ImGui::Button(label, {bw, 48.0F}) && !test) g_selected = action;
        if (test && down) {
            draw->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(77, 239, 155, 255), 10.0F, 0, 3.0F);
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        ImGui::PopID();
    };
    ImGui::BeginGroup();
    control("L", Action::L, .20F, 33, 64);
    control("R", Action::R, .80F, 33, 64);
    control("D-pad", Action::DpadUp, .18F, 137, 78, true);
    control("Start", Action::Start, .50F, 117, 62);
    control("C", Action::CUp, .81F, 111, 60, true, {0.31F, 0.25F, 0.10F, 1.0F});
    control("B", Action::B, .65F, 176, 48, false, {0.12F, 0.30F, 0.19F, 1.0F});
    control("A", Action::A, .83F, 218, 48, false, {0.13F, 0.24F, 0.43F, 1.0F});
    control("Stick", Action::StickUp, .50F, 218, 70, true);
    control("Z", Action::Z, .20F, 290, 64);
    ImGui::SetCursorScreenPos({origin.x, origin.y + 328});
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    ImGui::TextWrapped("Front view. Z is underneath the controller.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy({width, 0});
    ImGui::EndGroup();
}

void GroupPicker(float width, bool guided) {
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    float minimum = 0.0F;
    for (const char* name : kGroupNames)
        minimum = std::max(minimum, ImGui::CalcTextSize(name).x + ImGui::GetStyle().FramePadding.x * 2.0F);
    const int columns = width >= minimum * 4 + gap * 3 ? 4 : width >= minimum * 2 + gap ? 2 : 1;
    const float bw = (width - gap * (columns - 1)) / columns;
    for (int group = 0; group < 4; ++group) {
        if (group % columns != 0) ImGui::SameLine();
        if (Choice(kGroupNames[group], Group(g_selected) == group, {bw, 44})) {
            g_selected = kSetupOrder[kGroupStarts[group]];
            if (guided) { g_step = kGroupStarts[group]; g_review = false; }
        }
    }
}

std::string BindingName(Action action, BindingSlot slot) {
    const int source = input::binding(action, slot);
    if (source == input::kUnbound) return "Add input...";
    return slot == BindingSlot::KeyboardPrimary || slot == BindingSlot::KeyboardSecondary
        ? input::keyboard_binding_name(source) : input::controller_binding_name(source);
}

void Assignment(const char* label, Action action, BindingSlot slot, const Callbacks& callbacks) {
    ImGui::PushID(static_cast<int>(slot));
    ImGui::TextWrapped("%s", label);
    const auto name = BindingName(action, slot);
    if (ImGui::Button(name.c_str(), {-1.0F, 46.0F})) callbacks.capture(action, slot);
    ImGui::PopID();
}

void Editor(const Callbacks& callbacks, bool guided) {
    ImGui::PushID(static_cast<int>(g_selected));
    ImGui::TextColored(kOrange, "N64 CONTROL");
    ImGui::TextUnformatted(input::action_label(g_selected));
    ImGui::Spacing();
    if (!guided) {
        const int group = Group(g_selected);
        const float width = ImGui::GetContentRegionAvail().x;
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const int columns = width >= 420.0F ? 4 : 2;
        const float bw = (width - (columns - 1) * gap) / columns;
        for (int i = kGroupStarts[group]; i < kGroupStarts[group + 1]; ++i) {
            if ((i - kGroupStarts[group]) % columns != 0) ImGui::SameLine();
            ImGui::PushID(i);
            if (Choice(ShortName(kSetupOrder[i]), g_selected == kSetupOrder[i], {bw, 44})) g_selected = kSetupOrder[i];
            ImGui::PopID();
        }
        ImGui::Spacing();
    }
    Assignment("KEYBOARD", g_selected, BindingSlot::KeyboardPrimary, callbacks);
    Assignment("CONTROLLER", g_selected, BindingSlot::ControllerPrimary, callbacks);
    const int alternate_count = (input::binding(g_selected, BindingSlot::KeyboardSecondary) != input::kUnbound) +
                                (input::binding(g_selected, BindingSlot::ControllerSecondary) != input::kUnbound);
    char label[64];
    std::snprintf(label, sizeof(label), "Extra inputs (%d)", alternate_count);
    ImGui::Checkbox(label, &g_alternates);
    if (g_alternates) {
        Assignment("SECOND KEYBOARD INPUT", g_selected, BindingSlot::KeyboardSecondary, callbacks);
        Assignment("SECOND CONTROLLER INPUT", g_selected, BindingSlot::ControllerSecondary, callbacks);
    }
    ImGui::TextWrapped("Select an input to change or remove it.");
    ImGui::PopID();
}

void TestInputs(float width, const Callbacks& callbacks) {
    ImGui::TextColored(kOrange, "TEST YOUR INPUTS");
    ImGui::TextWrapped("Press a key or controller button, or move a stick. The matching N64 control lights up without sending input to the game.");
    if (ImGui::Button("DONE TESTING", {std::min(width, 250.0F), 46.0F})) end_test();
    ImGui::TextWrapped("Press Escape or your controller's Back / Select button to finish.");
    const auto preview = callbacks.preview();
    Diagram(width, preview, true);
    ImGui::Text("Stick X: %+.2f   Y: %+.2f", preview.stick_x, preview.stick_y);
    const float bw = width >= 650.0F ? (width - 24.0F) / 3.0F : (width - 12.0F) / 2.0F;
    const int columns = width >= 650.0F ? 3 : 2;
    for (std::size_t i = 0; i < kSetupOrder.size(); ++i) {
        if (i % columns != 0) ImGui::SameLine();
        const bool down = IsPressed(kSetupOrder[i], preview);
        ImGui::PushID(static_cast<int>(i));
        Choice(input::action_label(kSetupOrder[i]), false, {bw, 44.0F}, down);
        ImGui::PopID();
    }
}

void Review(float width, const Callbacks& callbacks) {
    ImGui::PushID("guided-review");
    ImGui::TextColored(kOrange, "REVIEW YOUR SETUP");
    ImGui::TextWrapped("%d of 18 controls reviewed. Anything you skipped keeps its previous input.", static_cast<int>(g_reviewed.count()));
    if (ImGui::Button("TEST INPUTS", {std::min(width, 240.0F), 46.0F})) StartTest();
    if (ImGui::Button("FINISH SETUP", {std::min(width, 240.0F), 46.0F})) { g_guided = false; g_review = false; }
    ImGui::TextWrapped("Your changes are saved. Select a control to edit it again.");
    for (std::size_t i = 0; i < kSetupOrder.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const auto action = kSetupOrder[i];
        if (ImGui::Button(input::action_label(action), {-1, 44})) {
            g_step = static_cast<int>(i); g_selected = action; g_review = false;
        }
        ImGui::TextWrapped("Keyboard: %s   |   Controller: %s",
            BindingName(action, BindingSlot::KeyboardPrimary).c_str(),
            BindingName(action, BindingSlot::ControllerPrimary).c_str());
        ImGui::PopID();
    }
    ImGui::PopID();
}
} // namespace

bool testing() { return g_testing.load(); }
void end_test() { if (g_testing.exchange(false)) g_reset_test_input.store(true); }

void draw(float width, const Callbacks& callbacks, const std::string& controller_name, bool connected) {
    if (g_reset_test_input.exchange(false)) ImGui::GetIO().ClearInputKeys();
    width = std::min(width, ImGui::GetContentRegionAvail().x);
    ImGui::PushID("controller-studio");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {12, 8});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10, 9});
    ImGui::PushStyleColor(ImGuiCol_Button, kQuiet);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.24F, 0.32F, 0.36F, 1.0F});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kSelected);
    ImGui::TextColored(kOrange, g_guided ? "GUIDED SETUP" : "CONTROLLER STUDIO");
    ImGui::TextWrapped(g_guided ? "Set up each control in turn. Changes are saved automatically." : "Select an N64 control to choose its keyboard or controller input.");
    ImGui::Spacing();
    if (testing()) {
        TestInputs(width, callbacks);
    } else {
        const float action_width = width >= 550.0F ? 235.0F : width;
        if (ImGui::Button(g_guided ? "BACK TO STUDIO" : "GUIDED SETUP", {action_width, 46})) {
            g_guided = !g_guided;
            if (g_guided) { g_step = 0; g_selected = kSetupOrder[0]; g_review = false; g_reviewed.reset(); }
        }
        if (width >= 550.0F) ImGui::SameLine();
        if (ImGui::Button("TEST INPUTS", {action_width, 46})) StartTest();
        ImGui::TextWrapped("Controller: %s", connected ? controller_name.c_str() : "Not connected. Keyboard controls are available.");
        ImGui::Separator();
        if (g_guided && g_review) {
            Review(width, callbacks);
        } else {
            GroupPicker(width, g_guided);
            if (g_guided) {
                char progress[64];
                std::snprintf(progress, sizeof(progress), "Control %d of 18", g_step + 1);
                ImGui::ProgressBar(static_cast<float>(g_step) / 18.0F, {-1, 24}, progress);
                g_selected = kSetupOrder[g_step];
            }
            ImGui::Spacing();
            const bool wide = width >= 740.0F;
            if (wide) {
                const float diagram_width = (width - 18.0F) * .47F;
                const float editor_width = width - diagram_width - 18.0F;
                ImGui::BeginGroup();
                Diagram(diagram_width, {}, false);
                ImGui::EndGroup();
                ImGui::SameLine(0, 18);
                ImGui::PushStyleColor(ImGuiCol_ChildBg, kSurface);
                ImGui::BeginChild("binding-card", {editor_width, 0}, ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY,
                                  ImGuiWindowFlags_NoScrollbar);
                Editor(callbacks, g_guided);
                ImGui::EndChild();
                ImGui::PopStyleColor();
            } else {
                Diagram(width, {}, false);
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_ChildBg, kSurface);
                ImGui::BeginChild("binding-card", {width, 0}, ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY,
                                  ImGuiWindowFlags_NoScrollbar);
                Editor(callbacks, g_guided);
                ImGui::EndChild();
                ImGui::PopStyleColor();
            }
            if (g_guided) {
                // Diagram selections are also valid shortcuts within the guide.
                g_step = OrderIndex(g_selected);
                ImGui::Spacing();
                ImGui::BeginDisabled(g_step == 0);
                if (ImGui::Button("BACK", {std::min(width, 155.0F), 46})) { --g_step; g_selected = kSetupOrder[g_step]; }
                ImGui::EndDisabled();
                if (width >= 430.0F) ImGui::SameLine();
                if (ImGui::Button(g_step == 17 ? "REVIEW SETUP" : "NEXT", {std::min(width, 225.0F), 46})) {
                    g_reviewed.set(g_step);
                    if (g_step == 17) g_review = true;
                    else { ++g_step; g_selected = kSetupOrder[g_step]; }
                }
                ImGui::TextWrapped("Your current input will be kept.");
            }
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextWrapped("Button positions: bottom, right, left and top.");
        if (ImGui::Button("RESET N64 CONTROLS", {std::min(width, 380.0F), 44})) ImGui::OpenPopup("Reset N64 controls?");
        ImGui::SetNextWindowSizeConstraints({std::min(width, 440.0F), 0}, {std::min(width, 440.0F), FLT_MAX});
        if (ImGui::BeginPopupModal("Reset N64 controls?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("Reset all keyboard and controller inputs to their defaults?");
            if (ImGui::Button("RESET", {-1, 44})) {
                input::reset_bindings(); callbacks.save(); ImGui::CloseCurrentPopup();
            }
            if (ImGui::Button("CANCEL", {-1, 44})) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    ImGui::PopID();
}
} // namespace rocket::ui::controls
