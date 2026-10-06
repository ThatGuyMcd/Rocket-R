#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace rocket::mods::sdk {
struct AssetLayer {
  std::string owner;
  std::vector<std::uint8_t> patch;
};
// Merge non-overlapping SDK patches against their shared original cartridge.
std::vector<std::uint8_t>
compose_asset_layers(std::span<const AssetLayer> layers);
} // namespace rocket::mods::sdk
