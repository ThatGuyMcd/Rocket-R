#include "asset_layers.hpp"
#include "miniz.h"
#include "sdk_services.hpp"
#include <algorithm>
#include <stdexcept>

namespace rocket::mods::sdk {
namespace {
constexpr std::uint32_t rom_size = 0xC00000;
void require(bool value, const std::string &message) {
  if (!value)
    throw std::runtime_error(message);
}
std::uint32_t read_little(std::span<const std::uint8_t> data, std::size_t p) {
  return data[p] | (std::uint32_t(data[p + 1]) << 8) |
         (std::uint32_t(data[p + 2]) << 16) |
         (std::uint32_t(data[p + 3]) << 24);
}
void write_little(std::vector<std::uint8_t> &data, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    data.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}
void number(std::vector<std::uint8_t> &data, std::uint32_t value) {
  for (;;) {
    auto byte = static_cast<std::uint8_t>(value & 127);
    value >>= 7;
    if (!value) {
      data.push_back(byte | 128);
      return;
    }
    data.push_back(byte);
    --value;
  }
}
struct Change {
  std::uint32_t offset;
  std::vector<std::uint8_t> data;
  std::string owner;
};
} // namespace
std::vector<std::uint8_t>
compose_asset_layers(std::span<const AssetLayer> layers) {
  require(layers.size() >= 2 && layers.size() <= 64,
          "Asset composition needs 2–64 layers.");
  std::vector<Change> changes;
  std::uint32_t source_crc = 0, target_crc = 0;
  bool first = true;
  for (const auto &layer : layers) {
    const auto &data = layer.patch;
    validate_asset_patch(data);
    const auto crc = read_little(data, data.size() - 12),
               target = read_little(data, data.size() - 8);
    if (first) {
      source_crc = target_crc = crc;
      first = false;
    }
    require(crc == source_crc,
            "Asset layers need the same original cartridge: " + layer.owner);
    // Equal-length CRC32 differences compose by XOR when changed ranges
    // do not overlap. The game loader still verifies the resulting target.
    target_crc ^= crc ^ target;
    std::size_t cursor = 4, end = data.size() - 12;
    auto read = [&]() {
      std::uint32_t value = 0, shift = 1;
      for (;;) {
        const auto byte = data.at(cursor++);
        value += (byte & 127) * shift;
        if (byte & 128)
          return value;
        shift <<= 7;
        value += shift;
      }
    };
    read();
    read();
    const auto metadata = read();
    cursor += metadata;
    std::uint32_t output = 0;
    while (cursor < end) {
      const auto action = read(), mode = action & 3, length = (action >> 2) + 1;
      require(mode == 0 || mode == 1,
              "To combine " + layer.owner +
                  ", regenerate its patch with rocket_sdk.py asset-patch. BPS "
                  "copy operations need a single package.");
      if (mode == 1) {
        require(changes.size() < 16384, "Too many asset changes.");
        const auto next =
            std::lower_bound(changes.begin(), changes.end(), output,
                             [](const Change &c, std::uint32_t offset) {
                               return c.offset < offset;
                             });
        if (next != changes.end())
          require(output + length <= next->offset,
                  "Asset changes overlap: " + layer.owner + " and " +
                      next->owner);
        if (next != changes.begin()) {
          const auto &previous = *(next - 1);
          require(previous.offset + previous.data.size() <= output,
                  "Asset changes overlap: " + layer.owner + " and " +
                      previous.owner);
        }
        changes.insert(next, Change{output,
                                    {data.begin() + cursor,
                                     data.begin() + cursor + length},
                                    layer.owner});
        cursor += length;
      }
      output += length;
    }
  }
  std::vector<std::uint8_t> patch{'B', 'P', 'S', '1'};
  number(patch, rom_size);
  number(patch, rom_size);
  number(patch, 0);
  std::uint32_t cursor = 0;
  for (const auto &change : changes) {
    if (change.offset > cursor)
      number(patch, ((change.offset - cursor - 1) << 2));
    number(patch,
           ((static_cast<std::uint32_t>(change.data.size()) - 1) << 2) | 1);
    patch.insert(patch.end(), change.data.begin(), change.data.end());
    cursor = change.offset + static_cast<std::uint32_t>(change.data.size());
  }
  if (cursor < rom_size)
    number(patch, ((rom_size - cursor - 1) << 2));
  write_little(patch, source_crc);
  write_little(patch, target_crc);
  write_little(patch,
         static_cast<std::uint32_t>(mz_crc32(0, patch.data(), patch.size())));
  validate_asset_patch(patch);
  return patch;
}
} // namespace rocket::mods::sdk
