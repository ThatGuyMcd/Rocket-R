#pragma once

#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <type_traits>

namespace rocket::ui::theme {
constexpr ImVec4 rgb(unsigned value) {
    return {((value >> 16) & 255) / 255.0F, ((value >> 8) & 255) / 255.0F,
            (value & 255) / 255.0F, 1.0F};
}
inline constexpr auto background = rgb(0x091626);
inline constexpr auto navigation = rgb(0x101f34);
inline constexpr auto panel = rgb(0x142a43);
inline constexpr auto card = rgb(0x1c3652);
inline constexpr auto field = rgb(0x0c1d31);
inline constexpr auto border = rgb(0x426b91);
inline constexpr auto text = rgb(0xeaf4fd);
inline constexpr auto muted = rgb(0xb0c7dc);
inline constexpr auto gold = rgb(0xffc46a);
inline constexpr auto cyan = rgb(0x90daf5);
inline constexpr auto green = rgb(0x83dcb5);
inline constexpr auto blue = rgb(0x17537d);
inline constexpr auto selected = rgb(0x267eab);
inline constexpr auto red = rgb(0x81394f);
inline constexpr float overlay_backdrop_opacity = .20F;
// Composite the panel over the backdrop to reach 75%, rather than stacking
// two 75% layers and leaving the game almost completely hidden.
inline constexpr float overlay_panel_opacity = (.75F - overlay_backdrop_opacity) /
    (1.0F - overlay_backdrop_opacity);
inline ImVec4 opacity(ImVec4 colour, float alpha) { colour.w = alpha; return colour; }
inline ImFont* page_font = nullptr;
inline ImFont* card_font = nullptr;

inline void apply() {
    auto& s = ImGui::GetStyle();
    // Modal windows use WindowRounding; ordinary menus use PopupRounding.
    s.WindowRounding = 18;
    s.PopupRounding = 16;
    s.ChildRounding = 18;
    s.FrameRounding = 12;
    s.ScrollbarRounding = 10;
    s.GrabRounding = 9;
    s.TabRounding = 8;
    s.WindowPadding = {22, 22};
    s.FramePadding = {12, 9};
    s.ItemSpacing = {12, 12};
    s.ItemInnerSpacing = {9, 7};
    s.CellPadding = {10, 10};
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 1;
    s.FrameBorderSize = 1;
    s.Colors[ImGuiCol_WindowBg] = panel;
    s.Colors[ImGuiCol_ChildBg] = panel;
    s.Colors[ImGuiCol_PopupBg] = panel;
    s.Colors[ImGuiCol_Border] = border;
    s.Colors[ImGuiCol_Text] = text;
    s.Colors[ImGuiCol_TextDisabled] = muted;
    s.Colors[ImGuiCol_FrameBg] = field;
    s.Colors[ImGuiCol_FrameBgHovered] = rgb(0x193c58);
    s.Colors[ImGuiCol_FrameBgActive] = rgb(0x245878);
    s.Colors[ImGuiCol_Button] = blue;
    s.Colors[ImGuiCol_ButtonHovered] = rgb(0x286c94);
    s.Colors[ImGuiCol_ButtonActive] = selected;
    s.Colors[ImGuiCol_Header] = rgb(0x214663);
    s.Colors[ImGuiCol_HeaderHovered] = rgb(0x2b6387);
    s.Colors[ImGuiCol_HeaderActive] = selected;
    s.Colors[ImGuiCol_Tab] = rgb(0x183b59);
    s.Colors[ImGuiCol_TabHovered] = rgb(0x2b6387);
    s.Colors[ImGuiCol_TabActive] = selected;
    s.Colors[ImGuiCol_CheckMark] = gold;
    s.Colors[ImGuiCol_SliderGrab] = gold;
    s.Colors[ImGuiCol_SliderGrabActive] = rgb(0xffdc99);
    s.Colors[ImGuiCol_Separator] = rgb(0x345575);
    s.Colors[ImGuiCol_NavHighlight] = gold;
    s.Colors[ImGuiCol_TitleBg] = navigation;
    s.Colors[ImGuiCol_TitleBgActive] = navigation;
    s.Colors[ImGuiCol_ScrollbarBg] = navigation;
    s.Colors[ImGuiCol_ScrollbarGrab] = border;
    s.Colors[ImGuiCol_ScrollbarGrabHovered] = selected;
    s.Colors[ImGuiCol_ScrollbarGrabActive] = gold;
    s.Colors[ImGuiCol_TableHeaderBg] = card;
    s.Colors[ImGuiCol_TableBorderStrong] = border;
    s.Colors[ImGuiCol_TableBorderLight] = border;
    s.Colors[ImGuiCol_DragDropTarget] = gold;
    s.Colors[ImGuiCol_TextSelectedBg] = {1, .77F, .42F, .3F};
    s.Colors[ImGuiCol_ModalWindowDimBg] = {0.02F, 0.05F, 0.09F, 0.72F};
}

inline void heading(const char* title, float scale = 1.0F) {
    ImFont* font = scale >= 1.5F ? page_font : scale >= 1.1F ? card_font : nullptr;
    if (font) ImGui::PushFont(font);
    else ImGui::SetWindowFontScale(scale);
    ImGui::TextColored(gold, "%s", title);
    if (font) ImGui::PopFont();
    else ImGui::SetWindowFontScale(1.0F);
}

// Native buttons keep ImGui's input, navigation, disabled and focus behaviour.
// The solid base is a single clipped shape, with no texture or animation.
inline bool button(const char* label, ImVec2 size = {}, bool active = false,
                   bool primary = false) {
    if (active || primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, primary ? gold : selected);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, primary ? rgb(0xffd18b) : rgb(0x318eba));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, primary ? rgb(0xe8ad54) : blue);
        ImGui::PushStyleColor(ImGuiCol_Text, primary ? background : text);
        ImGui::PushStyleColor(ImGuiCol_Border, primary ? rgb(0xf8dc95) : cyan);
    } else ImGui::PushStyleColor(ImGuiCol_Border, border);
    const auto pos = ImGui::GetCursorScreenPos();
    const auto measured = ImGui::CalcTextSize(label, nullptr, true);
    const auto padding = ImGui::GetStyle().FramePadding;
    // Negative widths retain the standard fill-available-width convention.
    const float w = size.x < 0 ? std::max(1.0F, ImGui::GetContentRegionAvail().x + size.x)
        : size.x > 0 ? size.x : measured.x + padding.x * 2;
    const float h = size.y > 0 ? size.y : measured.y + padding.y * 2;
    const float depth = 3;
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled({pos.x, pos.y + depth}, {pos.x + w, pos.y + h + depth},
        ImGui::GetColorU32(primary ? rgb(0x8c6538) : rgb(0x061020)), ImGui::GetStyle().FrameRounding);
    const bool pressed = ImGui::Button(label, size);
    if (active) {
        const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        draw->AddRectFilled({a.x + 4, a.y + 8}, {a.x + 7, b.y - 8},
            ImGui::GetColorU32(gold), 2);
    }
    ImGui::PopStyleColor(active || primary ? 5 : 1);
    return pressed;
}

