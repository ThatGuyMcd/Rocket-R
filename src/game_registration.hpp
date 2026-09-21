#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace rocket {

inline constexpr char8_t kGameId[] = u8"rocket.nsue.us";
inline constexpr std::uint32_t kAudioUcodeVram = 0x80001560U;
inline constexpr std::uint32_t kRetailEntrypoint = 0x80000400U;

void register_generated_sections();
bool register_game(const std::filesystem::path& config_directory, std::string& error);
bool select_rom(const std::filesystem::path& rom_path, std::string& error);
bool rom_ready();
bool start_game_once();

} // namespace rocket
