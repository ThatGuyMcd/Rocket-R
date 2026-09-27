#include "mods/mod_library.hpp"
#include "mods/sha256.hpp"
#include "miniz.h"
#include <iostream>
#include <functional>
#include <chrono>
using namespace rocket::mods;
int checks=0;
void need(bool b,const char* name){++checks;if(!b)throw std::runtime_error(name);}
void rejects(const std::function<void()>& f,const char* name){bool threw=false;try{f();}catch(const std::exception&){threw=true;}need(threw,name);}
std::vector<std::uint8_t> archive(const std::vector<std::pair<std::string,std::string>>& entries) {
    mz_zip_archive z{};
    if(!mz_zip_writer_init_heap(&z,0,0))throw std::runtime_error("zip init");
    for(const auto& [name,data]:entries)if(!mz_zip_writer_add_mem(&z,name.c_str(),data.data(),data.size(),MZ_BEST_COMPRESSION))throw std::runtime_error("zip add");
    void* ptr=nullptr;size_t length=0;
    if(!mz_zip_writer_finalize_heap_archive(&z,&ptr,&length))throw std::runtime_error("zip finalize");
    std::vector<std::uint8_t> out(static_cast<std::uint8_t*>(ptr),static_cast<std::uint8_t*>(ptr)+length);
    mz_free(ptr);mz_zip_writer_end(&z);return out;
}
Json manifest(const std::string& id){
 return {{"id",id},{"version","1.0.0"},{"display_name",id},{"game_id","rocket"},{"minimum_recomp_version","1.1.0"},{"authors",Json::array({"Test"})}};
}
std::vector<std::uint8_t> mod(const Json& m,const Json& metadata=Json::object()){
 std::vector<std::pair<std::string,std::string>> files{{"mod.json",m.dump()}};
 if(!metadata.empty())files.emplace_back("rocket.json",metadata.dump());
 return archive(files);
}
int main(){
 const auto dir=std::filesystem::temp_directory_path()/("rocket-mod-tests-"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
 try {
    std::string abc="abc";
    need(sha256({reinterpret_cast<const std::uint8_t*>(abc.data()),abc.size()})=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","sha256 abc");
    need(sha256({})=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","sha256 empty");
    std::string million(1000000,'a');
    need(sha256({reinterpret_cast<const std::uint8_t*>(million.data()),million.size()})=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0","sha256 multiblock");
    need(valid_id("camera_test")&&!valid_id("../camera")&&!valid_id("Camera"),"safe ID");
    auto m=manifest("camera");auto bytes=mod(m);
    need(inspect_package(bytes,".nrm").id=="camera","valid package");
    rejects([&]{inspect_package(archive({{"mod.json",m.dump()},{"../escape","bad"}}),".nrm");},"path traversal");
    rejects([&]{inspect_package(archive({{"mod.json",m.dump()},{"A.txt","1"},{"a.txt","2"}}),".nrm");},"case collision");
    rejects([&]{inspect_package(archive({{"mod.json",m.dump()},{"C:/evil","x"}}),".nrm");},"drive path");
    rejects([&]{auto x=m;x["game_id"]="bk";inspect_package(mod(x),".nrm");},"wrong game");
    rejects([&]{auto x=m;x["native_libraries"]=Json::array({{{"name","native"}}});inspect_package(mod(x),".nrm");},"native library");
    rejects([&]{inspect_package(archive({{"mod.json",m.dump()},{"mod_syms.bin","bad"}}),".nrm");},"partial code");
    Library lib;lib.open(dir,"1.1.0");
    need(lib.snapshot().profile.at("id")=="original","original profile");
    lib.install_bytes(bytes,".nrm");
    need(lib.snapshot().profile.at("id")!="original","install separate profile");
    need(lib.resolve().packages.size()==1,"resolve installed");
    auto main_id=lib.snapshot().profile.at("id").get<std::string>();
    lib.select_profile("original");need(lib.resolve().packages.empty(),"original remains empty");
    lib.select_profile(main_id);
    auto dependent=manifest("dependent");dependent["dependencies"]=Json::array({"missing:1.0.0"});
    lib.install_bytes(mod(dependent),".nrm");
    need(!lib.resolve(),"missing dependency");
    lib.install_bytes(mod(manifest("missing")),".nrm");
    auto resolved=lib.resolve();need(static_cast<bool>(resolved),"dependency found");
    auto dep_pos=std::find_if(resolved.packages.begin(),resolved.packages.end(),[](auto& p){return p.id=="dependent";});
    auto req_pos=std::find_if(resolved.packages.begin(),resolved.packages.end(),[](auto& p){return p.id=="missing";});
    need(req_pos<dep_pos,"dependency before dependent");
    lib.set_enabled("missing",false);need(!lib.resolve(),"explicitly disabled dependency");
    lib.set_enabled("missing",true);
    auto a=manifest("cycle_a");auto b=manifest("cycle_b");
    a["dependencies"]=Json::array({"cycle_b"});b["dependencies"]=Json::array({"cycle_a"});
    lib.install_bytes(mod(a),".nrm");lib.install_bytes(mod(b),".nrm");
    need(!lib.resolve(),"cycles rejected");lib.set_enabled("cycle_a",false);lib.set_enabled("cycle_b",false);
    auto settings=manifest("settings");
    settings["config_schema"]={{"options",Json::array({{{"id","speed"},{"name","Speed"},{"type","Number"},{"min",1},{"max",10},{"default",3}}})}};
    lib.install_bytes(mod(settings),".nrm");lib.set_option("settings","speed",7);
    auto newer_settings=settings;newer_settings["version"]="1.0.1";
    const auto profile_before_update=lib.snapshot().profile.at("id");
    lib.set_enabled("settings",false);
    lib.install_bytes(mod(newer_settings),".nrm");
    need(lib.snapshot().profile.at("id")==profile_before_update,"mod update preserves save profile identity");
    const auto updated_profile=lib.snapshot().profile;
    for(const auto& entry:updated_profile.at("mods"))if(entry.at("id")=="settings") {
        need(entry.at("settings").at("speed")==7&&!entry.at("enabled").get<bool>(),"update preserves options and enabled state");
        need(entry.at("hash")==inspect_package(mod(newer_settings),".nrm").hash,"update selects the new package");
    }
    lib.set_enabled("settings",true);
    rejects([&]{lib.set_option("settings","speed",11);},"out of range");
    lib.export_profile(dir/"share.json");
    const auto original_profile=lib.snapshot().profile;
    lib.import_profile(dir/"share.json");
    need(lib.snapshot().profile.at("mods")==original_profile.at("mods"),"profile round trip");
    need(lib.snapshot().profile.at("id")!=original_profile.at("id"),"profile gets distinct identity");
    auto runtime=lib.prepare_launch();
    need(std::filesystem::exists(runtime/"mods/camera.nrm"),"launch materialized package");
    need(lib.active_save_path()!=dir/"saves","modded save isolated");
    need(read_json_file(runtime/"mod_config/settings.json").at("storage").at("speed")==7,"launch config persisted");
    rejects([&]{lib.prepare_launch();},"no in-process restart");
    const auto active=lib.snapshot().active;
    lib.set_enabled("camera",false);
    need(lib.snapshot().active==active,"active plan immutable");
    Library recovery;recovery.open(dir,"1.1.0");need(recovery.snapshot().recovery,"abnormal session detected");
    lib.finish_session();Library clean;clean.open(dir,"1.1.0");need(!clean.snapshot().recovery,"clean session");
    clean.select_profile("original");clean.prepare_launch();
    need(clean.active_save_path()==dir/"saves","original save location");
    clean.finish_session();
    auto invalid=manifest("too_new");invalid["minimum_recomp_version"]="9.0.0";
    const auto before=lib.snapshot().profile;
    rejects([&]{lib.install_bytes(mod(invalid),".nrm");},"future engine");
    need(lib.snapshot().profile==before,"failed install preserves profile");
    const auto badbundle=archive({{"one.nrm",std::string(bytes.begin(),bytes.end())},{"two.nrm","bad"}});
    const auto count=lib.snapshot().packages.size();
    rejects([&]{lib.install_bytes(badbundle,".zip");},"invalid bundle");
    need(lib.snapshot().packages.size()==count,"bundle validation before commit");
    rejects([&]{auto x=m;x["version"]="1.0.0-";inspect_package(mod(x),".nrm");},"empty version label");
    rejects([&]{inspect_package(mod(m,{{"schema",1},{"conflicts",12}}),".nrm");},"invalid conflict schema");
    rejects([&]{inspect_package(mod(m,{{"schema",1},{"live_settings","yes"}}),".nrm");},"invalid live settings declaration");
    Library tampered;tampered.open(dir/"tampered","1.1.0");tampered.install_bytes(bytes,".nrm");
    auto tampered_package=tampered.snapshot().packages.at(0);
    auto index=read_json_file(dir/"tampered/mod-library"/(tampered_package.hash+".json"));
    index["manifest"]["display_name"]="changed";
    write_json_atomic(dir/"tampered/mod-library"/(tampered_package.hash+".json"),index);
    Library altered;altered.open(dir/"tampered","1.1.0");
    rejects([&]{altered.prepare_launch();},"metadata tampering blocks launch");
    write_json_atomic(dir/"damaged/mod-profiles.json",{{"profiles",12}});
    Library damaged;damaged.open(dir/"damaged","1.1.0");
    need(damaged.snapshot().profile.at("id")=="original"&&damaged.snapshot().recovery,"invalid profiles recover to original");
    Library live;live.open(dir/"live","1.1.0");
    auto live_manifest=manifest("live_settings");
    live_manifest["config_schema"]={{"options",Json::array({
        {{"id","speed"},{"name","Speed"},{"type","Number"},{"min",1},{"max",10},{"default",3}},
        {{"id","invert"},{"name","Invert"},{"type","Enum"},{"options",Json::array({"Off","On"})},{"default","Off"}},
        {{"id","label"},{"name","Label"},{"type","String"},{"default","Start"}}
    })}};
    live.install_bytes(mod(live_manifest),".nrm");
    const auto live_id=live.snapshot().profile.at("id").get<std::string>();
    const auto live_hash=live.resolve().packages.at(0).hash;
    live.prepare_launch();
    std::vector<OptionUpdate> updates;
    live.set_option("live_settings","speed",4);
    need(updates.empty(),"settings do not enter an uninitialized runtime");
    live.set_live_option_handler([&](const OptionUpdate& update){updates.push_back(update);});
    live.set_option("live_settings","speed",5.5);
    live.set_option("live_settings","invert","On");
    live.set_option("live_settings","label","Updated");
    need(updates.size()==3 && updates[0].mod_id=="live_settings" && updates[0].option_id=="speed" &&
         std::get<double>(updates[0].value)==5.5 && std::get<std::uint32_t>(updates[1].value)==1 &&
         std::get<std::string>(updates[2].value)=="Updated","live options use the runtime's number, enum-index and string types");
    need(live.snapshot().active.at("packages")[0].at("settings").at("invert")=="On","running snapshot reports changed settings");
    Library saved_live;saved_live.open(dir/"live","1.1.0");
    need(saved_live.snapshot().profile.at("mods")[0].at("settings").at("speed")==5.5,"live settings also survive restart");
    rejects([&]{live.set_option("live_settings","speed",99);},"invalid live number rejected");
    rejects([&]{live.set_option("live_settings","invert","Unknown");},"invalid live enum rejected");
    live.create_profile("Next game",true);
    live.set_option("live_settings","speed",7);
    need(updates.size()==3,"another profile cannot alter the running game");
    live.select_profile(live_id);
    auto live_newer=live_manifest;live_newer["version"]="1.0.1";
    live.install_bytes(mod(live_newer),".nrm");
    live.set_option("live_settings","speed",8);
    need(updates.size()==3,"another package version cannot alter the running game");
    live.select_version("live_settings",live_hash);
    live.set_enabled("live_settings",false);
    live.set_option("live_settings","speed",9);
    need(updates.size()==3,"disabled next-launch selection cannot alter the running game");
    live.set_enabled("live_settings",true);
    live.set_option("live_settings","speed",6);
    need(updates.size()==4 && std::get<double>(updates.back().value)==6,"returning to the running package restores live changes");
    live.finish_session();live.set_option("live_settings","speed",2);
    need(updates.size()==4,"no settings callback survives runtime shutdown");
    Library toggle;toggle.open(dir/"toggle","1.1.0");
    auto toggle_manifest=manifest("camera_toggle");
    toggle_manifest["config_schema"]=live_manifest["config_schema"];
    const Json camera_resource={{"schema",1},{"exclusive_resources",Json::array({"camera.orbit"})}};
    const auto toggle_bytes=mod(toggle_manifest,camera_resource);
    const auto toggle_hash=inspect_package(toggle_bytes,".nrm").hash;
    toggle.install_bytes(toggle_bytes,".nrm");
    toggle.set_enabled("camera_toggle",false);
    toggle.install_bytes(mod(manifest("ordinary_disabled")),".nrm");
    toggle.set_enabled("ordinary_disabled",false);
    toggle.allow_live_toggle("camera_toggle",toggle_hash);
    toggle.prepare_launch();
    const auto standby=toggle.snapshot().active.at("packages");
    need(standby.size()==1 && standby[0].at("id")=="camera_toggle" && !standby[0].at("enabled").get<bool>() &&
         standby[0].at("live_toggle").get<bool>(),"only the approved disabled package loads in standby");
    std::vector<bool> switches;
    need(!toggle.can_toggle_live("camera_toggle",toggle_hash),"toggle waits until runtime initialization");
    toggle.set_live_toggle_handler([&](const std::string& id,bool on){need(id=="camera_toggle","toggle identifies the loaded mod");switches.push_back(on);});
    toggle.set_live_option_handler([&](const OptionUpdate& update){updates.push_back(update);});
    toggle.set_option("camera_toggle","invert","On");
    need(updates.back().mod_id=="camera_toggle" && std::get<std::uint32_t>(updates.back().value)==1,
        "settings changed in standby reach the resident mod before it is enabled");
    need(toggle.can_toggle_live("camera_toggle",toggle_hash),"standby camera exposes a live switch");
    toggle.set_enabled("camera_toggle",true);
    need(switches==std::vector<bool>{true} && toggle.snapshot().active.at("packages")[0].at("enabled").get<bool>(),
        "starting disabled then enabling changes the running session");
    toggle.set_enabled("camera_toggle",false);
    need(switches==std::vector<bool>({true,false}) && !toggle.snapshot().active.at("packages")[0].at("enabled").get<bool>(),
        "live disable records standby and persists the next-launch choice");
    auto alternate=toggle_manifest;alternate["version"]="1.0.1";
    toggle.install_bytes(mod(alternate,camera_resource),".nrm");
    toggle.set_enabled("camera_toggle",true);
    need(switches.size()==2 && !toggle.can_toggle_live("camera_toggle",inspect_package(mod(alternate,camera_resource),".nrm").hash),
        "changing package version cannot retarget an existing live switch");
    toggle.set_active_enabled("camera_toggle",true);
    toggle.set_active_enabled("camera_toggle",false);
    need(switches==std::vector<bool>({true,false,true,false}) &&
         !toggle.snapshot().active.at("packages")[0].at("enabled").get<bool>() &&
         toggle.snapshot().profile.at("mods")[0].at("enabled").get<bool>(),
         "explicit running-game toggle operates the resident version without overwriting a next-launch version");
    const auto playing_profile=toggle.snapshot().profile.at("id").get<std::string>();
    toggle.create_profile("Next launch",false);
    toggle.set_active_enabled("camera_toggle",true);
    need(toggle.snapshot().profile.at("mods").empty() && toggle.snapshot().active.at("packages")[0].at("enabled").get<bool>(),
         "running-game toggle still works while a different profile is selected");
    toggle.select_profile(playing_profile);
    toggle.finish_session();toggle.set_enabled("camera_toggle",false);toggle.prepare_launch();
    need(toggle.snapshot().active.at("packages").empty(),"unapproved package versions are never preloaded");
    toggle.finish_session();toggle.select_version("camera_toggle",toggle_hash);
    toggle.install_bytes(mod(manifest("other_camera"),camera_resource),".nrm");
    toggle.prepare_launch();
    need(toggle.snapshot().active.at("packages").size()==1 && toggle.snapshot().active.at("packages")[0].at("id")=="other_camera",
        "standby camera does not conflict with another enabled camera mod");
    toggle.finish_session();toggle.set_enabled("other_camera",false);toggle.set_enabled("camera_toggle",true);
    auto needs_camera=manifest("needs_camera");needs_camera["dependencies"]=Json::array({"camera_toggle"});
    toggle.install_bytes(mod(needs_camera),".nrm");toggle.prepare_launch();
    const auto required_camera_session=toggle.snapshot();
    for(const auto& p:required_camera_session.active.at("packages")) if(p.at("id")=="camera_toggle")
        need(!p.at("live_toggle").get<bool>(),"a mod required by another running mod cannot be switched off live");
    toggle.finish_session();toggle.prepare_launch(true);
    need(toggle.snapshot().active.at("packages").empty(),"Original Game and recovery launch never preload a standby mod");
    toggle.finish_session();
    // Release numbering must not strand an approved development camera profile.
    const auto upgrade_root = dir / "release-upgrade";
    Library development; development.open(upgrade_root,"1.1.0");
    auto old_camera = settings; old_camera["id"] = "release_camera";
    const auto old_bytes = mod(old_camera);
    const auto old_hash = inspect_package(old_bytes,".nrm").hash;
    development.install_bytes(old_bytes,".nrm");
    development.set_option("release_camera","speed",7);
    development.set_enabled("release_camera",false);
    const auto first_profile = development.snapshot().profile.at("id").get<std::string>();
    development.create_profile("Another save",true);
    auto alternate_camera = old_camera; alternate_camera["version"] = "1.0.2";
    development.create_profile("Custom version",true);
    development.install_bytes(mod(alternate_camera),".nrm");
    const auto profiles_before = development.snapshot().profiles;
    development.select_profile("original");
    write_json_atomic(upgrade_root/"profile-saves"/first_profile/"keep.json",{{"save",42}});
    auto release_camera = old_camera;
    release_camera["version"] = "1.0.3";
    release_camera["minimum_recomp_version"] = "1.0.1";
    const auto release_bytes = mod(release_camera);
    const auto release_hash = inspect_package(release_bytes,".nrm").hash;
    Library release; release.open(upgrade_root,"1.0.1");
    release.migrate_bundled_package(release_bytes,old_hash);
    need(release.snapshot().profile.at("id")=="original","migration keeps the selected profile");
    auto expected_profiles = profiles_before;
    for(auto& p:expected_profiles) for(auto& entry:p["mods"])
        if(entry["id"]=="release_camera" && entry["hash"]==old_hash) entry["hash"]=release_hash;
    need(release.snapshot().profiles==expected_profiles,"migration preserves profiles, options, disabled state and other versions");
    need(std::filesystem::exists(upgrade_root/"mod-library"/(old_hash+".nrm")),"migration keeps the old package");
    need(read_json_file(upgrade_root/"profile-saves"/first_profile/"keep.json").at("save")==42,"migration keeps save data");
    release.migrate_bundled_package(release_bytes,old_hash);
    need(release.snapshot().profiles==expected_profiles,"migration is idempotent");
    release.select_profile(first_profile); release.set_enabled("release_camera",true);
    need(static_cast<bool>(release.resolve()),"release camera resolves on 1.0.1");
    release.prepare_launch(); release.finish_session();
    rejects([&]{release.install_bytes(mod(alternate_camera),".nrm");},"release still rejects unrelated 1.1.0 requirements");
    const auto texture = archive({{"rt64.json","{}"}});
    release.install_bytes(texture,".rtz");
    const auto texture_package = inspect_package(texture,".rtz");
    const auto texture_index_path = upgrade_root/"mod-library"/(texture_package.hash+".json");
    auto texture_index = read_json_file(texture_index_path);
    texture_index["manifest"]["minimum_recomp_version"]="1.1.0";
    write_json_atomic(texture_index_path,texture_index);
    Library reopened; reopened.open(upgrade_root,"1.0.1");
    need(static_cast<bool>(reopened.resolve()),"unlabelled development texture pack remains compatible");
    reopened.prepare_launch(); reopened.finish_session();
    need(read_json_file(texture_index_path).at("manifest").at("minimum_recomp_version")=="1.0.1","generated texture metadata uses the release baseline");
    // Only this uniquely created test directory is removed.
    std::filesystem::remove_all(dir);
    std::cout<<checks<<" mod checks passed\n";return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\nFixtures: "<<dir<<"\n";return 1;}
}
