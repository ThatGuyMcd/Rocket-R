#pragma once
#include "sdk_world.hpp"
namespace rocket::mods::sdk {
// Called after native render-entry construction and before the existing sort.
// Uses the expanded render-entry queue and its reserved two-bank data gap.
// The native frame arena is retained for sorting/draw commands and the HUD.
unsigned render_world(const World &world, std::uint8_t *rdram);
} // namespace rocket::mods::sdk
