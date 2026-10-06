#pragma once
#include <filesystem>
#include <cstdint>
#include <functional>
#include <mutex>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <variant>
#include "json/json.hpp"

namespace rocket::mods {
using Json = nlohmann::json;
struct Package {
    std::string id, version, hash, kind, filename;
    Json manifest = Json::object();
    Json metadata = Json::object();
    std::filesystem::path path;
    bool asset_patch=false;
};
struct Resolution {
    std::vector<Package> packages;
    std::vector<std::string> errors;
    explicit operator bool() const { return errors.empty(); }
};
struct Snapshot {
    std::vector<Package> packages;
    Json profiles;
    Json profile;
    Json active;
    bool running=false, recovery=false;
};
struct OptionUpdate {
    std::string mod_id, option_id;
    std::variant<std::uint32_t, double, std::string> value;
};
bool valid_id(const std::string& id);
std::vector<std::uint8_t> read_bounded(const std::filesystem::path& path, std::size_t limit);
Package inspect_package(std::span<const std::uint8_t> bytes, const std::string& extension);
void write_json_atomic(const std::filesystem::path& path, const Json& value);
Json read_json_file(const std::filesystem::path& path, const Json& fallback=Json::object());

class Library {
public:
    void open(const std::filesystem::path& root, const std::string& engine_version);
    // Replace one explicitly identified bundled development package in saved profiles.
    // Keep settings, enabled state, profile identity and the old immutable package.
    void migrate_bundled_package(std::span<const std::uint8_t> bytes, const std::string& legacy_hash);
    Snapshot snapshot() const;
    // Immutable UI views survive edits, and are rebuilt only when state changes.
    std::shared_ptr<const Snapshot> snapshot_view() const;
    std::vector<std::string> install(const std::filesystem::path& path);
    std::vector<std::string> install_bytes(std::span<const std::uint8_t> bytes, const std::string& extension);
    void set_enabled(const std::string& id, bool enabled);
    void set_active_enabled(const std::string& id, bool enabled);
    void set_option(const std::string& id, const std::string& key, const Json& value);
    void set_action_binding(const std::string& id,const std::string& action,bool keyboard,int source);
    void set_action_touch(const std::string& id,const std::string& action,int mask);
    void set_live_action_handler(std::function<void(const std::string&,const std::string&,const std::string&,int)> handler);
    // Installed only after the runtime has finished loading its mod context.
    void set_live_option_handler(std::function<void(const OptionUpdate&)> handler);
    // Only host-approved package hashes may be loaded in standby and toggled.
    void allow_live_toggle(const std::string& id, const std::string& hash);
    void set_live_toggle_handler(std::function<void(const std::string&, bool)> handler);
    bool can_toggle_live(const std::string& id, const std::string& hash) const;
    void move(const std::string& id, int direction);
    void select_version(const std::string& id, const std::string& hash);
    void create_profile(const std::string& name, bool duplicate);
    void select_profile(const std::string& id);
    void export_profile(const std::filesystem::path& path) const;
    void import_profile(const std::filesystem::path& path);
    Resolution resolve() const;
    std::shared_ptr<const Resolution> resolution_view() const;
    // Creates an immutable launch snapshot; runtime-only files live outside the library.
    std::filesystem::path prepare_launch(bool without_mods=false);
    std::filesystem::path active_save_path() const;
    void finish_session();
    void dismiss_recovery();
    const std::filesystem::path& root() const { return root_; }
private:
    Resolution resolve_locked(const Json& profile) const;
    Json& profile_locked();
    void persist_locked();
    void invalidate_views_locked();
    void refresh_locked();
    void ensure_editable_locked();
    const Package* find_locked(const std::string& id, const std::string& hash="") const;
    mutable std::recursive_mutex mutex_;
    std::filesystem::path root_, active_save_;
    std::string version_;
    Json state_=Json::object(), active_=Json::object();
    std::vector<Package> packages_;
    mutable std::shared_ptr<const Snapshot> snapshot_cache_;
    mutable std::shared_ptr<const Resolution> resolution_cache_;
    bool running_=false, recovery_=false;
    std::function<void(const OptionUpdate&)> live_option_handler_;
    std::function<void(const std::string&,const std::string&,const std::string&,int)> live_action_handler_;
    void compose_assets_locked(const std::vector<Package>& packages) const;
    mutable std::string asset_cache_key_,asset_cache_error_;
    mutable std::vector<std::uint8_t> asset_cache_;
    std::function<void(const std::string&, bool)> live_toggle_handler_;
    std::map<std::string,std::string> live_toggle_packages_;
};
Library& library();
}
