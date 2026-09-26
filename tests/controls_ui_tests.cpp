#include "controls_studio.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace studio = rocket::ui::controls;
using namespace rocket::input;
struct Item { ImGuiID id; ImRect rect; ImGuiWindow* window; };
std::map<ImGuiID, Item> bounds;
std::map<std::string, Item> items;
Action captured = Action::Count;
BindingSlot captured_slot{};
int saves = 0;
int checks = 0;
float width = 960;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}
void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& rect, const ImGuiLastItemData*) {
    bounds[id] = {id, rect, ctx->CurrentWindow};
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags) {
    if (bounds.count(id)) items[label] = bounds[id];
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return "test"; }
void frame() {
    items.clear(); bounds.clear();
    auto& io = ImGui::GetIO(); io.DisplaySize = {width + 40, 2200}; io.DeltaTime = 1.0F / 60;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({width + 40, 2100});
    ImGui::Begin("controls-test", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    studio::draw(width, {[](Action a, BindingSlot s) { captured = a; captured_slot = s; },
                         [] { ++saves; }, [] { return State{0x8000, .8F, 0}; }}, "Virtual test pad", true);
    ImGui::End(); ImGui::Render();
}
void click(const char* label) {
    check(items.count(label) != 0, label);
    const auto p = items.at(label).rect.GetCenter();
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(p.x, p.y); frame();
    io.AddMouseButtonEvent(0, true); frame();
    io.AddMouseButtonEvent(0, false); frame(); frame();
}
void activate(const char* label, ImGuiKey key) {
    check(items.count(label) != 0, label);
    const auto item = items.at(label);
    ImGui::FocusWindow(item.window);
    ImGui::SetNavID(item.id, ImGuiNavLayer_Main, item.window->NavRootFocusScopeId,
        ImRect(item.rect.Min.x - item.window->Pos.x, item.rect.Min.y - item.window->Pos.y,
               item.rect.Max.x - item.window->Pos.x, item.rect.Max.y - item.window->Pos.y));
    GImGui->NavDisableHighlight = false;
    GImGui->NavDisableMouseHover = true;
    ImGui::GetIO().AddKeyEvent(key, true); frame();
    ImGui::GetIO().AddKeyEvent(key, false); frame(); frame();
}
int main(int argc, char** argv) {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    GImGui->TestEngineHookItems = true;
    // Exercise the inherited application font; a path permits visual-metric
    // checks with the actual installed font without redistributing it.
    if (argc > 1) io.FontDefault = io.Fonts->AddFontFromFileTTF(argv[1], 21.0F);
    else {
        ImFontConfig font{}; font.SizePixels = 21.0F;
        io.FontDefault = io.Fonts->AddFontDefault(&font);
    }
    unsigned char* pixels; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    frame(); frame();
    check(items.count("GUIDED SETUP"), "studio is the default");
    click("C BUTTONS"); click("Right"); click("L");
    check(captured == Action::CRight && captured_slot == BindingSlot::KeyboardPrimary, "group and direction select correct action");
    click("MAIN BUTTONS");
    captured = Action::Count; activate("Space", ImGuiKey_Enter);
    check(captured == Action::A, "keyboard activates focused binding");
    captured = Action::Count; activate("Bottom face button", ImGuiKey_GamepadFaceDown);
    check(captured == Action::A && captured_slot == BindingSlot::ControllerPrimary, "gamepad activates focused binding");
    for (const float test_width : {960.0F, 740.0F, 600.0F, 480.0F, 320.0F}) {
        width = test_width; frame(); frame();
        const auto card = items.at("Bottom face button").window;
        check(card->Pos.x >= 0 && card->Pos.x + card->Size.x <= width + 40, "binding card stays inside content at responsive widths");
    }
    width = 960; frame(); frame();
    click("GUIDED SETUP");
    for (int step = 0; step < 18; ++step) {
        captured = Action::Count;
        const auto action = studio::kSetupOrder[step];
        const auto name = keyboard_binding_name(binding(action, BindingSlot::KeyboardPrimary));
        click(name.c_str());
        check(captured == action, "guided step edits expected action");
        click(step == 17 ? "REVIEW SETUP" : "NEXT");
    }
    check(items.count("FINISH SETUP"), "all 18 steps reach review");
    click("TEST INPUTS"); check(studio::testing(), "review starts test mode");
    click("DONE TESTING"); check(!studio::testing(), "test exits without leaving guide");
    click("FINISH SETUP"); check(items.count("GUIDED SETUP"), "finish returns to studio");
    check(saves == 0, "navigation and tests never rewrite bindings");
    ImGui::DestroyContext();
    std::printf("Passed %d native UI checks.\n", checks);
}
