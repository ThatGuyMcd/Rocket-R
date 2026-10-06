#pragma once
#include "mod_library.hpp"
#include <map>
#include <mutex>

namespace rocket::mods::sdk {
// Only modules with a working host implementation are advertised here.
unsigned module_version(const std::string &name);
void validate_metadata(const Json &metadata);
void validate_bindings(const Package &package, const Json &bindings);
void validate_asset_patch(std::span<const std::uint8_t> bytes);
bool managed_activation(const Package &package);
std::vector<std::uint8_t> read_package_entry(const std::filesystem::path &path,
                                             const std::string &name,
                                             std::size_t limit);

class Services {
public:
  void begin(const std::vector<Package> &packages,
             const std::filesystem::path &save_root);
  std::uint32_t open(const std::string &owner, const std::string &name);
  int size(const std::string &owner, std::uint32_t handle) const;
  std::vector<std::uint8_t> read(const std::string &owner, std::uint32_t handle,
                                 std::uint32_t offset,
                                 std::uint32_t capacity) const;
  bool close(const std::string &owner, std::uint32_t handle);
  void release(const std::string &owner);
  void save(const std::string &owner, const std::string &name,
            std::uint32_t schema, std::span<const std::uint8_t> bytes);
  std::pair<std::uint32_t, std::vector<std::uint8_t>>
  load(const std::string &owner, const std::string &name) const;
  std::size_t resident_bytes() const;

private:
  struct OpenResource {
    std::string owner;
    std::vector<std::uint8_t> bytes;
  };
  std::map<std::string, Package> packages_;
  std::map<std::uint32_t, OpenResource> open_;
  std::filesystem::path save_root_;
  std::uint32_t next_handle_ = 1;
  std::size_t bytes_ = 0;
  mutable std::recursive_mutex mutex_;
};
} // namespace rocket::mods::sdk
