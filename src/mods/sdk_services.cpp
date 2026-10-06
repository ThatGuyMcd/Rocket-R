#include "sdk_services.hpp"
#include "miniz.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>

namespace rocket::mods::sdk {
namespace {
constexpr std::size_t resource_limit = 16 * 1024 * 1024,
                      resident_limit = 32 * 1024 * 1024;
constexpr std::size_t save_limit = 256 * 1024;
bool resource_path(const std::string &path) {
  if (path.empty() || path.size() > 240 || path.front() == '/' ||
      path.find('\\') != std::string::npos ||
      path.find(':') != std::string::npos)
    return false;
  for (unsigned char c : path)
    if (c < 32 || c == 127)
      return false;
  for (const auto &part : std::filesystem::u8path(path))
    if (part == "." || part == "..")
      return false;
  return true;
}
bool keyboard_source(int source) {
  return source >= -1 && (source < 512 || (source >= 2001 && source <= 2005) ||
                          (source >= 2100 && source <= 2103) ||
                          (source >= 2200 && source <= 2203));
}
bool controller_source(int source) {
  return source >= -1 && (source <= 20 || (source >= 1000 && source <= 1011));
}
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
} // namespace
unsigned module_version(const std::string &name) {
  if (name == "core" || name == "lifecycle" || name == "resources" ||
      name == "saves" || name == "native_objects" || name == "diagnostics" ||
      name == "input" || name == "events" || name == "systems" ||
      name == "hud" || name == "native_audio")
    return 1;
  if (name == "meshes" || name == "actors" || name == "custom_scenes" ||
      name == "custom_collision" || name == "custom_audio" ||
      name == "native_render" || name == "commands" || name == "asset_layers")
    return 1;
  return 0;
}
void validate_metadata(const Json &metadata) {
  const auto api = metadata.value("api", 1);
  require(api >= 1 && api <= 2, "This mod requires an unsupported Rocket API.");
  // Unknown fields in API 1 retain their original semantics.
  if (api == 1)
    return;
  if (metadata.contains("requires")) {
    const auto &required = metadata.at("requires");
    require(required.is_object() && required.size() <= 64,
            "Invalid SDK module requirements.");
    for (auto it = required.begin(); it != required.end(); ++it) {
      require(it.value().is_number_integer(),
              "SDK module versions must be integers.");
      const auto version = it.value().get<int>();
      require(version > 0 &&
                  static_cast<unsigned>(version) <= module_version(it.key()),
              "This mod needs an unavailable SDK module/version.");
    }
  }
  if (metadata.contains("activation")) {
    const auto mode = metadata.at("activation").get<std::string>();
    require(mode == "restart" || mode == "managed",
            "Unknown mod activation mode.");
    if (mode == "managed")
      require(metadata.value("exclusive_resources", Json::array()).empty(),
              "Managed live mods cannot claim exclusive game replacements.");
  }
  if (metadata.contains("resources")) {
    const auto &resources = metadata.at("resources");
    require(resources.is_object() && resources.size() <= 4096,
            "Invalid resource index.");
    for (auto it = resources.begin(); it != resources.end(); ++it) {
      require(resource_path(it.key()) && it.value().is_object(),
              "Invalid resource name.");
      require(resource_path(it.value().at("file").get<std::string>()),
              "Invalid resource filename.");
      require(it.value().value("type", std::string("binary")).size() <= 64,
              "Invalid resource type.");
    }
  }
  if (metadata.contains("input_actions")) {
    const auto &actions = metadata.at("input_actions");
    require(actions.is_object() && actions.size() <= 64,
            "Invalid input action list.");
    for (auto it = actions.begin(); it != actions.end(); ++it) {
      require(valid_id(it.key()) && it.value().is_object(),
              "Invalid input action ID.");
      require(it.value().value("name", it.key()).size() <= 160,
              "Input action name is too long.");
      const int keyboard = it.value().value("keyboard", -1),
                controller = it.value().value("controller", -1),
                n64 = it.value().value("n64", 0);
      require(keyboard_source(keyboard), "Invalid keyboard/mouse input.");
      require(controller_source(controller), "Invalid controller input.");
      require(n64 >= 0 && n64 <= 65535, "Invalid N64 input mask.");
    }
  }
  if (metadata.contains("settings_ui")) {
    const auto &widgets = metadata.at("settings_ui");
    require(widgets.is_object() && widgets.size() <= 128,
            "Invalid settings widgets.");
    for (auto widget = widgets.begin(); widget != widgets.end(); ++widget)
      require(valid_id(widget.key()) && widget.value() == "toggle",
              "Unknown settings widget.");
  }
  if (metadata.contains("systems")) {
    const auto &systems = metadata.at("systems");
    require(systems.is_array() && systems.size() <= 64,
            "Invalid system declarations.");
    for (const auto &system : systems)
      require(system.is_string() && resource_path(system.get<std::string>()) &&
                  system.get_ref<const std::string &>().size() < 96,
              "Invalid system name.");
  }
  if (metadata.contains("commands")) {
    const auto &commands = metadata.at("commands");
    require(commands.is_array() && commands.size() <= 32,
            "Invalid mod commands.");
    std::set<std::string> ids;
    for (const auto &command : commands)
      require(command.is_object() &&
                  valid_id(command.at("id").get<std::string>()) &&
                  ids.insert(command.at("id").get<std::string>()).second &&
                  command.at("name").is_string() &&
                  command.at("name").get_ref<const std::string &>().size() <=
                      160,
              "Invalid or duplicate mod command.");
  }
}
void validate_bindings(const Package &package, const Json &bindings) {
  if (package.metadata.value("api", 1) != 2)
    return;
  require(bindings.is_object() && bindings.size() <= 64,
          "Invalid mod bindings.");
  const auto actions = package.metadata.value("input_actions", Json::object());
  for (auto binding = bindings.begin(); binding != bindings.end(); ++binding) {
    require(actions.contains(binding.key()) && binding.value().is_object(),
            "Binding references an unknown action.");
    for (auto source = binding.value().begin(); source != binding.value().end();
         ++source) {
      require(source.value().is_number_integer(),
              "Mod binding must be an integer.");
      const int value = source.value().get<int>();
      require((source.key() == "keyboard" && keyboard_source(value)) ||
                  (source.key() == "controller" && controller_source(value)) ||
                  (source.key() == "n64" && value >= 0 && value <= 65535),
              "Invalid mod binding source.");
    }
  }
}
bool managed_activation(const Package &package) {
  return package.metadata.value("api", 1) == 2 &&
         package.metadata.value("activation", std::string()) == "managed";
}
void validate_asset_patch(std::span<const std::uint8_t> data) {
  require(data.size() >= 16 && std::memcmp(data.data(), "BPS1", 4) == 0,
          "Invalid BPS asset patch.");
  std::size_t cursor = 4;
  const auto end = data.size() - 12;
  auto read = [&]() {
    std::uint64_t value = 0, shift = 1;
    for (unsigned i = 0; i < 5; ++i) {
      require(cursor < end, "Truncated BPS number.");
      const auto byte = data[cursor++];
      value += (byte & 127) * shift;
      require(value <= UINT32_MAX, "BPS number is too large.");
      if (byte & 128)
        return value;
      shift <<= 7;
      value += shift;
    }
    throw std::runtime_error("Invalid BPS number.");
  };
  const auto source_size = read(), target_size = read(), metadata = read();
  require(source_size == 0xC00000 && target_size == source_size,
          "Asset patches must keep the US cartridge size.");
  require(metadata <= end - cursor, "Invalid BPS metadata size.");
  cursor += static_cast<std::size_t>(metadata);
  std::uint64_t target = 0;
  std::int64_t source_copy = 0, target_copy = 0;
  while (cursor < end) {
    const auto action = read(), mode = action & 3, length = (action >> 2) + 1;
    require(length <= target_size - target,
            "BPS write is outside the cartridge.");
    if (mode == 1) {
      require(target >= 0xB0460,
              "Asset patches cannot change game code or the cartridge header.");
      require(length <= end - cursor, "Truncated BPS literal.");
      cursor += static_cast<std::size_t>(length);
    } else if (mode >= 2) {
      const auto offset = read();
      require((offset >> 1) <= INT64_MAX, "Invalid BPS copy offset.");
      auto &relative = mode == 2 ? source_copy : target_copy;
      const auto distance = static_cast<std::int64_t>(offset >> 1);
      require(relative <= INT64_MAX - distance, "BPS offset overflow.");
      relative += (offset & 1) ? -distance : distance;
      require(relative >= 0, "Negative BPS copy offset.");
      if (mode == 2) {
        require(static_cast<std::uint64_t>(relative) <= source_size - length,
                "BPS source copy is outside the cartridge.");
        require(target >= 0xB0460 ||
                    static_cast<std::uint64_t>(relative) == target,
                "Asset patch copies different game code.");
      } else {
        require(target >= 0xB0460 &&
                    static_cast<std::uint64_t>(relative) < target,
                "Invalid BPS target copy.");
      }
      relative += static_cast<std::int64_t>(length);
    }
    target += length;
  }
  require(target == target_size && cursor == end, "BPS target is incomplete.");
  const auto crc = static_cast<std::uint32_t>(data[data.size() - 4]) |
                   (static_cast<std::uint32_t>(data[data.size() - 3]) << 8) |
                   (static_cast<std::uint32_t>(data[data.size() - 2]) << 16) |
                   (static_cast<std::uint32_t>(data[data.size() - 1]) << 24);
  require(mz_crc32(0, data.data(), data.size() - 4) == crc,
          "BPS checksum failed.");
}
std::vector<std::uint8_t> read_package_entry(const std::filesystem::path &path,
                                             const std::string &name,
                                             std::size_t limit) {
  require(resource_path(name), "Unsafe resource path.");
  mz_zip_archive archive{};
#if defined(_WIN32)
  FILE *file = nullptr;
  _wfopen_s(&file, path.c_str(), L"rb");
#else
  FILE *file = std::fopen(path.c_str(), "rb");
#endif
  require(file != nullptr, "Cannot open resource package.");
  struct FileClose {
    FILE *file;
    ~FileClose() { std::fclose(file); }
  } file_close{file};
  require(mz_zip_reader_init_cfile(&archive, file, 0, 0) != 0,
          "Cannot read resource package.");
  struct Close {
    mz_zip_archive *zip;
    ~Close() { mz_zip_reader_end(zip); }
  } close{&archive};
  const int index = mz_zip_reader_locate_file(&archive, name.c_str(), nullptr,
                                              MZ_ZIP_FLAG_CASE_SENSITIVE);
  mz_zip_archive_file_stat stat{};
  require(index >= 0 && mz_zip_reader_file_stat(&archive, index, &stat),
          "Resource file is missing.");
  require(!stat.m_is_directory && stat.m_uncomp_size <= limit,
          "Resource exceeds its memory budget.");
  std::vector<std::uint8_t> data(static_cast<std::size_t>(stat.m_uncomp_size));
  require(mz_zip_reader_extract_to_mem(&archive, index, data.data(),
                                       data.size(), 0) != 0,
          "Resource is damaged.");
  return data;
}
void Services::begin(const std::vector<Package> &packages,
                     const std::filesystem::path &save_root) {
  std::lock_guard lock(mutex_);
  packages_.clear();
  open_.clear();
  bytes_ = 0;
  save_root_ = save_root;
  for (const auto &package : packages)
    packages_.emplace(package.id, package);
  // Handles never reset: a stale previous-session handle cannot alias a new
  // one.
}
std::uint32_t Services::open(const std::string &owner,
                             const std::string &name) {
  std::lock_guard lock(mutex_);
  const auto package = packages_.find(owner);
  require(package != packages_.end(), "Mod is not in the running session.");
  const auto index =
      package->second.metadata.value("resources", Json::object());
  require(index.contains(name), "Unknown resource.");
  require(open_.size() < 256 && next_handle_ != 0,
          "Resource handle limit reached.");
  auto data = read_package_entry(package->second.path,
                                 index.at(name).at("file"), resource_limit);
  require(data.size() <= resident_limit - bytes_,
          "Resource pool exceeds 32 MiB.");
  const auto handle = next_handle_++;
  bytes_ += data.size();
  open_.emplace(handle, OpenResource{owner, std::move(data)});
  return handle;
}
int Services::size(const std::string &owner, std::uint32_t handle) const {
  std::lock_guard lock(mutex_);
  const auto it = open_.find(handle);
  return it != open_.end() && it->second.owner == owner
             ? static_cast<int>(it->second.bytes.size())
             : -2;
}
std::vector<std::uint8_t> Services::read(const std::string &owner,
                                         std::uint32_t handle,
                                         std::uint32_t offset,
                                         std::uint32_t capacity) const {
  std::lock_guard lock(mutex_);
  const auto it = open_.find(handle);
  require(it != open_.end() && it->second.owner == owner,
          "Invalid resource handle or owner.");
  const auto &data = it->second.bytes;
  require(offset <= data.size(), "Resource offset is outside the file.");
  const auto length = std::min<std::size_t>(capacity, data.size() - offset);
  return {data.begin() + offset, data.begin() + offset + length};
}
bool Services::close(const std::string &owner, std::uint32_t handle) {
  std::lock_guard lock(mutex_);
  const auto it = open_.find(handle);
  if (it == open_.end() || it->second.owner != owner)
    return false;
  bytes_ -= it->second.bytes.size();
  open_.erase(it);
  return true;
}
void Services::release(const std::string &owner) {
  std::lock_guard lock(mutex_);
  for (auto it = open_.begin(); it != open_.end();)
    if (it->second.owner == owner) {
      bytes_ -= it->second.bytes.size();
      it = open_.erase(it);
    } else
      ++it;
}
void Services::save(const std::string &owner, const std::string &name,
                    std::uint32_t schema, std::span<const std::uint8_t> bytes) {
  std::lock_guard lock(mutex_);
  require(packages_.contains(owner) && valid_id(owner) && valid_id(name),
          "Invalid save namespace.");
  require(schema > 0 && bytes.size() <= save_limit,
          "Save schema or size is invalid.");
  std::string hex;
  hex.reserve(bytes.size() * 2);
  constexpr char digits[] = "0123456789abcdef";
  for (auto byte : bytes) {
    hex.push_back(digits[byte >> 4]);
    hex.push_back(digits[byte & 15]);
  }
  write_json_atomic(save_root_ / "mods" / owner / (name + ".json"),
                    {{"schema", schema}, {"data", hex}});
}
std::pair<std::uint32_t, std::vector<std::uint8_t>>
Services::load(const std::string &owner, const std::string &name) const {
  std::lock_guard lock(mutex_);
  require(packages_.contains(owner) && valid_id(owner) && valid_id(name),
          "Invalid save namespace.");
  const auto json =
      read_json_file(save_root_ / "mods" / owner / (name + ".json"));
  require(json.contains("schema") && json.contains("data"),
          "Save does not exist.");
  const auto schema = json.at("schema").get<std::uint32_t>();
  const auto hex = json.at("data").get<std::string>();
  require(schema > 0 && hex.size() <= save_limit * 2 && hex.size() % 2 == 0,
          "Invalid mod save.");
  std::vector<std::uint8_t> bytes;
  bytes.reserve(hex.size() / 2);
  auto nibble = [](char c) -> unsigned {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    throw std::runtime_error("Invalid save encoding.");
  };
  for (std::size_t i = 0; i < hex.size(); i += 2)
    bytes.push_back(
        static_cast<std::uint8_t>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
  return {schema, std::move(bytes)};
}
std::size_t Services::resident_bytes() const {
  std::lock_guard lock(mutex_);
  return bytes_;
}
} // namespace rocket::mods::sdk
