#include "mod_library.hpp"
#include "sha256.hpp"
#include "miniz.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

namespace rocket::mods {
namespace {
constexpr std::size_t kPackageLimit=256ULL*1024*1024;
constexpr std::size_t kExpandedLimit=512ULL*1024*1024;
constexpr std::size_t kMetadataLimit=256*1024;
[[noreturn]] void fail(const std::string& s) { throw std::runtime_error(s); }
std::string lower(std::string s) {
    for (auto& c:s) if(c>='A'&&c<='Z') c+= 'a'-'A';
    return s;
}
std::array<unsigned,3> semver(const std::string& s) {
    std::array<unsigned,3> out{}; std::size_t pos=0;
    for (unsigned i=0;i<3;++i) {
        if(pos>=s.size()||s[pos]<'0'||s[pos]>'9') fail("Invalid version: "+s);
        const auto start=pos;
        while(pos<s.size()&&s[pos]>='0'&&s[pos]<='9') {
            if(pos-start>7) fail("Version component is too large.");
            out[i]=out[i]*10+static_cast<unsigned>(s[pos++]-'0');
        }
        if(pos-start>1&&s[start]=='0') fail("Version has a leading zero.");
        if(i<2) { if(pos>=s.size()||s[pos++]!='.') fail("Invalid version: "+s); }
    }
    if(pos<s.size()) {
        if(s[pos]!='-'&&s[pos]!='+') fail("Invalid version: "+s);
        bool empty=true;
        for(++pos;pos<s.size();++pos) {
            const char c=s[pos];
            if(c=='.'||c=='+') {if(empty)fail("Invalid version label.");empty=true;}
            else if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-')empty=false;
            else fail("Invalid version label.");
        }
        if(empty)fail("Invalid version label.");
    }
    return out;
}
class Archive {
public:
    explicit Archive(std::span<const std::uint8_t> bytes) {
        if(bytes.size()>kPackageLimit) fail("Package exceeds the 256 MiB import limit.");
        if(!mz_zip_reader_init_mem(&zip_,bytes.data(),bytes.size(),0)) fail("This is not a readable ZIP package.");
        try {
            auto count=mz_zip_reader_get_num_files(&zip_);
            if(count==0||count>16384) fail("Package contains too many files or is empty.");
            std::set<std::string> names;
            std::uint64_t total=0;
            for(mz_uint i=0;i<count;++i) {
                mz_zip_archive_file_stat s{};
                if(!mz_zip_reader_file_stat(&zip_,i,&s)) fail("Invalid package directory.");
                std::string name=s.m_filename;
                if(name.empty()||name.size()>240||name.front()=='/'||name.find('\\')!=std::string::npos||
                   name.find(':')!=std::string::npos||name.find('\0')!=std::string::npos)
                    fail("Package contains an unsafe filename.");
                for(unsigned char c:name) if(c<32||c==127) fail("Package filename contains control characters.");
                std::filesystem::path p=std::filesystem::u8path(name);
                for(const auto& part:p) if(part==".."||part==".") fail("Package paths must stay inside the archive.");
                if(!names.insert(lower(name)).second) fail("Package contains duplicate filenames.");
                if(((s.m_external_attr>>16)&0170000)==0120000) fail("Symbolic links are not supported in mods.");
                total+=s.m_uncomp_size;
                if(total>kExpandedLimit||s.m_uncomp_size>kPackageLimit) fail("Package expands beyond the supported memory limit.");
                entries_.push_back(name);
            }
        } catch(...) { mz_zip_reader_end(&zip_); throw; }
    }
    ~Archive(){mz_zip_reader_end(&zip_);}
    Archive(const Archive&)=delete;
    const std::vector<std::string>& entries() const {return entries_;}
    bool has(const char* name) const {return mz_zip_reader_locate_file(const_cast<mz_zip_archive*>(&zip_),name,nullptr,MZ_ZIP_FLAG_CASE_SENSITIVE)>=0;}
    std::vector<std::uint8_t> read(const std::string& name,std::size_t limit) {
        const int index=mz_zip_reader_locate_file(&zip_,name.c_str(),nullptr,MZ_ZIP_FLAG_CASE_SENSITIVE);
        mz_zip_archive_file_stat s{};
        if(index<0||!mz_zip_reader_file_stat(&zip_,index,&s)||s.m_uncomp_size>limit) fail("Missing or oversized file: "+name);
        std::vector<std::uint8_t> out(static_cast<std::size_t>(s.m_uncomp_size));
        if(!mz_zip_reader_extract_to_mem(&zip_,index,out.data(),out.size(),0)) fail("Damaged package file: "+name);
        return out;
    }
    Json json(const char* name) {
        auto bytes=read(name,kMetadataLimit);
        auto j=Json::parse(bytes,nullptr,false);
        if(j.is_discarded()||!j.is_object()) fail(std::string("Invalid JSON object: ")+name);
        return j;
    }
private:
    mz_zip_archive zip_{};
    std::vector<std::string> entries_;
};
void write_bytes(const std::filesystem::path& path,std::span<const std::uint8_t> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path,std::ios::binary|std::ios::trunc);
    if(!f) fail("Could not write "+path.filename().string());
    f.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    f.flush();
    if(!f) fail("Not enough space to write "+path.filename().string());
}
std::string stamp() {
    return std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
}
Json blank_profile(const std::string& id,const std::string& name) {
    return Json{{"id",id},{"name",name},{"mods",Json::array()}};
}
void check_option(const Json& o,const Json& value) {
    const auto type=o.at("type").get<std::string>();
    if(type=="Number") {
        if(!value.is_number()) fail("Expected a number for "+o.at("name").get<std::string>());
        const auto n=value.get<double>();
        if(!std::isfinite(n)||n<o.value("min",0.0)||n>o.value("max",0.0)) fail("Setting is outside its allowed range.");
    } else if(type=="Enum") {
        if(!value.is_string()||!o.contains("options")) fail("Expected an option name.");
        const auto& choices=o.at("options");
        if(std::find(choices.begin(),choices.end(),value)==choices.end()) fail("Unknown setting option.");
    } else if(type=="String") {
        if(!value.is_string()||value.get_ref<const std::string&>().size()>4096) fail("Setting text is too long.");
    } else fail("This setting type needs a newer Rocket-R.");
}
}
bool valid_id(const std::string& id) {
    if(id.empty()||id.size()>96||id=="."||id=="..") return false;
    for(unsigned char c:id) if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-')) return false;
    return true;
}
std::vector<std::uint8_t> read_bounded(const std::filesystem::path& path,std::size_t limit) {
    const auto length=std::filesystem::file_size(path);
    if(length>limit) fail("File exceeds the supported size limit.");
    std::vector<std::uint8_t> out(static_cast<std::size_t>(length));
    std::ifstream f(path,std::ios::binary);
    if(!f||!f.read(reinterpret_cast<char*>(out.data()),static_cast<std::streamsize>(out.size()))) fail("Could not read "+path.filename().string());
    return out;
}
Json read_json_file(const std::filesystem::path& path,const Json& fallback) {
    if(!std::filesystem::exists(path)) {
        auto backup=path;backup+=".previous";
        if(std::filesystem::exists(backup)) return read_json_file(backup,fallback);
        return fallback;
    }
    auto bytes=read_bounded(path,4*1024*1024);
    auto value=Json::parse(bytes,nullptr,false);
    if(value.is_discarded()) fail("Could not read settings: "+path.filename().string());
    return value;
}
void write_json_atomic(const std::filesystem::path& path,const Json& value) {
    const auto data=value.dump(2);
    auto temp=path; temp += ".new";
    auto backup=path; backup += ".previous";
    write_bytes(temp,{reinterpret_cast<const std::uint8_t*>(data.data()),data.size()});
    std::error_code ec;
    if(std::filesystem::exists(path)) {
        // This is a replaceable settings backup, never a package/save payload.
        std::filesystem::remove(backup,ec); ec.clear();
        std::filesystem::rename(path,backup);
    }
    try {std::filesystem::rename(temp,path);}
    catch(...) {if(!std::filesystem::exists(path)&&std::filesystem::exists(backup)) std::filesystem::rename(backup,path); throw;}
}
Package inspect_package(std::span<const std::uint8_t> bytes,const std::string& extension) {
    Archive zip(bytes);
    Package p;
    p.hash=sha256(bytes);
    if(extension==".rtz") {
        if(!zip.has("rt64.json")) fail("Texture pack is missing rt64.json.");
        zip.json("rt64.json");
        p.kind="Textures";
        p.manifest=zip.has("mod.json")?zip.json("mod.json"):Json{
            {"id","texture_"+p.hash.substr(0,16)},{"version","1.0.0"},
            {"display_name","Texture pack "+p.hash.substr(0,8)},{"game_id","rocket"},
            {"authors",Json::array({"Unknown author"})},{"minimum_recomp_version","1.0.1"},
            {"description","RT64 replacement texture pack."}};
    } else if(extension==".nrm") {
        p.manifest=zip.json("mod.json");
        p.kind=zip.has("mod_syms.bin")?"Gameplay":"Data";
        if(zip.has("mod_syms.bin")!=zip.has("mod_binary.bin")) fail("Code mod is missing its code or symbols.");
        if(zip.has("mod_syms.bin")) {zip.read("mod_syms.bin",16*1024*1024);zip.read("mod_binary.bin",32*1024*1024);}
    } else fail("Choose an .nrm, .rtz or distribution .zip file.");
    auto& m=p.manifest;
    p.id=m.at("id").get<std::string>(); p.version=m.at("version").get<std::string>();
    if(!valid_id(p.id)) fail("Mod ID must use lowercase letters, numbers, underscores or hyphens.");
    semver(p.version);
    if(m.at("game_id")!="rocket") fail("This mod is for another game.");
    if(!m.contains("display_name")||!m["display_name"].is_string()||m["display_name"].get_ref<const std::string&>().size()>160) fail("Invalid mod name.");
    if(!m.contains("authors")||!m["authors"].is_array()) fail("Mod authors are missing.");
    for(const auto& a:m["authors"]) if(!a.is_string()||a.get_ref<const std::string&>().size()>160) fail("Invalid author name.");
    semver(m.at("minimum_recomp_version").get<std::string>());
    for(const char* field:{"description","short_description"}) if(m.contains(field)&&(!m[field].is_string()||m[field].get_ref<const std::string&>().size()>32768)) fail("Invalid mod description.");
    if(m.contains("native_libraries")&&!m["native_libraries"].empty()) fail("Native libraries are not accepted by this build.");
    for(const auto& name:zip.entries()) {
        const auto ext=lower(std::filesystem::path(name).extension().string());
        if(ext==".dll"||ext==".so"||ext==".exe"||ext==".dylib") fail("Native executable payloads are not supported.");
    }
    if(zip.has("rocket.json")) {
        p.metadata=zip.json("rocket.json");
        if(p.metadata.value("schema",0)!=1) fail("Unsupported Rocket metadata version.");
        if(p.metadata.value("api",1)>1) fail("This mod requires a newer Rocket API.");
        p.kind=p.metadata.value("category",p.kind);
        for(const char* key:{"conflicts","exclusive_resources"}) if(p.metadata.contains(key)) {
            const auto& values=p.metadata.at(key);
            if(!values.is_array()||values.size()>256)fail("Invalid conflict declaration.");
            for(const auto& value:values)if(!value.is_string()||value.get_ref<const std::string&>().size()>160)fail("Invalid conflict name.");
        }
        if(p.metadata.contains("adventure")&&!p.metadata["adventure"].is_boolean())fail("Invalid adventure declaration.");
        if(p.metadata.contains("live_settings")&&!p.metadata["live_settings"].is_boolean())fail("Invalid live settings declaration.");
    }
    if(m.contains("config_schema")) {
        const auto& options=m.at("config_schema").at("options");
        if(!options.is_array()||options.size()>128) fail("Invalid settings schema.");
        std::set<std::string> ids;
        for(const auto& o:options) {
            const auto id=o.at("id").get<std::string>();
            if(!valid_id(id)||!ids.insert(id).second||!o.at("name").is_string()) fail("Invalid or duplicate setting ID.");
            check_option(o,o.at("default"));
        }
    }
    for(const char* key:{"dependencies","optional_dependencies"}) if(m.contains(key)) {
        if(!m[key].is_array()||m[key].size()>128) fail("Invalid dependency list.");
        for(const auto& d:m[key]) {
            auto s=d.get<std::string>();auto sep=s.find(':');
            if(!valid_id(s.substr(0,sep))) fail("Invalid dependency ID.");
            if(sep!=std::string::npos) semver(s.substr(sep+1));
        }
    }
    p.filename=p.hash+extension;
    return p;
}
Library& library(){ static Library instance; return instance; }
void Library::open(const std::filesystem::path& root,const std::string& engine_version) {
    std::lock_guard lock(mutex_); root_=root;version_=engine_version;
    semver(version_);
    std::filesystem::create_directories(root_/"mod-library");
    recovery_=std::filesystem::exists(root_/"mod-session.json");
    const auto initial=Json{{"schema",1},{"selected","original"},{"profiles",Json::array({blank_profile("original","Original Game")})}};
    auto valid_state=[](const Json& state) {
        if(state.value("schema",0)!=1||!state.at("profiles").is_array()||state.at("profiles").size()>64)fail("Invalid profiles.");
        std::set<std::string> ids;
        for(const auto& p:state.at("profiles")) {
            const auto id=p.at("id").get<std::string>();
            if(!valid_id(id)||!ids.insert(id).second||!p.at("name").is_string()||!p.at("mods").is_array()||p.at("mods").size()>256)fail("Invalid profile.");
            if(id=="original"&&!p.at("mods").empty())fail("Original profile must be empty.");
            std::set<std::string> mods;
            for(const auto& m:p.at("mods")) {
                const auto mod=m.at("id").get<std::string>();
                if(!valid_id(mod)||!mods.insert(mod).second||!m.at("hash").is_string()||m.at("hash").get_ref<const std::string&>().size()!=64||!m.at("settings").is_object()||!m.at("enabled").is_boolean())fail("Invalid mod selection.");
            }
        }
        if(!ids.contains("original")||!ids.contains(state.at("selected").get<std::string>()))fail("Selected profile is missing.");
    };
    try {state_=read_json_file(root_/"mod-profiles.json",initial);valid_state(state_);}
    catch(...) {
        recovery_=true;
        try {state_=read_json_file(root_/"mod-profiles.json.previous",initial);valid_state(state_);}
        catch(...) {state_=initial;}
        const auto source=root_/"mod-profiles.json";
        if(std::filesystem::exists(source))std::filesystem::rename(source,root_/("mod-profiles.corrupt-"+stamp()+".json"));
        write_json_atomic(source,state_);
    }
    refresh_locked();
    profile_locked();
}
void Library::migrate_bundled_package(std::span<const std::uint8_t> bytes, const std::string& legacy_hash) {
    std::lock_guard lock(mutex_);
    if (running_) return;
    const auto package = inspect_package(bytes, ".nrm");
    auto updated = state_;
    bool changed = false;
    for (auto& profile : updated["profiles"]) {
        for (auto& entry : profile["mods"]) {
            if (entry.at("id") == package.id && entry.at("hash") == legacy_hash) {
                entry["hash"] = package.hash;
                changed = true;
            }
        }
    }
    if (!changed) return;
    if (semver(version_) < semver(package.manifest.at("minimum_recomp_version")))
        fail("The included mod requires a newer Rocket-R.");
    const auto path = root_ / "mod-library" / package.filename;
    if (!std::filesystem::exists(path)) {
        const auto staged = root_ / "mod-library" / (package.hash + ".partial");
        write_bytes(staged, bytes);
        std::filesystem::rename(staged, path);
    } else if (sha256(read_bounded(path, kPackageLimit)) != package.hash) {
        fail("The installed release camera package is damaged. Reinstall it from Browse.");
    }
    write_json_atomic(root_ / "mod-library" / (package.hash + ".json"), {
        {"id",package.id},{"version",package.version},{"hash",package.hash},{"filename",package.filename},
        {"kind",package.kind},{"manifest",package.manifest},{"metadata",package.metadata}});
    write_json_atomic(root_ / "mod-profiles.json", updated);
    state_ = std::move(updated);
    refresh_locked();
}
Json& Library::profile_locked() {
    const auto id=state_.value("selected",std::string("original"));
    for(auto& p:state_["profiles"]) if(p.value("id",std::string())==id) return p;
    fail("The selected mod profile is missing.");
}
void Library::persist_locked(){write_json_atomic(root_/"mod-profiles.json",state_);}
void Library::refresh_locked() {
    packages_.clear();
    // Index is metadata only. Full package/hash verification is repeated before launch.
    for(const auto& e:std::filesystem::directory_iterator(root_/"mod-library")) {
        if(e.is_symlink()||!e.is_regular_file()||e.path().extension()!=".json") continue;
        try {
            auto j=read_json_file(e.path()); Package p;
            p.id=j.at("id");p.version=j.at("version");p.hash=j.at("hash");p.filename=j.at("filename");
            p.kind=j.at("kind");p.manifest=j.at("manifest");p.metadata=j.value("metadata",Json::object());
            if(!valid_id(p.id)||p.hash.size()!=64||p.filename!=p.hash+std::filesystem::path(p.filename).extension().string()) continue;
            if(p.hash.find_first_not_of("0123456789abcdef")!=std::string::npos)continue;
            const auto ext=std::filesystem::path(p.filename).extension();
            if(ext!=".nrm"&&ext!=".rtz")continue;
            if(p.filename.find_first_of("/\\:")!=std::string::npos) continue;
            p.path=root_/"mod-library"/p.filename;
            if(std::filesystem::is_regular_file(p.path)&&!std::filesystem::is_symlink(p.path)) {
                // Development builds assigned 1.1.0 to packs with no manifest.
                // Correct only that generated metadata after verifying the payload.
                if (ext == ".rtz" && p.manifest.value("minimum_recomp_version", "") == "1.1.0") {
                    const auto bytes = read_bounded(p.path, kPackageLimit);
                    const auto checked = inspect_package(bytes, ".rtz");
                    auto legacy = checked.manifest;
                    legacy["minimum_recomp_version"] = "1.1.0";
                    if (checked.hash == p.hash && !Archive(bytes).has("mod.json") &&
                        checked.id == "texture_" + p.hash.substr(0,16) &&
                        checked.manifest.at("minimum_recomp_version") == "1.0.1" &&
                        p.manifest == legacy && p.metadata == checked.metadata) {
                        p.manifest = checked.manifest;
                        j["manifest"] = p.manifest;
                        write_json_atomic(e.path(), j);
                    }
                }
                packages_.push_back(std::move(p));
            }
        }catch(const std::exception&){}
    }
    std::sort(packages_.begin(),packages_.end(),[](const auto& a,const auto& b){return a.id==b.id?a.version<b.version:a.id<b.id;});
}
const Package* Library::find_locked(const std::string& id,const std::string& hash) const {
    const Package* found=nullptr;
    for(const auto& p:packages_) if(p.id==id&&(hash.empty()||hash==p.hash))
        if(!found||semver(found->version)<semver(p.version)) found=&p;
    return found;
}
Snapshot Library::snapshot() const {
    std::lock_guard lock(mutex_);
    Snapshot s; s.packages=packages_;s.profiles=state_.at("profiles");s.active=active_;s.running=running_;s.recovery=recovery_;
    for(const auto& p:s.profiles) if(p.at("id")==state_.at("selected")) s.profile=p;
    return s;
}
void Library::ensure_editable_locked(){
    if(profile_locked().at("id")=="original") create_profile("My Mods",false);
}
std::vector<std::string> Library::install(const std::filesystem::path& path) {
    auto bytes=read_bounded(path,kPackageLimit);
    return install_bytes(bytes,lower(path.extension().string()));
}
std::vector<std::string> Library::install_bytes(std::span<const std::uint8_t> bytes,const std::string& extension) {
    std::lock_guard lock(mutex_);
    std::vector<std::pair<Package,std::vector<std::uint8_t>>> pending;
    if(extension==".zip") {
        Archive outer(bytes);
        for(const auto& name:outer.entries()) {
            const auto ext=lower(std::filesystem::path(name).extension().string());
            if(ext==".nrm"||ext==".rtz") {
                if(pending.size()>=32) fail("A bundle may contain at most 32 packages.");
                auto payload=outer.read(name,kPackageLimit);
                auto p=inspect_package(payload,ext);
                pending.emplace_back(std::move(p),std::move(payload));
            }
        }
        if(pending.empty()) fail("This ZIP does not contain an .nrm or .rtz mod.");
    } else pending.emplace_back(inspect_package(bytes,extension),std::vector<std::uint8_t>(bytes.begin(),bytes.end()));
    std::set<std::string> ids;
    for(const auto& [p,data]:pending) {
        if(!ids.insert(p.id).second) fail("Bundle contains the same mod ID twice.");
        if(semver(version_)<semver(p.manifest.at("minimum_recomp_version"))) fail(p.manifest.at("display_name").get<std::string>()+" needs a newer Rocket-R.");
    }
    // Publish the profile only after all immutable packages and indexes are present.
    for(auto& [p,data]:pending) {
        p.path=root_/"mod-library"/p.filename;
        if(!std::filesystem::exists(p.path)) {
            auto stage=root_/"mod-library"/(p.hash+".partial");
            write_bytes(stage,data);
            std::filesystem::rename(stage,p.path);
        }
        Json entry={{"id",p.id},{"version",p.version},{"hash",p.hash},{"filename",p.filename},{"kind",p.kind},{"manifest",p.manifest},{"metadata",p.metadata}};
        write_json_atomic(root_/"mod-library"/(p.hash+".json"),entry);
    }
    refresh_locked();ensure_editable_locked();
    std::vector<std::string> installed;
    for(const auto& [p,data]:pending) {
        bool existing=false;
        for(auto& item:profile_locked()["mods"]) if(item.at("id")==p.id) {item["hash"]=p.hash;existing=true;break;}
        if(!existing) profile_locked()["mods"].push_back({{"id",p.id},{"hash",p.hash},{"enabled",true},{"settings",Json::object()}});
        installed.push_back(p.id);
    }
    persist_locked();return installed;
}
void Library::set_enabled(const std::string& id,bool enabled){
    std::lock_guard lock(mutex_);ensure_editable_locked();
    for(auto& item:profile_locked()["mods"]) if(item.at("id")==id) {
        item["enabled"]=enabled;persist_locked();
        if(can_toggle_live(id,item.at("hash"))) {
            live_toggle_handler_(id,enabled);
            for(auto& active:active_.at("packages")) if(active.at("id")==id) active["enabled"]=enabled;
        }
        return;
    }
    const auto* p=find_locked(id);
    if(!p) fail("Mod is not installed.");
    profile_locked()["mods"].push_back({{"id",id},{"hash",p->hash},{"enabled",enabled},{"settings",Json::object()}});
    persist_locked();
}
void Library::select_version(const std::string& id,const std::string& hash) {
    std::lock_guard lock(mutex_);ensure_editable_locked();
    if(!find_locked(id,hash)) fail("This package version is not installed.");
    for(auto& item:profile_locked()["mods"]) if(item.at("id")==id) {item["hash"]=hash;persist_locked();return;}
    fail("Enable the mod in this profile first.");
}
void Library::set_option(const std::string& id,const std::string& key,const Json& value) {
    std::lock_guard lock(mutex_);ensure_editable_locked();
    for(auto& item:profile_locked()["mods"]) if(item.at("id")==id) {
        const auto* p=find_locked(id,item.at("hash"));
        if(!p) fail("Package is missing.");
        for(const auto& o:p->manifest.at("config_schema").at("options")) if(o.at("id")==key) {
            check_option(o,value); item["settings"][key]=value;persist_locked();
            // A different profile/version is a next-launch selection. Never
            // send its settings into the mod code that is currently running.
            if(running_ && live_option_handler_ &&
               active_.at("profile")==state_.at("selected")) {
                for(auto& active:active_.at("packages")) if(active.at("id")==id &&
                    active.at("hash")==p->hash && p->path.extension()==".nrm" &&
                    (item.at("enabled").get<bool>() || active.value("live_toggle",false))) {
                    OptionUpdate update{id,key,{}};
                    const auto type=o.at("type").get<std::string>();
                    if(type=="Enum") {
                        const auto& choices=o.at("options");
                        update.value=static_cast<std::uint32_t>(std::find(choices.begin(),choices.end(),value)-choices.begin());
                    } else if(type=="Number") update.value=value.get<double>();
                    else update.value=value.get<std::string>();
                    live_option_handler_(update);
                    active["settings"][key]=value;
                    break;
                }
            }
            return;
        }
        fail("Unknown mod setting.");
    }
    fail("Enable the mod before changing its settings.");
}
void Library::set_active_enabled(const std::string& id,bool enabled) {
    std::lock_guard lock(mutex_);
    if (!running_ || !live_toggle_handler_) fail("No live mod session is ready.");
    for (auto& active : active_.at("packages")) if (active.at("id") == id) {
        if (!active.value("live_toggle",false)) fail("This running package requires a restart.");
        live_toggle_handler_(id,enabled);
        active["enabled"] = enabled;
        // A next-launch profile/version selection must not redirect a switch
        // meant for the currently running game. Save only the matching entry.
        for (auto& profile : state_.at("profiles")) if (profile.at("id") == active_.at("profile"))
            for (auto& entry : profile.at("mods"))
                if (entry.at("id") == id && entry.at("hash") == active.at("hash")) entry["enabled"] = enabled;
        persist_locked();
        return;
    }
    fail("This mod is not loaded in the current game.");
}
void Library::set_live_option_handler(std::function<void(const OptionUpdate&)> handler) {
    std::lock_guard lock(mutex_);live_option_handler_=std::move(handler);
}
void Library::allow_live_toggle(const std::string& id,const std::string& hash) {
    std::lock_guard lock(mutex_);
    if(running_) fail("Live toggle packages must be registered before launch.");
    live_toggle_packages_[id]=hash;
}
void Library::set_live_toggle_handler(std::function<void(const std::string&,bool)> handler) {
    std::lock_guard lock(mutex_);live_toggle_handler_=std::move(handler);
}
bool Library::can_toggle_live(const std::string& id,const std::string& hash) const {
    std::lock_guard lock(mutex_);
    if(!running_ || !live_toggle_handler_ || active_.at("profile")!=state_.at("selected")) return false;
    for(const auto& active:active_.at("packages")) if(active.at("id")==id && active.at("hash")==hash)
        return active.value("live_toggle",false);
    return false;
}
void Library::move(const std::string& id,int direction) {
    std::lock_guard lock(mutex_);auto& mods=profile_locked()["mods"];
    for(std::size_t i=0;i<mods.size();++i) if(mods[i].at("id")==id) {
        const auto target=static_cast<long long>(i)+(direction<0?-1:1);
        if(target>=0&&target<static_cast<long long>(mods.size())) std::swap(mods[i],mods[static_cast<std::size_t>(target)]);
        persist_locked();return;
    }
}
void Library::create_profile(const std::string& name,bool duplicate) {
    std::lock_guard lock(mutex_);
    if(name.empty()||name.size()>96) fail("Choose a profile name with 1–96 characters.");
    if(state_["profiles"].size()>=64) fail("There are already 64 profiles.");
    const auto id="profile_"+stamp();
    Json p=duplicate?profile_locked():blank_profile(id,name);
    p["id"]=id;p["name"]=name;
    state_["profiles"].push_back(p);state_["selected"]=id;persist_locked();
}
void Library::select_profile(const std::string& id) {
    std::lock_guard lock(mutex_);
    for(const auto& p:state_["profiles"]) if(p.at("id")==id) {state_["selected"]=id;persist_locked();return;}
    fail("Profile not found.");
}
void Library::export_profile(const std::filesystem::path& path) const {
    auto s=snapshot();write_json_atomic(path,{{"schema",1},{"game","rocket"},{"profile",s.profile}});
}
void Library::import_profile(const std::filesystem::path& path) {
    std::lock_guard lock(mutex_);auto j=read_json_file(path);
    if(j.value("schema",0)!=1||j.value("game",std::string())!="rocket") fail("This is not a Rocket mod profile.");
    auto p=j.at("profile");
    if(!p.at("mods").is_array()||p["mods"].size()>256) fail("Invalid profile mod list.");
    std::set<std::string> ids;
    for(const auto& m:p["mods"]) {
        if(!valid_id(m.at("id"))||!ids.insert(m.at("id")).second||!m.at("enabled").is_boolean()||
           !m.at("settings").is_object()||m.at("hash").get<std::string>().size()!=64) fail("Invalid profile entry.");
    }
    auto name=p.at("name").get<std::string>();
    if(name.empty()||name.size()>96||state_["profiles"].size()>=64) fail("Invalid profile name or profile limit reached.");
    p["id"]="profile_"+stamp();
    state_["profiles"].push_back(p);state_["selected"]=p["id"];persist_locked();
}
Resolution Library::resolve_locked(const Json& profile) const {
    Resolution out;
    if(profile.at("id")=="original") return out;
    std::map<std::string,Package> selected;
    std::map<std::string,int> visit;
    std::function<void(const std::string&,const std::string&,const std::string&)> add;
    add=[&](const std::string& id,const std::string& hash,const std::string& required) {
        if(visit[id]==1){out.errors.push_back("Dependency cycle involving "+id);return;}
        const auto* p=find_locked(id,hash);
        if(!p) {out.errors.push_back("Install the required package: "+id);return;}
        if(!required.empty()&&semver(p->version)<semver(required)){out.errors.push_back(id+" needs version "+required+" or later.");return;}
        if(visit[id]==2) {
            if(selected[id].hash!=p->hash) out.errors.push_back("Two versions of "+id+" were selected.");
            return;
        }
        visit[id]=1;
        if(semver(version_)<semver(p->manifest.at("minimum_recomp_version"))) out.errors.push_back(id+" requires a newer Rocket-R.");
        for(const auto& dep:p->manifest.value("dependencies",Json::array())) {
            auto d=dep.get<std::string>();auto split=d.find(':');auto dep_id=d.substr(0,split);
            std::string dep_hash;
            for(const auto& entry:profile.at("mods")) if(entry.at("id")==dep_id) {
                if(!entry.at("enabled").get<bool>()) out.errors.push_back("Enable "+dep_id+" to use "+id+".");
                dep_hash=entry.at("hash");break;
            }
            add(dep_id,dep_hash,split==std::string::npos?"":d.substr(split+1));
        }
        visit[id]=2;selected[id]=*p;out.packages.push_back(*p);
    };
    try {
        for(const auto& entry:profile.at("mods")) if(entry.at("enabled").get<bool>()) add(entry.at("id"),entry.at("hash"),"");
        std::map<std::string,std::string> owners;
        std::string adventure;
        for(const auto& p:out.packages) {
            for(const auto& id:p.metadata.value("conflicts",Json::array()))
                if(selected.contains(id.get<std::string>())) out.errors.push_back(p.id+" conflicts with "+id.get<std::string>());
            for(const auto& resource:p.metadata.value("exclusive_resources",Json::array())) {
                auto key=resource.get<std::string>();
                if(owners.contains(key)) out.errors.push_back(p.id+" and "+owners[key]+" both replace "+key);
                owners[key]=p.id;
            }
            if(p.metadata.value("adventure",false)) {
                if(!adventure.empty()) out.errors.push_back("Choose one adventure: "+adventure+" or "+p.id);
                adventure=p.id;
            }
        }
    } catch(const std::exception& e){out.errors.push_back(std::string("Invalid profile or package metadata: ")+e.what());}
    return out;
}
Resolution Library::resolve() const {
    std::lock_guard lock(mutex_);
    for(const auto& p:state_.at("profiles")) if(p.at("id")==state_.at("selected")) return resolve_locked(p);
    return {{},{"Selected profile is missing."}};
}
std::filesystem::path Library::prepare_launch(bool without_mods) {
    std::lock_guard lock(mutex_);
    if(running_) fail("Restart Rocket-R to apply package changes.");
    live_option_handler_={};
    live_toggle_handler_={};
    const Json profile=without_mods?blank_profile("original","Original Game"):profile_locked();
    auto resolved=resolve_locked(profile);
    if(!resolved) fail(resolved.errors.front());
    // A disabled, host-approved camera package can stay resident without
    // controlling the game. Do not preload arbitrary disabled code, add new
    // dependencies, or occupy a camera resource another enabled mod needs.
    Json load_profile=profile;
    for(auto& entry:load_profile.at("mods")) if(!entry.at("enabled").get<bool>()) {
        const auto approved=live_toggle_packages_.find(entry.at("id").get<std::string>());
        if(approved==live_toggle_packages_.end() || approved->second!=entry.at("hash").get<std::string>()) continue;
        entry["enabled"]=true;
        auto with_standby=resolve_locked(load_profile);
        const bool only_camera_added=with_standby && with_standby.packages.size()==resolved.packages.size()+1;
        if(only_camera_added) resolved=std::move(with_standby);
        else entry["enabled"]=false;
    }
    const auto runtime=root_/"mod-runs"/("run_"+stamp());
    std::filesystem::create_directories(runtime/"mods");
    std::filesystem::create_directories(runtime/"mod_config");
    Json ids=Json::array(),locklist=Json::array();
    for(const auto& p:resolved.packages) {
        auto bytes=read_bounded(p.path,kPackageLimit);
        auto verified=inspect_package(bytes,p.path.extension().string());
        if(verified.hash!=p.hash||verified.id!=p.id||verified.manifest!=p.manifest||verified.metadata!=p.metadata) fail("Package changed on disk: "+p.id);
        ids.push_back(p.id);
        if(p.path.extension()==".nrm") {
            const auto dest=runtime/"mods"/(p.id+".nrm");
            std::error_code ec;std::filesystem::create_hard_link(p.path,dest,ec);
            if(ec) std::filesystem::copy_file(p.path,dest);
        }
        Json settings=Json::object();
        if(p.manifest.contains("config_schema"))
            for(const auto& o:p.manifest["config_schema"]["options"]) settings[o.at("id").get<std::string>()]=o.at("default");
        for(const auto& entry:profile.at("mods")) if(entry.at("id")==p.id)
            for(auto it=entry.at("settings").begin();it!=entry.at("settings").end();++it)
                if(p.manifest.contains("config_schema")) for(const auto& o:p.manifest["config_schema"]["options"])
                    if(o.at("id")==it.key()) {check_option(o,it.value());settings[it.key()]=it.value();}
        write_json_atomic(runtime/"mod_config"/(p.id+".json"),{{"mod_id",p.id},{"mod_version",p.version},{"recomp_version",version_},{"storage",settings}});
        bool enabled=true;
        for(const auto& entry:profile.at("mods")) if(entry.at("id")==p.id) enabled=entry.at("enabled").get<bool>();
        const auto approved=live_toggle_packages_.find(p.id);
        bool live_toggle=approved!=live_toggle_packages_.end() && approved->second==p.hash;
        for(const auto& other:resolved.packages) for(const auto& dep:other.manifest.value("dependencies",Json::array())) {
            const auto dependency=dep.get<std::string>();
            if(dependency.substr(0,dependency.find(':'))==p.id) live_toggle=false;
        }
        locklist.push_back({{"id",p.id},{"hash",p.hash},{"version",p.version},{"kind",p.kind},{"path",p.path.generic_string()},
            {"settings",settings},{"enabled",enabled},{"live_toggle",live_toggle}});
    }
    write_json_atomic(runtime/"mods.json",{{"enabled_mods",ids},{"mod_order",ids}});
    active_={{"profile",profile.at("id")},{"name",profile.at("name")},{"packages",locklist},{"runtime",runtime.generic_string()}};
    write_json_atomic(runtime/"launch.json",active_);
    active_save_=profile.at("id")=="original"?root_/"saves":root_/"mod-saves"/profile.at("id").get<std::string>();
    std::filesystem::create_directories(active_save_);
    write_json_atomic(root_/"mod-session.json",active_);
    running_=true;return runtime;
}
std::filesystem::path Library::active_save_path() const {std::lock_guard lock(mutex_);return active_save_;}
void Library::finish_session() {
    std::lock_guard lock(mutex_);running_=false;
    live_option_handler_={};
    live_toggle_handler_={};
    std::error_code ec;std::filesystem::remove(root_/"mod-session.json",ec);
}
void Library::dismiss_recovery(){std::lock_guard lock(mutex_);recovery_=false;}
}

