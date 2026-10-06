#if defined(__ANDROID__)
#include "mod_library.hpp"
#include "mod_runtime.hpp"
#include "sdk_runtime.hpp"
#include "mod_camera.generated.hpp"
#include <jni.h>

namespace {
std::string utf(JNIEnv* env,jstring value) {
    if(!value) return {};
    const char* p=env->GetStringUTFChars(value,nullptr);
    std::string result=p?p:"";
    if(p)env->ReleaseStringUTFChars(value,p);
    return result;
}
}
extern "C" JNIEXPORT jstring JNICALL
Java_com_rocketret_rocketr_ModActivity_nativeCommand(JNIEnv* env,jclass,jstring root,jstring request) {
    using namespace rocket::mods;
    Json answer;
    try {
        auto& lib=library();
        if(lib.root().empty()) {
            lib.open(std::filesystem::u8path(utf(env,root)),ROCKET_R_VERSION);
            configure_library();
        }
        const auto command=Json::parse(utf(env,request));
        const std::string op=command.at("op");
        if(op=="install")lib.install(std::filesystem::u8path(command.at("path").get<std::string>()));
        else if(op=="camera")lib.install_bytes(rocket::generated::kCameraMod,".nrm");
        else if(op=="enable")lib.set_enabled(command.at("id"),command.at("enabled"));
        else if(op=="select")lib.select_profile(command.at("id"));
        else if(op=="create")lib.create_profile(command.at("name"),command.value("duplicate",false));
        else if(op=="option")lib.set_option(command.at("id"),command.at("key"),command.at("value"));
        else if(op=="binding") {
            if(command.value("device",std::string())=="n64")lib.set_action_touch(command.at("id"),command.at("key"),command.at("value"));
            else lib.set_action_binding(command.at("id"),command.at("key"),command.at("device")=="keyboard",command.at("value"));
        }
        else if(op=="command") {if(!sdk::request_command(command.at("id"),command.at("key")))throw std::runtime_error("Start a game with this mod enabled to use this action.");}
        else if(op=="retry")lib.dismiss_recovery();
        else if(op!="snapshot")throw std::runtime_error("Unknown mod command.");
        auto s=lib.snapshot();Json packages=Json::array();
        for(const auto& p:s.packages) packages.push_back({{"id",p.id},{"hash",p.hash},{"manifest",p.manifest},{"metadata",p.metadata},{"live_toggle",lib.can_toggle_live(p.id,p.hash)}});
        static const auto camera = inspect_package(rocket::generated::kCameraMod, ".nrm");
        answer={{"ok",true},{"profiles",s.profiles},{"profile",s.profile},{"packages",packages},{"recovery",s.recovery},{"errors",lib.resolve().errors},{"running",s.running},{"active",s.active},
            {"included_camera",{{"version",camera.version},{"hash",camera.hash}}}};
    }catch(const std::exception& e){answer={{"ok",false},{"error",e.what()}};}
    return env->NewStringUTF(answer.dump(-1,' ',true,Json::error_handler_t::replace).c_str());
}
#endif
