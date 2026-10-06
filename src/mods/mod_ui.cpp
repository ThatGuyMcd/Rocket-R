#include "mod_ui.hpp"
#include "mod_library.hpp"
#include "mod_runtime.hpp"
#include "sdk_runtime.hpp"
#include "sdk_services.hpp"
#include "runtime_input.hpp"
#include "mod_camera.generated.hpp"
#include "runtime_ui.hpp"
#include "ui_theme.hpp"
#include "imgui/imgui.h"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <chrono>
#include <set>

namespace rocket::mods::ui {
namespace {
std::string status;
bool error=false;
std::array<char,1024> path_text{};
std::array<char,128> search{}, profile_name{};
std::filesystem::path folder;
struct FileChoice { std::filesystem::path path; std::string label; bool directory; };
std::vector<FileChoice> folder_entries;
std::filesystem::path scanned_folder;
std::string folder_error;
std::chrono::steady_clock::time_point folder_scan_due{};
void refresh_folder() {
    const auto now=std::chrono::steady_clock::now();
    if(folder==scanned_folder&&now<folder_scan_due)return;
    scanned_folder=folder;folder_scan_due=now+std::chrono::seconds(2);
    folder_entries.clear();folder_error.clear();
    std::error_code ec;
    std::filesystem::directory_iterator it(folder,ec),end;
    for(;!ec&&it!=end;it.increment(ec)) {
        std::error_code type_error;
        const bool directory=it->is_directory(type_error);
        if(type_error)continue;
        const auto path=it->path();
        const auto ext=path.extension().string();
        if(!directory&&ext!=".nrm"&&ext!=".rtz"&&ext!=".zip"&&ext!=".json")continue;
        const auto name=path.filename().u8string();
        folder_entries.push_back({path,(directory?"[Folder] ":"")+std::string(name.begin(),name.end()),directory});
    }
    if(ec)folder_error=ec.message();
    std::sort(folder_entries.begin(),folder_entries.end(),[](const auto& a,const auto& b){
        if(a.directory!=b.directory)return a.directory;
        return a.path.filename()<b.path.filename();
    });
}
const Package& included_camera() {
    static const Package package = inspect_package(rocket::generated::kCameraMod, ".nrm");
    return package;
}
template<class F> bool attempt(F&& action,const char* success="") {
    try {action();status=success;error=false;return true;}
    catch(const std::exception& e){status=e.what();error=true;return false;}
}
std::string utf8(const std::filesystem::path& p) {
    auto s=p.u8string();return {s.begin(),s.end()};
}
void hint(const char* text) { ImGui::PushStyleColor(ImGuiCol_Text,ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));ImGui::TextWrapped("%s",text);ImGui::PopStyleColor(); }
bool matches(std::string text,std::string query) {
    auto lower=[](unsigned char c){return static_cast<char>(c>='A'&&c<='Z'?c+32:c);};
    std::transform(text.begin(),text.end(),text.begin(),lower);
    std::transform(query.begin(),query.end(),query.begin(),lower);
    return text.find(query)!=std::string::npos;
}
void settings(const Package& p,const Json& entry) {
    if(!p.manifest.contains("config_schema")) return;
    for(const auto& o:p.manifest["config_schema"]["options"]) {
        const std::string id=o.at("id"),name=o.at("name");
        const auto values=entry.value("settings",Json::object());
        Json value=values.value(id,o.at("default"));
        ImGui::PushID(id.c_str());
        const auto type=o.at("type").get<std::string>();
        if(type=="Number") {
            float n=value.get<float>();
            ImGui::TextUnformatted(name.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if(rocket::ui::theme::slider_float("##value",&n,o.value("min",0.0F),o.value("max",1.0F),"%.2f"))
                attempt([&]{library().set_option(p.id,id,n);});
        } else if(type=="Enum") {
            const auto selected=value.get<std::string>();
            const auto widgets=p.metadata.value("api",1)==2?p.metadata.value("settings_ui",Json::object()):Json::object();
            if(widgets.value(id,std::string())=="toggle"&&o.at("options").size()==2) {
                bool checked=selected==o.at("options")[1].get<std::string>();
                if(ImGui::Checkbox(name.c_str(),&checked)) attempt([&]{library().set_option(p.id,id,o.at("options")[checked?1:0]);});
                if(o.contains("description")&&ImGui::IsItemHovered()) ImGui::SetTooltip("%s",o.at("description").get<std::string>().c_str());
                ImGui::PopID();continue;
            }
            ImGui::TextUnformatted(name.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if(ImGui::BeginCombo("##value",selected.c_str())) {
                for(const auto& choice:o.at("options")) {
                    const auto label=choice.get<std::string>();
                    if(ImGui::Selectable(label.c_str(),label==selected)) attempt([&]{library().set_option(p.id,id,label);});
                }
                ImGui::EndCombo();
            }
        } else if(type=="String") {
            std::array<char,4097> buffer{};
            std::snprintf(buffer.data(),buffer.size(),"%s",value.get<std::string>().c_str());
            ImGui::TextUnformatted(name.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if(ImGui::InputText("##value",buffer.data(),buffer.size(),ImGuiInputTextFlags_EnterReturnsTrue))
                attempt([&]{library().set_option(p.id,id,std::string(buffer.data()));});
        }
        if(o.contains("description")&&ImGui::IsItemHovered()) ImGui::SetTooltip("%s",o.at("description").get<std::string>().c_str());
        ImGui::PopID();
    }
}
void action_settings(const Package& package,const Json& entry) {
    if(package.metadata.value("api",1)!=2)return;
    if(package.metadata.value("api",1)!=2)return;
    const auto actions=package.metadata.value("input_actions",Json::object());if(actions.empty())return;
    ImGui::SeparatorText("MOD CONTROLS");
    const auto bindings=entry.value("bindings",Json::object());
    for(auto action=actions.begin();action!=actions.end();++action) {
        const auto id=action.key(),name=action.value().value("name",id);
        ImGui::PushID(id.c_str());ImGui::TextUnformatted(name.c_str());
        const auto assigned=bindings.value(id,Json::object());
        const bool wide=ImGui::GetContentRegionAvail().x>=660;
        if(ImGui::BeginTable("bindings",wide?2:1,ImGuiTableFlags_SizingStretchSame)) {
            if(wide) {
                ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::TextUnformatted("Keyboard / mouse");
                ImGui::TableSetColumnIndex(1);ImGui::TextUnformatted("Controller");ImGui::TableNextRow();
            }
            for(bool keyboard:{true,false}) {
                if(wide)ImGui::TableSetColumnIndex(keyboard?0:1);
                else {ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::TextUnformatted(keyboard?"Keyboard / mouse":"Controller");}
                const int source=assigned.value(keyboard?"keyboard":"controller",action.value().value(keyboard?"keyboard":"controller",-1));
                const auto label=keyboard?rocket::input::keyboard_binding_name(source):rocket::input::controller_binding_name(source);
                ImGui::PushID(keyboard?"keyboard":"controller");
                if(rocket::ui::theme::button(label.c_str(),{ImGui::GetContentRegionAvail().x,42}))
                    rocket::ui::begin_mod_binding_capture(name,keyboard,[owner=package.id,id,keyboard](int value){attempt([&]{library().set_action_binding(owner,id,keyboard,value);});});
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        const std::array<std::pair<int,const char*>,10> touch={{{0,"Unbound"},{0x8000,"A"},{0x4000,"B"},{0x2000,"Z"},{0x20,"L"},{0x10,"R"},{8,"C up"},{4,"C down"},{2,"C left"},{1,"C right"}}};
        const int mask=assigned.value("n64",action.value().value("n64",0));const char* touch_label="Button combination";
        for(const auto& [value,label]:touch)if(mask==value)touch_label=label;
        ImGui::TextUnformatted("Touch / N64 fallback");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if(ImGui::BeginCombo("##touch-fallback",touch_label)) {
            for(const auto& [value,label]:touch)if(ImGui::Selectable(label,mask==value))attempt([&]{library().set_action_touch(package.id,id,value);});
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }
}
void picker() {
    const float width=std::max(1.0F,std::min(740.0F,ImGui::GetIO().DisplaySize.x-40));
    ImGui::SetNextWindowSizeConstraints({width,0},{width,std::max(1.0F,ImGui::GetIO().DisplaySize.y-40)});
    if(!ImGui::BeginPopupModal("Add mods",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextWrapped("Choose an .nrm, .rtz or mod bundle .zip. Profiles use .rocket-profile.json.");
    ImGui::TextUnformatted("File or folder");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##file-or-folder",path_text.data(),path_text.size());
    if(rocket::ui::theme::button("OPEN")) {
        auto path=std::filesystem::u8path(path_text.data());
        if(std::filesystem::is_directory(path)) folder=path;
        else {import_file(path);if(!error)ImGui::CloseCurrentPopup();}
    }
    if(ImGui::GetContentRegionAvail().x>=500)ImGui::SameLine();
    if(rocket::ui::theme::button("UP")&&folder.has_parent_path()) folder=folder.parent_path();
    if(ImGui::GetContentRegionAvail().x>=420)ImGui::SameLine();if(rocket::ui::theme::button("REFRESH"))folder_scan_due={};
    if(ImGui::GetContentRegionAvail().x>=350)ImGui::SameLine();if(rocket::ui::theme::button("CLOSE")) ImGui::CloseCurrentPopup();
    ImGui::BeginChild("files",{0,std::min(300.0F,ImGui::GetIO().DisplaySize.y*0.4F)},true);
    refresh_folder();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(folder_entries.size()));
    bool changed_folder=false;
    while(!changed_folder&&clipper.Step()) for(int i=clipper.DisplayStart;i<clipper.DisplayEnd;++i) {
        const auto& e=folder_entries[i];
        if(ImGui::Selectable(e.label.c_str())) {
            if(e.directory){folder=e.path;changed_folder=true;break;}
            std::snprintf(path_text.data(),path_text.size(),"%s",utf8(e.path).c_str());
        }
    }
    if(!folder_error.empty())ImGui::TextWrapped("Cannot browse this folder: %s",folder_error.c_str());
    ImGui::EndChild();
    hint(utf8(folder).c_str());
    if(error)ImGui::TextWrapped("%s",status.c_str());
    ImGui::EndPopup();
}
}
void import_file(const std::filesystem::path& path) {
    attempt([&]{
        if(path.extension()==".json") library().import_profile(path);
        else library().install(path);
    },"Added to your selected profile.");
}
bool prepare_launch() {
    return attempt([&]{library().prepare_launch(library().snapshot().recovery);});
}
void launch_summary() {
    const auto view=library().snapshot_view();
    const auto& s=*view;
    ImGui::TextWrapped("Profile: %s",s.profile.at("name").get<std::string>().c_str());
    int count=0;
    for(const auto& entry:s.profile.at("mods"))if(entry.value("enabled",false))++count;
    if(count)ImGui::TextWrapped("%d mod%s enabled",count,count==1?"":"s");
    else hint("No mods enabled.");
    if(s.recovery)hint("The last session did not close normally. Your next launch will use Original Game. Open Mods to review your profile.");
    if(!status.empty()&&error)ImGui::TextWrapped("%s",status.c_str());
}
void draw(bool expand_details) {
    const auto view=library().snapshot_view();
    const auto& s=*view;
    ImGui::TextUnformatted("Mod profile");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if(ImGui::BeginCombo("##profile",s.profile.at("name").get<std::string>().c_str())) {
        for(const auto& p:s.profiles) {
            auto id=p.at("id").get<std::string>();
            if(ImGui::Selectable(p.at("name").get<std::string>().c_str(),id==s.profile.at("id").get<std::string>()))
                attempt([&]{library().select_profile(id);});
        }
        ImGui::EndCombo();
    }
    if(s.running) hint(("Playing: "+s.active.at("name").get<std::string>()+". Supported mods can switch on and off here. Installing packages or changing profiles needs a restart.").c_str());
    if(s.recovery) {
        ImGui::TextWrapped("Rocket-R did not close normally last time. The next launch will skip mods and use the original save folder.");
        if(rocket::ui::theme::button("TRY MY MODS AGAIN")) library().dismiss_recovery();
    }
    if(!status.empty())ImGui::TextColored(error?ImVec4(1,.5F,.4F,1):ImVec4(.4F,.9F,.8F,1),"%s",status.c_str());
    if(rocket::ui::theme::button("MANAGE PROFILES")) ImGui::OpenPopup("Manage profiles");
    const float modal_width=std::max(1.0F,std::min(680.0F,ImGui::GetIO().DisplaySize.x-40));
    ImGui::SetNextWindowSizeConstraints({modal_width,0},{modal_width,std::max(1.0F,ImGui::GetIO().DisplaySize.y-40)});
    if(ImGui::BeginPopupModal("Manage profiles",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {

            ImGui::TextWrapped("Keep different mod setups and saves separate. Original Game always uses your existing saves and never loads mods.");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##new-profile","Profile name",profile_name.data(),profile_name.size());
            if(rocket::ui::theme::button("CREATE EMPTY PROFILE"))attempt([&]{library().create_profile(profile_name.data(),false);});
            if(rocket::ui::theme::button("DUPLICATE SELECTED"))attempt([&]{library().create_profile(profile_name.data(),true);});
            ImGui::Spacing();
            if(rocket::ui::theme::button("EXPORT SELECTED PROFILE")) attempt([&]{
                auto path=library().root()/"exported-profiles"/(s.profile.at("id").get<std::string>()+".rocket-profile.json");
                library().export_profile(path);status="Saved profile to "+utf8(path);error=false;
            });
            hint("Exports contain the mod list, package hashes and settings. They do not contain mods or saves. Import one with Add Mods.");
            hint(("Data folder: "+utf8(library().root())).c_str());
        if(rocket::ui::theme::button("CLOSE",{-1,44}))ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if(ImGui::BeginTabBar("mod-tabs")) {
        if(ImGui::BeginTabItem("Installed")) {
            if(rocket::ui::theme::button("ADD MODS",{150,40})) {
                folder=library().root();
                folder_scan_due={};
                ImGui::OpenPopup("Add mods");
            }
            ImGui::SameLine();hint("You can also drop mod files onto the launcher.");
            picker();
            ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##search","Search installed mods",search.data(),search.size());
            const auto resolved=library().resolution_view();
            for(const auto& problem:resolved->errors) ImGui::TextWrapped("%s",problem.c_str());
            if(s.packages.empty()) {
                ImGui::Spacing();ImGui::TextWrapped("No mods installed yet. Install Modern Analogue Camera from Browse, or add a package you already have.");
            }
            std::vector<std::string> order;
            for(const auto& entry:s.profile.at("mods"))order.push_back(entry.at("id"));
            for(const auto& p:s.packages)if(std::find(order.begin(),order.end(),p.id)==order.end())order.push_back(p.id);
            for(const auto& id:order) {
                Json entry=Json::object();const Package* selected=nullptr;
                for(const auto& e:s.profile.at("mods"))if(e.at("id")==id)entry=e;
                for(const auto& p:s.packages)if(p.id==id&&(entry.empty()||entry.value("hash",std::string())==p.hash))selected=&p;
                if(!selected) continue;
                const auto& p=*selected;const auto name=p.manifest.at("display_name").get<std::string>();
                if(!matches(name+" "+id,search.data()))continue;
                ImGui::PushID(id.c_str());
                {
                rocket::ui::theme::Card mod_card("mod-card");
                bool enabled=entry.value("enabled",false);
                const bool live_toggle=library().can_toggle_live(id,p.hash);
                const Json* running_package = nullptr;
                if (s.running) for (const auto& active : s.active.at("packages")) if (active.at("id") == id) running_package=&active;
                const bool resident_toggle = running_package && running_package->value("live_toggle",false);
                if (resident_toggle) {
                    bool active = running_package->value("enabled",true);
                    if (ImGui::Checkbox("Active in this game", &active))
                        attempt([&]{library().set_active_enabled(id,active);}, id==included_camera().id?
                            (active?"Camera on.":"Camera off. Original controls restored."):(active?"Mod on.":"Mod off."));
                } else if(ImGui::Checkbox(s.running ? "Enabled next launch" : "##enabled",&enabled))
                    attempt([&]{library().set_enabled(id,enabled);},
                        s.running ? "Saved for next launch. Restart Rocket-R to change which mods are running." : "");
                ImGui::SameLine();ImGui::TextWrapped("%s",name.c_str());
                if (resident_toggle && !live_toggle) {
                    if (ImGui::Checkbox("Enabled next launch in selected profile", &enabled)) attempt([&]{library().set_enabled(id,enabled);});
                }
                hint((p.kind+"  |  v"+p.version).c_str());
                if (s.running) {
                    if (resident_toggle) {
                        const auto legacy = camera_status();
                        const auto managed = sdk::status(id);
                        const CameraStatus actual=id==included_camera().id?legacy:CameraStatus{managed.registered,managed.enabled,managed.requested};
                        const char* state = !actual.loaded ? "Loading: v" : actual.enabled != actual.requested
                            ? "Switching: v" : actual.enabled ? "On this session: v" : "Off, ready in standby: v";
                        hint((std::string(state) + running_package->at("version").get<std::string>()).c_str());
                    }
                    else if (running_package)
                        hint(("Running this session: v" + running_package->at("version").get<std::string>() +
                            (enabled ? "" : ". It will turn off after a restart.")).c_str());
                    else hint(enabled ? "Not running this session. Restart Rocket-R to load it." : "Not running this session.");
                }
                if (id == included_camera().id && p.hash != included_camera().hash) {
                    hint(("Included with this build: v" + included_camera().version).c_str());
                    if (rocket::ui::theme::button(("USE INCLUDED v" + included_camera().version).c_str()))
                        attempt([&]{library().install_bytes(rocket::generated::kCameraMod,".nrm");}, "Camera mod updated. Your profile and settings were kept. Restart the game to use it.");
                }
                ImGui::TextWrapped("%s",p.manifest.value("short_description",p.manifest.value("description",std::string())).c_str());
                if(expand_details) ImGui::SetNextItemOpen(true,ImGuiCond_Always);
                if(ImGui::TreeNode("Details and settings")) {
                    ImGui::TextWrapped("%s",p.manifest.value("description",std::string()).c_str());
                    std::string authors;
                    for(const auto& a:p.manifest.at("authors")){if(!authors.empty())authors+=", ";authors+=a.get<std::string>();}
                    hint(("By "+authors).c_str());hint(id.c_str());
                    if(p.metadata.value("api",1)==2) {
                        hint(sdk::managed_activation(p)?"SDK 2. Supports switching on and off during gameplay.":"SDK 2. Restart the game after switching this mod on or off.");
                        if(running_package) {
                            const auto state=sdk::status(id);
                            hint((std::string(state.registered?"SDK connected. Update callbacks: ":"SDK registration pending. Update callbacks: ")+std::to_string(state.ticks)).c_str());
                        }
                    }
                    if (s.running && p.manifest.contains("config_schema")) {
                        bool same_package = false;
                        for (const auto& active : s.active.at("packages")) if (active.at("id") == id)
                            same_package = active.at("hash") == p.hash;
                        if (!p.metadata.value("live_settings",false))
                            hint("This mod has not declared live settings support. Restart to apply its options.");
                        else if (same_package && s.active.at("profile") == s.profile.at("id") && (enabled || live_toggle))
                            hint(enabled ? "Options apply as soon as you change them and are saved to this profile." :
                                "Options are saved now and will be used when you switch this mod on.");
                        else hint("These options are saved for your next launch. Select the running profile and package to adjust it live.");
                    }
                    if(!entry.empty())settings(p,entry);
                    else hint("Enable this mod to change its settings.");
                    if (id == included_camera().id)
                        rocket::ui::draw_camera_mod_settings(ImGui::GetContentRegionAvail().x);
                    action_settings(p,entry);
                    if(p.metadata.value("api",1)==2) {
                        bool same=false;if(running_package)same=running_package->at("hash")==p.hash;
                        const auto current=sdk::status(id);
                        ImGui::BeginDisabled(!same||!current.enabled||!current.requested);
                        for(const auto& command:p.metadata.value("commands",Json::array())) {
                            if(rocket::ui::theme::button(command.at("name").get<std::string>().c_str()))attempt([&]{if(!sdk::request_command(id,command.at("id")))throw std::runtime_error("Start a game with this mod enabled to use this action.");},"Action sent to the mod.");
                            if(command.contains("description")&&ImGui::IsItemHovered())ImGui::SetTooltip("%s",command.at("description").get<std::string>().c_str());
                        }
                        ImGui::EndDisabled();
                    }
                    if(rocket::ui::theme::button("MOVE UP"))attempt([&]{library().move(id,-1);});
                    ImGui::SameLine();if(rocket::ui::theme::button("MOVE DOWN"))attempt([&]{library().move(id,1);});
                    ImGui::TextUnformatted("Installed version");
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if(ImGui::BeginCombo("##installed-version",p.version.c_str())) {
                        for(const auto& version:s.packages)if(version.id==id)
                            if(ImGui::Selectable((version.version+" ("+version.hash.substr(0,8)+")").c_str(),version.hash==p.hash))
                                attempt([&]{library().select_version(id,version.hash);});
                        ImGui::EndCombo();
                    }
                    hint(("SHA-256: "+p.hash).c_str());
                    ImGui::TreePop();
                }
                }
                ImGui::PopID();
            }
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Browse")) {
            {
            rocket::ui::theme::Card catalogue_card("catalogue-card", "MODERN ANALOGUE CAMERA");
            hint(("v" + included_camera().version + "  |  Included with Rocket-R  |  By ThatGuyMcd").c_str());
            ImGui::TextWrapped("Move the camera smoothly with a controller, keyboard or mouse. Open Details and settings in Installed to adjust the camera and remap its inputs.");
            bool current=false, installed=false;
            for (const auto& entry : s.profile.at("mods")) if (entry.at("id") == included_camera().id) {
                installed=true; current=entry.at("hash") == included_camera().hash;
            }
            const auto label = current ? "INCLUDED VERSION INSTALLED" : installed ? "UPDATE CAMERA MOD" : "INSTALL CAMERA MOD";
            ImGui::BeginDisabled(current);
            if(rocket::ui::theme::button(label,{300,46}))attempt([&]{library().install_bytes(rocket::generated::kCameraMod,".nrm");},"Camera mod installed. Your existing settings were kept.");
            ImGui::EndDisabled();
            ImGui::Spacing();ImGui::Separator();
            ImGui::TextWrapped("Have a mod package? Use Add Mods in Installed. This build includes an offline catalogue; community hosting is not connected yet.");
            hint("Code mods run inside the game process. Install packages from authors you trust.");
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}
}
