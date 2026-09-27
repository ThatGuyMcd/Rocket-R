#pragma once
#include <filesystem>
#include "recomp.h"
namespace rocket::mods {
void register_api();
void configure_library();
void request_camera_enabled(bool enabled);
struct CameraStatus { bool loaded, enabled, requested; };
CameraStatus camera_status();
void game_ready(std::uint8_t* rdram, recomp_context* ctx);
void prepare_runtime(bool without_mods = false);
}
extern "C" void rocket_mod_camera_update(std::uint8_t* rdram, recomp_context* ctx);
extern "C" void rocket_mod_camera_tick(std::uint8_t* rdram, recomp_context* ctx);
extern "C" void rocket_mod_camera_smoothing_begin(std::uint8_t* rdram, recomp_context* ctx);
extern "C" void rocket_mod_camera_smoothing_finish(std::uint8_t* rdram, recomp_context* ctx);
extern "C" void rocket_mod_camera_first_person(std::uint8_t* rdram, recomp_context* ctx);
extern "C" void rocket_mod_camera_prepare(std::uint8_t* rdram, recomp_context* ctx);
extern "C" void rocket_mod_camera_candidates_begin(std::uint8_t* rdram, recomp_context* ctx);
extern "C" int rocket_mod_camera_retry(std::uint8_t* rdram, recomp_context* ctx);
extern "C" void rocket_mod_camera_candidates_finish(std::uint8_t* rdram, recomp_context* ctx);
extern "C" void rocket_mod_camera_volume_update(std::uint8_t* rdram, recomp_context* ctx);
extern "C" void rocket_mod_camera_volume_check(std::uint8_t* rdram, recomp_context* ctx);
extern "C" void rocket_mod_camera_candidates_exhausted(std::uint8_t* rdram, recomp_context* ctx);