// Native SliderFloat/SliderInt still own dragging, stepping, keyboard/gamepad
// navigation and direct entry. Only the track, thumb and readout are redrawn.
template<class Value, class Slider>
inline bool styled_slider(const char* id, Value* value, Value minimum, Value maximum,
                          const char* format, ImGuiSliderFlags flags, Slider slider) {
    const float width=ImGui::CalcItemWidth();
    std::string widget_id(id);
    if (!(id[0]=='#' && id[1]=='#')) {
        ImGui::TextUnformatted(id);
        widget_id="##"+widget_id;
    }
    char readout[96]{}; std::snprintf(readout,sizeof(readout),format,*value);
    const float readout_width=ImGui::CalcTextSize(readout).x;
    const auto previous_end=ImGui::GetItemRectMax();
    if (previous_end.x+12 > ImGui::GetCursorScreenPos().x+width-readout_width)
        ImGui::Dummy({0,ImGui::GetFontSize()});
    const ImVec2 origin=ImGui::GetCursorScreenPos();
    const auto widget=ImGui::GetID(widget_id.c_str());
    const bool text_entry=ImGui::TempInputIsActive(widget) || ImGui::GetIO().KeyCtrl ||
        (GImGui->NavActivateId==widget && (GImGui->NavActivateFlags & ImGuiActivateFlags_PreferInput));
    ImGui::SetNextItemWidth(width);
    if (!text_entry) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{0,3});
        ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize,18);
        for (auto color : {ImGuiCol_FrameBg,ImGuiCol_FrameBgHovered,ImGuiCol_FrameBgActive,
                           ImGuiCol_Border,ImGuiCol_SliderGrab,ImGuiCol_SliderGrabActive,ImGuiCol_Text})
            ImGui::PushStyleColor(color,{0,0,0,0});
    }
    const bool changed=slider(widget_id.c_str(),value,minimum,maximum,format,flags);
    if (!text_entry) { ImGui::PopStyleColor(7); ImGui::PopStyleVar(2); }
    if (!text_entry && !ImGui::TempInputIsActive(widget)) {
        auto* draw=ImGui::GetWindowDrawList();
        const float y=(ImGui::GetItemRectMin().y+ImGui::GetItemRectMax().y)*.5F;
        const float slider_width=std::max(width-4.F,1.F);
        const float range=float(maximum)-float(minimum);
        float grab=18;
        if constexpr (std::is_integral_v<Value>) grab=std::max(grab,slider_width/(range+1.F));
        grab=std::min(grab,slider_width);
        const float left=origin.x+2.F+grab*.5F;
        const float right=origin.x+width-2.F-grab*.5F;
        const float fraction=range>0 && std::isfinite(float(*value)) ? std::clamp((float(*value)-float(minimum))/range,0.F,1.F) : 0;
        const float x=left+(right-left)*fraction;
        const auto col=[](ImVec4 c) { return ImGui::GetColorU32(c); };
        draw->AddRectFilled({origin.x+2,y-3},{origin.x+width-2,y+3},col(field),3);
        draw->AddRect({origin.x+2,y-3},{origin.x+width-2,y+3},col(border),3);
        draw->AddRectFilled({origin.x+2,y-3},{x,y+3},col(gold),3);
        draw->AddCircleFilled({x,y},8,col(gold));
        draw->AddCircle({x,y},8,col(text),0,1);
        if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) draw->AddCircle({x,y},11,col(gold));
        char output[96]{}; std::snprintf(output,sizeof(output),format,*value);
        const auto extent=ImGui::CalcTextSize(output);
        const float top=origin.y-ImGui::GetStyle().ItemSpacing.y-ImGui::GetFontSize();
        draw->AddText({origin.x+width-extent.x,top},col(gold),output);
    }
    return changed;
}
inline bool slider_float(const char* id,float* v,float low,float high,const char* format="%.3f",ImGuiSliderFlags flags=0) {
    return styled_slider(id,v,low,high,format,flags,[](auto... args) { return ImGui::SliderFloat(args...); });
}
inline bool slider_int(const char* id,int* v,int low,int high,const char* format="%d",ImGuiSliderFlags flags=0) {
    return styled_slider(id,v,low,high,format,flags,[](auto... args) { return ImGui::SliderInt(args...); });
}

