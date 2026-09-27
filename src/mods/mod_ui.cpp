#include "mod_ui.hpp"
#include "mod_library.hpp"
#include "mod_runtime.hpp"
#include "mod_camera.generated.hpp"
#include "runtime_ui.hpp"
#include "imgui/imgui.h"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <set>

namespace rocket::mods::ui {
namespace {
std::string status;
bool error=false;
std::array<char,1024> path_text{};
std::array<char,128> search{}, profile_name{};
std::filesystem::path folder;
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
            ImGui::SetNextItemWidth(std::min(330.0F,ImGui::GetContentRegionAvail().x*0.6F));
            if(ImGui::SliderFloat(name.c_str(),&n,o.value("min",0.0F),o.value("max",1.0F),"%.2f"))
                attempt([&]{library().set_option(p.id,id,n);});
        } else if(type=="Enum") {
            const auto selected=value.get<std::string>();
            ImGui::SetNextItemWidth(200);
            if(ImGui::BeginCombo(name.c_str(),selected.c_str())) {
                for(const auto& choice:o.at("options")) {
                    const auto label=choice.get<std::string>();
                    if(ImGui::Selectable(label.c_str(),label==selected)) attempt([&]{library().set_option(p.id,id,label);});
                }
                ImGui::EndCombo();
            }
        } else if(type=="String") {
            std::array<char,4097> buffer{};
            std::snprintf(buffer.data(),buffer.size(),"%s",value.get<std::string>().c_str());
            if(ImGui::InputText(name.c_str(),buffer.data(),buffer.size(),ImGuiInputTextFlags_EnterReturnsTrue))
                attempt([&]{library().set_option(p.id,id,std::string(buffer.data()));});
        }
        if(o.contains("description")&&ImGui::IsItemHovered()) ImGui::SetTooltip("%s",o.at("description").get<std::string>().c_str());
        ImGui::PopID();
    }
}
void picker() {
    if(!ImGui::BeginPopupModal("Add mods",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextWrapped("Choose an .nrm, .rtz or mod bundle .zip. Profiles use .rocket-profile.json.");
    ImGui::SetNextItemWidth(600);
    ImGui::InputText("File or folder",path_text.data(),path_text.size());
    if(ImGui::Button("OPEN")) {
        auto path=std::filesystem::u8path(path_text.data());
        if(std::filesystem::is_directory(path)) folder=path;
        else {import_file(path);if(!error)ImGui::CloseCurrentPopup();}
    }
    ImGui::SameLine();
    if(ImGui::Button("UP")&&folder.has_parent_path()) folder=folder.parent_path();
    ImGui::SameLine();if(ImGui::Button("CLOSE")) ImGui::CloseCurrentPopup();
    ImGui::BeginChild("files",{650,300},true);
    std::error_code ec;
    std::vector<std::filesystem::directory_entry> entries;
    for(const auto& e:std::filesystem::directory_iterator(folder,ec)) entries.push_back(e);
    std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.path().filename()<b.path().filename();});
    for(const auto& e:entries) {
        const bool directory=e.is_directory(ec);
        const auto ext=e.path().extension().string();
        if(!directory&&ext!=".nrm"&&ext!=".rtz"&&ext!=".zip"&&ext!=".json")continue;
        const auto name=(directory?"[Folder] ":"")+utf8(e.path().filename());
        if(ImGui::Selectable(name.c_str())) {
            if(directory){folder=e.path();break;}
            std::snprintf(path_text.data(),path_text.size(),"%s",utf8(e.path()).c_str());
        }
    }
    if(ec)ImGui::TextWrapped("Cannot browse this folder: %s",ec.message().c_str());
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
    auto s=library().snapshot();
    ImGui::TextWrapped("Profile: %s",s.profile.at("name").get<std::string>().c_str());
    if(s.recovery)hint("The last session did not close normally. Your next launch will use Original Game. Open Mods to review your profile.");
    if(!status.empty()&&error)ImGui::TextWrapped("%s",status.c_str());
}
void draw() {
    auto s=library().snapshot();
    ImGui::SetNextItemWidth(std::min(300.0F,ImGui::GetContentRegionAvail().x));
    if(ImGui::BeginCombo("Profile",s.profile.at("name").get<std::string>().c_str())) {
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
        if(ImGui::Button("TRY MY MODS AGAIN")) library().dismiss_recovery();
    }
    if(!status.empty())ImGui::TextColored(error?ImVec4(1,.5F,.4F,1):ImVec4(.4F,.9F,.8F,1),"%s",status.c_str());
    if(ImGui::BeginTabBar("mod-tabs")) {
        if(ImGui::BeginTabItem("Installed")) {
            if(ImGui::Button("ADD MODS",{150,40})) {
                folder=library().root();
                ImGui::OpenPopup("Add mods");
            }
            ImGui::SameLine();hint("You can also drop mod files onto the launcher.");
            picker();
            ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##search","Search installed mods",search.data(),search.size());
            const auto resolved=library().resolve();
            for(const auto& problem:resolved.errors) ImGui::TextWrapped("%s",problem.c_str());
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
                ImGui::PushID(id.c_str());ImGui::Spacing();ImGui::Separator();
                bool enabled=entry.value("enabled",false);
                const bool live_toggle=library().can_toggle_live(id,p.hash);
                const Json* running_package = nullptr;
                if (s.running) for (const auto& active : s.active.at("packages")) if (active.at("id") == id) running_package=&active;
                const bool resident_toggle = running_package && running_package->value("live_toggle",false);
                if (resident_toggle) {
                    bool active = running_package->value("enabled",true);
                    if (ImGui::Checkbox("Active in this game", &active))
                        attempt([&]{library().set_active_enabled(id,active);}, active ? "Camera on." : "Camera off. Original controls restored.");
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
                        const auto actual = camera_status();
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
                    if (ImGui::Button(("USE INCLUDED v" + included_camera().version).c_str()))
                        attempt([&]{library().install_bytes(rocket::generated::kCameraMod,".nrm");}, "Camera mod updated. Your profile and settings were kept. Restart the game to use it.");
                }
                ImGui::TextWrapped("%s",p.manifest.value("short_description",p.manifest.value("description",std::string())).c_str());
                if(ImGui::TreeNode("Details and settings")) {
                    ImGui::TextWrapped("%s",p.manifest.value("description",std::string()).c_str());
                    std::string authors;
                    for(const auto& a:p.manifest.at("authors")){if(!authors.empty())authors+=", ";authors+=a.get<std::string>();}
                    hint(("By "+authors).c_str());hint(id.c_str());
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
                    if(ImGui::Button("MOVE UP"))attempt([&]{library().move(id,-1);});
                    ImGui::SameLine();if(ImGui::Button("MOVE DOWN"))attempt([&]{library().move(id,1);});
                    if(ImGui::BeginCombo("Installed version",p.version.c_str())) {
                        for(const auto& version:s.packages)if(version.id==id)
                            if(ImGui::Selectable((version.version+" ("+version.hash.substr(0,8)+")").c_str(),version.hash==p.hash))
                                attempt([&]{library().select_version(id,version.hash);});
                        ImGui::EndCombo();
                    }
                    hint(("SHA-256: "+p.hash).c_str());
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Browse")) {
            ImGui::Spacing();ImGui::TextUnformatted("MODERN ANALOGUE CAMERA");
            hint(("v" + included_camera().version + "  |  Included with Rocket-R  |  By ThatGuyMcd").c_str());
            ImGui::TextWrapped("Move the camera smoothly with a controller, keyboard or mouse. Open Details and settings in Installed to adjust the camera and remap its inputs.");
            bool current=false, installed=false;
            for (const auto& entry : s.profile.at("mods")) if (entry.at("id") == included_camera().id) {
                installed=true; current=entry.at("hash") == included_camera().hash;
            }
            const auto label = current ? "INCLUDED VERSION INSTALLED" : installed ? "UPDATE CAMERA MOD" : "INSTALL CAMERA MOD";
            ImGui::BeginDisabled(current);
            if(ImGui::Button(label,{300,46}))attempt([&]{library().install_bytes(rocket::generated::kCameraMod,".nrm");},"Camera mod installed. Your existing settings were kept.");
            ImGui::EndDisabled();
            ImGui::Spacing();ImGui::Separator();
            ImGui::TextWrapped("Have a mod package? Use Add Mods in Installed. This build includes an offline catalogue; community hosting is not connected yet.");
            hint("Code mods run inside the game process. Install packages from authors you trust.");
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Profiles")) {
            ImGui::TextWrapped("Keep different mod setups and saves separate. Original Game always uses your existing saves and never loads mods.");
            ImGui::InputTextWithHint("##new-profile","Profile name",profile_name.data(),profile_name.size());
            if(ImGui::Button("CREATE EMPTY PROFILE"))attempt([&]{library().create_profile(profile_name.data(),false);});
            ImGui::SameLine();if(ImGui::Button("DUPLICATE SELECTED"))attempt([&]{library().create_profile(profile_name.data(),true);});
            ImGui::Spacing();
            if(ImGui::Button("EXPORT SELECTED PROFILE")) attempt([&]{
                auto path=library().root()/"exported-profiles"/(s.profile.at("id").get<std::string>()+".rocket-profile.json");
                library().export_profile(path);status="Saved profile to "+utf8(path);error=false;
            });
            hint("Exports contain the mod list, package hashes and settings. They do not contain mods or saves. Import one with Add Mods.");
            hint(("Data folder: "+utf8(library().root())).c_str());
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}
}
