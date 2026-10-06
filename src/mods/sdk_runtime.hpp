#pragma once
#include "mod_library.hpp"
#include "recomp.h"
#include "rocket/sdk_types.h"
namespace rocket::mods::sdk {
void register_exports();
void prepare();
void ready(std::uint8_t *rdram);
void request_enabled(const std::string &owner, bool enabled);
void settings_changed(const std::string &owner);
bool request_command(const std::string &owner, const std::string &command);
struct Status {
  bool registered = false, enabled = false, requested = false;
  std::uint64_t ticks = 0;
};
Status status(const std::string &owner);
std::vector<RocketHudItem> hud_snapshot();
} // namespace rocket::mods::sdk
extern "C" void rocket_sdk_tick(std::uint8_t *rdram, recomp_context *ctx);
extern "C" void rocket_sdk_render(std::uint8_t *rdram, recomp_context *ctx);