class Card {
public:
    explicit Card(const char* id, const char* title = nullptr, float width = 0,
                  ImVec4 edge = border) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, card);
        ImGui::PushStyleColor(ImGuiCol_Border, edge);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {20, 20});
        ImGui::BeginChild(id, {width, 0}, ImGuiChildFlags_Border |
            ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize | ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (title) { heading(title, 1.18F); ImGui::Spacing(); }
    }
    ~Card() {
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        ImGui::Spacing();
    }
    Card(const Card&) = delete;
    Card& operator=(const Card&) = delete;
};

class FieldGrid {
    bool table_ = false, field_ = false;
public:
    explicit FieldGrid(const char* id, bool paired = true) {
        const int columns = paired && ImGui::GetContentRegionAvail().x >= 700 ? 2 : 1;
        table_ = ImGui::BeginTable(id, columns, ImGuiTableFlags_SizingStretchSame |
            ImGuiTableFlags_NoPadOuterX);
    }
    void next() {
        if (field_) ImGui::PopItemWidth();
        if (table_) ImGui::TableNextColumn();
        // Give every label the same baseline. ImGui carries a preceding
        // combo/slider's frame padding into the next table cell.
        ImGui::AlignTextToFramePadding();
        ImGui::PushItemWidth(-FLT_MIN);
        field_ = true;
    }
    ~FieldGrid() {
        if (field_) ImGui::PopItemWidth();
        if (table_) ImGui::EndTable();
    }
    FieldGrid(const FieldGrid&) = delete;
    FieldGrid& operator=(const FieldGrid&) = delete;
};
} // namespace rocket::ui::theme
