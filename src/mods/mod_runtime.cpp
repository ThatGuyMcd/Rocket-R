#include "mod_runtime.hpp"
#include "mod_library.hpp"
#include "platform.hpp"
#include "runtime_input.hpp"
#include "librecomp/mods.hpp"
#include "librecomp/overlays.hpp"
#include "mod_protection.generated.hpp"
#include "mod_camera.generated.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
struct GuestFloat {
    std::uint8_t* address;
    operator float() const { float value;std::memcpy(&value,address,4);return value; }
    GuestFloat& operator=(float value) {std::memcpy(address,&value,4);return *this;}
    GuestFloat& operator=(const GuestFloat& value) {return *this=static_cast<float>(value);}
};
}
#define MEM_F(offset, address) GuestFloat{rdram + ((static_cast<std::uint32_t>(address) + (offset)) & 0x3FFFFFFFU)}

namespace {
std::atomic<bool> camera_claimed{false};
std::atomic<bool> camera_requested_enabled{true};
std::atomic<bool> camera_enabled{true};
bool first_person_enabled = false;
float first_person_clock = -1;
std::uint32_t first_person_player = 0;
// MEM_W takes a sign-extended N64 register value, including for globals.
constexpr gpr kPlayerObject = static_cast<gpr>(static_cast<std::int32_t>(0x800AAF5CU));
float previous_clock = -1;
float last_target[3]{};
struct CameraRequest {
    bool valid = false;
    std::uint32_t controller = 0, eye = 0, target = 0;
    float clock = -1, yaw = 0, pitch = 0, distance = 0;
    float fitted_distance = 0;
    float resolved_yaw = 0;
    bool has_resolved_yaw = false;
    bool special_path = false;
    std::uint32_t stack = 0, preset = 0;
    int filtered_count = -1;
    unsigned retries = 0;
    bool exhausted = false;
    int zoom_index = 0;
    float resolved_pitch = 0, requested_pitch = 0, lift_limit = 0;
    bool has_resolved_pitch = false;
    unsigned lift_retries = 0;
} request;
float last_fitted_distance = 0, fit_hold_until = -1;
bool adapting_special_path = false;
std::uint32_t active_camera_volume = 0;
float camera_smoothing = 1;
struct SmoothingFrame {
    std::uint32_t stack = 0;
    bool captured = false;
    float amount = 1, orbit[3]{}, view_yaw = 0, view_pitch = 0;
} smoothing_frame;
void suspend_camera() {
    smoothing_frame = {};
    rocket::input::set_camera_input_owned(false);
    previous_clock = -1;
    request = {};
    last_fitted_distance = 0;
    fit_hold_until = -1;
}
void claim_camera(std::uint8_t*, recomp_context*) {
    camera_claimed = true;
    camera_enabled = camera_requested_enabled.load();
    rocket::input::set_camera_runtime_enabled(camera_enabled.load());
    rocket::input::set_camera_actions_active(camera_enabled);
    std::fprintf(stderr, "[mods] analogue camera ready (%s)\n",camera_enabled?"on":"standby");
}
bool guest_range(std::uint32_t address, std::uint32_t size) {
    return address >= 0x80000400U && address <= 0x80800000U - size && (address & 3) == 0;
}
bool camera_active() {
    if(!camera_claimed) return false;
    // A volume adapter runs a nested candidate solve. Finish that transaction
    // with its existing state before applying a switch from the UI thread.
    if(!adapting_special_path) {
        const bool requested=camera_requested_enabled.load();
        if(requested!=camera_enabled) {
            camera_enabled=requested;
            suspend_camera();
            first_person_clock=-1;
            rocket::platform::camera_input(); // Discard movement from the previous mode.
            rocket::input::set_camera_actions_active(camera_enabled);
            rocket::input::set_camera_runtime_enabled(camera_enabled.load());
            std::fprintf(stderr,"[mods-camera] switched %s during gameplay\n",camera_enabled?"on":"off");
        }
        // Also handles off/on requests both arriving before this boundary.
        rocket::input::set_camera_runtime_enabled(camera_enabled.load() && camera_requested_enabled.load());
    }
    return camera_enabled;
}
void claim_mouse(std::uint8_t*, recomp_context*) { rocket::input::set_camera_mouse_supported(true); }
void claim_first_person(std::uint8_t*, recomp_context*) { first_person_enabled = true; }
void set_camera_smoothing(std::uint8_t*, recomp_context* ctx) {
    camera_smoothing = std::min(static_cast<std::uint32_t>(ctx->r4), 100U) / 100.0F;
}
gpr guest(std::uint32_t address) {
    return static_cast<gpr>(static_cast<std::int32_t>(address));
}
bool first_person_view(std::uint8_t* rdram) {
    const auto player = static_cast<std::uint32_t>(MEM_W(0, kPlayerObject));
    return guest_range(player, 0x510) && MEM_W(0x50C, guest(player)) != 0 &&
        static_cast<std::uint32_t>(MEM_W(0x1E0, guest(0x8009F098U))) == 0x8009F30CU;
}
void emit_mouse_look(std::uint8_t* rdram, const recomp_context& ctx, gpr packet,
                     const rocket::platform::CameraInput& input) {
    if (input.mouse_yaw == 0 && input.mouse_pitch == 0) return;
    MEM_W(0, packet) = 1;
    MEM_W(4, packet) = 16;
    MEM_F(8, packet) = input.mouse_yaw;
    MEM_F(12, packet) = input.mouse_pitch;
    auto callback = ctx;
    callback.r29 = packet - 0x40;
    callback.r4 = packet;
    recomp_trigger_event(rdram, &callback, 2);
}
bool active_candidates(std::uint8_t* rdram, const recomp_context* ctx) {
    return request.valid && request.stack == static_cast<std::uint32_t>(ctx->r29) &&
        request.target == static_cast<std::uint32_t>(ctx->r30) &&
        static_cast<float>(MEM_F(0, 0x8009FE14U)) == request.clock &&
        static_cast<std::uint32_t>(MEM_W(0xB0, ctx->r29)) == request.preset;
}
void write_orbit_preset(std::uint8_t* rdram) {
    MEM_F(0, request.preset) = std::cos(request.pitch) * request.fitted_distance;
    MEM_F(4, request.preset) = std::sin(request.pitch) * request.fitted_distance;
}
}
void rocket::mods::configure_library() {
    static const auto included=inspect_package(rocket::generated::kCameraMod,".nrm");
    // 0.1.13 was shipped under the temporary 1.1.0-dev host version. The release
    // is 1.0.1; migrate only that exact bundled package, never arbitrary mods.
    library().migrate_bundled_package(rocket::generated::kCameraMod,
        "fd74d64491fdddfcdbb66ed8b354260b80d313a57f976c5695c803f86a05964b");
    library().allow_live_toggle(included.id,included.hash);
}
void rocket::mods::request_camera_enabled(bool enabled) {
    // Release input immediately, even if a fixed view or paused game delays
    // the next camera callback. A solve already in progress cannot reclaim it.
    if (!enabled) rocket::input::set_camera_runtime_enabled(false);
    camera_requested_enabled.store(enabled);
    std::fprintf(stderr, "[mods-camera] requested %s during gameplay\n", enabled ? "on" : "off");
}
rocket::mods::CameraStatus rocket::mods::camera_status() {
    return {camera_claimed.load(), camera_enabled.load(), camera_requested_enabled.load()};
}
void rocket::mods::register_api() {
    static const char* events[] = {"rocket_on_game_ready", "rocket_on_camera_update", "rocket_on_mouse_look", "rocket_on_first_person_update", nullptr};
    recomp::overlays::register_base_events(events);
    recomp::overlays::register_base_export("rocket_claim_analogue_camera", claim_camera);
    recomp::overlays::register_base_export("rocket_enable_mouse_look", claim_mouse);
    recomp::overlays::register_base_export("rocket_enable_first_person_look", claim_first_person);
    recomp::overlays::register_base_export("rocket_set_camera_smoothing", set_camera_smoothing);
    for (auto address : rocket::generated::kModProtectedFunctions)
        recomp::mods::protect_game_function(address);
}
void rocket::mods::prepare_runtime(bool without_mods) {
    camera_claimed = false;
    camera_enabled = true;
    camera_requested_enabled.store(true);
    rocket::input::set_camera_runtime_enabled(false);
    first_person_enabled = false;
    first_person_clock = -1;
    first_person_player = 0;
    camera_smoothing = 1; // Older mods retain the original camera behavior.
    rocket::input::set_camera_actions_active(false);
    rocket::input::set_camera_mouse_supported(false);
    suspend_camera();
    auto& lib = library();
    if (!lib.snapshot().running) lib.prepare_launch(without_mods);
    const auto active = lib.snapshot().active;
    for(const auto& package:active.at("packages")) if(package.at("id")=="rocket_modern_camera" && package.value("live_toggle",false))
        camera_requested_enabled.store(package.value("enabled",true));
    recomp::mods::configure_profile_paths(
        std::filesystem::u8path(active.at("runtime").get<std::string>()), lib.active_save_path());
    register_api();
    std::fprintf(stderr, "[mods] profile: %s; packages: %zu\n",
        active.at("name").get<std::string>().c_str(), active.at("packages").size());
}
void rocket::mods::game_ready(std::uint8_t* rdram, recomp_context* ctx) {
    recomp_trigger_event(rdram, ctx, 0);
    library().set_live_option_handler([](const OptionUpdate& update) {
        // The runtime synchronizes config storage and saves its session copy
        // asynchronously. The profile copy is already saved by the library.
        std::visit([&](const auto& value) {
            recomp::mods::set_mod_config_value(update.mod_id,update.option_id,value);
        },update.value);
    });
    library().set_live_toggle_handler([](const std::string& id,bool enabled) {
        if(id=="rocket_modern_camera") request_camera_enabled(enabled);
    });
    std::fprintf(stderr, "[mods] game-ready callbacks completed\n");
}

// Checked hook in func_80036D84, immediately before its object filter. The
// original function has already opened a second-heap scope and selected a zoom
// preset. Fallback angles, camera volumes and collision response still use
// the original game. Optional follow smoothing is adjusted before final collision.
extern "C" void rocket_mod_camera_prepare(std::uint8_t* rdram, recomp_context* ctx) {
    if (static_cast<std::uint32_t>(ctx->r4) != 0x8009F098U || !camera_active()) return;
    const auto stack = static_cast<std::uint32_t>(ctx->r29);
    if (stack < 0x80002000U || !guest_range(stack - 0x1000U, 0x111CU) ||
        MEM_W(0, kPlayerObject) == 0) {
        suspend_camera();
        return;
    }

    const auto controller = static_cast<std::uint32_t>(MEM_W(0x108, ctx->r29));
    const auto preset = static_cast<std::uint32_t>(MEM_W(0xB0, ctx->r29));
    const auto eye = static_cast<std::uint32_t>(MEM_W(0x114, ctx->r29));
    const auto target = static_cast<std::uint32_t>(ctx->r30);
    const auto settings = static_cast<std::uint32_t>(ctx->r20);
    if (!guest_range(controller, 0x1C) || !guest_range(preset, 16) ||
        !guest_range(eye, 12) || !guest_range(target, 12) ||
        !guest_range(settings, 0x5C) ||
        static_cast<std::uint32_t>(MEM_W(4, guest(controller))) != 0x8009F098U) {
        suspend_camera();
        return;
    }
    const bool heading_locked = (MEM_W(0, guest(settings)) & 0x20) != 0;

    const float horizontal = MEM_F(0, preset);
    const float height = MEM_F(4, preset);
    const float distance = std::hypot(horizontal, height);
    const float yaw = MEM_F(0x10, controller);
    const float clock = MEM_F(0, 0x8009FE14U);
    if (!std::isfinite(horizontal) || horizontal <= 0 || !std::isfinite(height) ||
        !std::isfinite(distance) || distance < 0.25F || distance > 2000 ||
        !std::isfinite(yaw) || !std::isfinite(clock)) {
        suspend_camera();
        return;
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (!std::isfinite(static_cast<float>(MEM_F(i * 4, target)))) {
            suspend_camera();
            return;
        }
    }

    const bool same_frame = request.valid && clock == request.clock;
    if (!same_frame) {
        first_person_clock = -1;
        float dt = clock - previous_clock;
        bool reset = !request.valid || dt <= 0 || dt > 0.25F;
        for (unsigned i = 0; i < 3; ++i) {
            const float value = MEM_F(i * 4, target);
            reset |= std::abs(value - last_target[i]) > 300;
            last_target[i] = value;
        }
        dt = reset ? 1.0F / 30.0F : std::clamp(dt, 0.001F, 0.1F);
        const auto packet = guest((stack - 0x100U) & ~15U);
        const auto input = rocket::platform::camera_input();
        emit_mouse_look(rdram, *ctx, packet, input);
        // A heading lock still uses the normal third-person camera. Releasing
        // ownership here would turn the same stick movement into a C-button
        // press. Continue from the last checked heading instead of integrating
        // each frame from the lock's unchanged angle.
        const bool keep_heading = heading_locked || request.special_path != adapting_special_path;
        const int zoom_index = MEM_W(0xC, guest(controller));
        const bool zoom_changed = request.valid && !request.special_path && !adapting_special_path &&
            request.zoom_index != zoom_index;
        const float preset_pitch = std::atan2(height, horizontal);
        const float input_pitch = !reset && !input.recenter && !zoom_changed && request.has_resolved_pitch
            ? request.resolved_pitch : preset_pitch;
        float input_yaw = keep_heading && !reset && !input.recenter && request.has_resolved_yaw
            ? request.resolved_yaw : yaw;
        if (input.recenter) {
            const auto player = static_cast<std::uint32_t>(MEM_W(0, kPlayerObject));
            if (guest_range(player, 0x2C0)) {
                const float heading = MEM_F(0x2BC, player);
                // Orbit yaw points from Rocket to the eye. Behind Rocket is
                // half a turn from his facing direction, not the current eye.
                if (std::isfinite(heading)) input_yaw = std::remainder(heading + 3.1415926536F, 6.283185307F);
            }
        }
        static bool reported_heading_lock = false;
        if (heading_locked && !reported_heading_lock) {
            std::fprintf(stderr, "[mods-camera] normal heading lock keeps analogue controls\n");
            reported_heading_lock = true;
        }
        MEM_W(0, packet) = 1;
        MEM_W(4, packet) = 56;
        MEM_F(8, packet) = dt;
        MEM_F(12, packet) = input.x;
        MEM_F(16, packet) = input.y;
        MEM_W(20, packet) = input.recenter;
        MEM_W(24, packet) = reset;
        MEM_F(28, packet) = input_yaw;
        MEM_F(32, packet) = input_pitch;
        MEM_F(36, packet) = distance;
        MEM_W(40, packet) = 0;
        MEM_F(44, packet) = input_yaw;
        MEM_F(48, packet) = MEM_F(32, packet);
        MEM_F(52, packet) = distance;

        // Both callbacks and the allocator run below our temporary packet with
        // copied registers, leaving the interrupted guest function intact.
        recomp_context callback = *ctx;
        callback.r29 = packet - 0x40;
        callback.r4 = packet;
        recomp_trigger_event(rdram, &callback, 1);
        const float output_yaw = MEM_F(44, packet);
        const float output_pitch = MEM_F(48, packet);
        const float output_distance = MEM_F(52, packet);
        if (MEM_W(40, packet) == 0 || !std::isfinite(output_yaw) ||
            !std::isfinite(output_pitch) || !std::isfinite(output_distance) ||
            output_distance < 0.25F || output_distance > 2000) {
            suspend_camera();
            return;
        }
        request = {true, controller, eye, target, clock,
            std::remainder(output_yaw, 6.283185307F),
            std::clamp(output_pitch, -0.30F, 1.10F), output_distance};
        request.resolved_yaw = input_yaw;
        request.has_resolved_yaw = true;
        request.resolved_pitch = input_pitch;
        request.has_resolved_pitch = true;
        request.requested_pitch = request.pitch;
        request.lift_limit = std::clamp(preset_pitch, 0.35F, 0.60F);
        request.zoom_index = zoom_index;
        if (reset || zoom_changed) {
            last_fitted_distance = 0;
            fit_hold_until = -1;
        }
        // Keep the last fitting distance until the passage opens. Outward
        // probes are small; a blocked probe will retry the last fit before
        // the original code can select a different viewing angle.
        request.fitted_distance = request.distance;
        if (last_fitted_distance > 0) {
            const float recovery = clock > fit_hold_until ? 2.0F * dt : 0.0F;
            request.fitted_distance = std::min(request.distance, last_fitted_distance + recovery);
        }
        previous_clock = clock;
    }
    request.controller = controller;
    request.eye = eye;
    request.target = target;
    request.special_path = adapting_special_path;

    // Do not edit the shared zoom table or store a lasting pointer below SP:
    // subsequent guest calls reuse that stack. The game's existing heap scope
    // keeps this copy alive through every avoidance candidate, then frees it.
    recomp_context allocation = *ctx;
    allocation.r29 = guest(((stack - 0x100U) & ~15U) - 0x40U);
    allocation.r4 = 16;
    get_function(static_cast<std::int32_t>(0x800615A4U))(rdram, &allocation);
    const auto temporary = static_cast<std::uint32_t>(allocation.r2);
    if (!guest_range(temporary, 16) || temporary == preset) {
        suspend_camera();
        return;
    }
    std::memcpy(rdram + (temporary & 0x7FFFFFFFU), rdram + (preset & 0x7FFFFFFFU), 16);
    request.stack = stack;
    request.preset = temporary;
    request.filtered_count = -1;
    request.retries = 0;
    request.lift_retries = 0;
    request.exhausted = false;
    write_orbit_preset(rdram);
    MEM_W(0xB0, ctx->r29) = static_cast<std::int32_t>(temporary);
    MEM_F(0x10, controller) = request.yaw;

    // This call's box must contain both positive and negative pitch, the ray
    // origins near Rocket, and every fallback heading. These are just filter
    // bounds: the untouched vanilla collision routines choose the actual eye.
    const float extent = std::max({distance, request.distance, std::abs(height)}) + 1.0F;
    ctx->r7 = guest(std::bit_cast<std::uint32_t>(extent));
    MEM_F(0x10, stack) = extent;
    rocket::input::set_camera_input_owned(true);
    static bool reported = false;
    if (!reported) {
        std::fprintf(stderr, "[mods] analogue orbit prepared before vanilla obstacle avoidance\n");
        reported = true;
    }
}

extern "C" void rocket_mod_camera_tick(std::uint8_t*, recomp_context* ctx) {
    // Entry to the main camera update, before any normal/corridor/FPS solve.
    if (static_cast<std::uint32_t>(ctx->r4) == 0x8009F098U) camera_active();
}

extern "C" void rocket_mod_camera_candidates_begin(std::uint8_t* rdram, recomp_context* ctx) {
    if (!active_candidates(rdram, ctx)) return;
    const auto count = static_cast<std::int32_t>(ctx->r2);
    if (count >= 0 && count <= 32768) request.filtered_count = count;
}

extern "C" void rocket_mod_camera_volume_check(std::uint8_t* rdram, recomp_context* ctx) {
    // The active rail region can deliberately exclude the NORMAL camera. Its
    // own camera is allowed inside it. Exempt only that exact region during
    // the rail adapter's solve; the next instructions still run the original
    // geometry rays, and all other exclusion regions retain their checks.
    if (adapting_special_path && active_camera_volume != 0 &&
        static_cast<std::uint32_t>(ctx->r16) == active_camera_volume &&
        active_candidates(rdram, ctx)) {
        if (ctx->r2 != 0) {
            static unsigned reports = 0;
            if (reports++ < 3)
                std::fprintf(stderr, "[mods-camera] active corridor region permits its own camera; geometry checks retained\n");
        }
        ctx->r2 = 0;
    }
}

extern "C" void rocket_mod_camera_candidates_exhausted(std::uint8_t* rdram, recomp_context* ctx) {
    if (active_candidates(rdram, ctx)) request.exhausted = true;
}

extern "C" int rocket_mod_camera_retry(std::uint8_t* rdram, recomp_context* ctx) {
    if (!active_candidates(rdram, ctx) || request.filtered_count < 0) return 0;
    // A low orbit often meets the floor. Preserve zoom and try the previous
    // safe elevation, then bounded higher candidates through the SAME native
    // checks. Shrinking a downward orbit first drives the eye into Rocket.
    if (request.requested_pitch < 0.20F && request.retries == 0 &&
        request.lift_retries < 6 && request.pitch < request.lift_limit - 0.001F) {
        float next = request.pitch + 0.12F;
        if (request.lift_retries == 0 && request.has_resolved_pitch &&
            request.resolved_pitch > request.pitch + 0.001F)
            next = request.resolved_pitch;
        request.pitch = std::min(next, request.lift_limit);
        ++request.lift_retries;
        write_orbit_preset(rdram);
        ctx->r2 = static_cast<gpr>(request.filtered_count);
        return 1;
    }
    // If lifting cannot fit (for example under a low ceiling), retain the
    // proven corridor fallback at the user's requested pitch.
    request.pitch = request.requested_pitch;
    write_orbit_preset(rdram);
    // The ORIGINAL candidate checks reached their blocked branch. Try a
    // closer eye with the same heading before the original angular fallback.
    // No host ray tests or post-smoothing position corrections are involved.
    fit_hold_until = request.clock + 0.45F;
    const float minimum = std::min(request.distance, 2.0F);
    if (request.retries >= 6 || request.fitted_distance <= minimum + 0.001F) return 0;
    float next = std::max(minimum, request.fitted_distance * 0.75F);
    if (last_fitted_distance >= minimum && last_fitted_distance < request.fitted_distance - 0.001F)
        next = last_fitted_distance;
    request.fitted_distance = next;
    ++request.retries;
    write_orbit_preset(rdram);
    // The retry label precedes the instruction which saves this return value
    // as the filtered object count. Reuse the original list and count.
    ctx->r2 = static_cast<gpr>(request.filtered_count);
    return 1;
}

extern "C" void rocket_mod_camera_candidates_finish(std::uint8_t* rdram, recomp_context* ctx) {
    if (!active_candidates(rdram, ctx)) return;
    // Exhaustion copies the previous published eye; that is not a new fit.
    if (request.exhausted) return;
    last_fitted_distance = request.fitted_distance;
    static unsigned fit_reports = 0;
    if (request.retries > 0 && fit_reports++ < 4) {
        std::fprintf(stderr, "[mods-camera] closer orbit checked by vanilla: retries=%u distance=%.3f requested=%.3f\n",
            request.retries, request.fitted_distance, request.distance);
    }
    const float x = MEM_F(0x18, request.stack) - MEM_F(0, request.target);
    const float y = MEM_F(0x1C, request.stack) - MEM_F(4, request.target);
    const float z = MEM_F(0x20, request.stack) - MEM_F(8, request.target);
    if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::hypot(x, y) > 0.001F) {
        request.resolved_yaw = std::atan2(y, x);
        request.has_resolved_yaw = true;
        request.resolved_pitch = std::clamp(std::atan2(z, std::hypot(x, y)), -0.30F, 1.10F);
        request.has_resolved_pitch = true;
    }
}

namespace {
bool adapt_special_path(std::uint8_t* rdram, const recomp_context& original, std::uint32_t volume) {
    // Camera-volume views use flags 0xD/0xF and skip func_80036D84 entirely.
    // Route their orbit through that same ORIGINAL candidate solver before
    // their own smoothing update. Fixed placements and first-person views
    // remain outside this adapter.
    if ((original.r5 & 0xDU) != 0xDU || MEM_W(0, kPlayerObject) == 0) return false;
    const auto eye = static_cast<std::uint32_t>(original.r6);
    const auto target = static_cast<std::uint32_t>(original.r7);
    const auto stack = static_cast<std::uint32_t>(original.r29);
    if (!guest_range(eye, 12) || !guest_range(target, 12) || stack < 0x80003000U ||
        !guest_range(stack - 0x2000U, 0x202CU)) return false;
    const auto world = static_cast<std::uint32_t>(MEM_W(0, guest(0x8009F094U)));
    const auto subject = static_cast<std::uint32_t>(MEM_W(0x1E4, original.r4));
    if (!guest_range(world, 0x34) || !guest_range(subject, 0x250)) return false;
    float saved_eye[3], saved_target[3];
    for (unsigned i = 0; i < 3; ++i) {
        saved_eye[i] = MEM_F(i * 4, eye);
        saved_target[i] = MEM_F(i * 4, target);
        if (!std::isfinite(saved_eye[i]) || !std::isfinite(saved_target[i])) return false;
    }
    const float x = saved_eye[0] - saved_target[0], y = saved_eye[1] - saved_target[1];
    const float height = saved_eye[2] - saved_target[2];
    const float horizontal = std::hypot(x, y), distance = std::hypot(horizontal, height);
    if (horizontal < 0.001F || !std::isfinite(distance) || distance < 0.25F || distance > 2000) return false;

    const auto scratch_stack = guest((stack - 0x400U) & ~15U);
    auto call = [&](std::uint32_t function, gpr a0 = 0) {
        auto context = original;
        context.r29 = scratch_stack;
        context.r4 = a0;
        get_function(static_cast<std::int32_t>(function))(rdram, &context);
        return context;
    };
    call(0x80061574U); // push_second_heap_state
    struct Scope {
        decltype(call)& invoke;
        ~Scope() { invoke(0x800615D4U); } // pop_second_heap_state
    } scope{call};
    const auto allocation = call(0x800615A4U, 0x80);
    const auto controller = static_cast<std::uint32_t>(allocation.r2);
    if (!guest_range(controller, 0x80)) return false;
    const auto settings = controller + 0x20;
    std::memset(rdram + (controller & 0x7FFFFFFFU), 0, 0x80);
    MEM_W(4, guest(controller)) = static_cast<std::int32_t>(0x8009F098U);
    MEM_F(0x10, controller) = std::atan2(y, x);
    // Skip automatic velocity-following and seed a single orbit preset. The
    // candidate solver still runs all its volume and obstruction checks.
    MEM_W(0, guest(settings)) = 0x2C;
    MEM_F(0x10, settings) = std::atan2(y, x);
    MEM_W(0x1C, guest(settings)) = 1;
    MEM_F(0x20, settings) = horizontal;
    MEM_F(0x24, settings) = height;
    for (unsigned i = 0; i < 3; ++i) MEM_F(0x50 + i * 4, settings) = saved_target[i];

    auto candidate = original;
    candidate.r29 = scratch_stack;
    candidate.r4 = guest(controller);
    candidate.r5 = guest(settings);
    candidate.r6 = guest(std::bit_cast<std::uint32_t>(1.0F / 30.0F));
    candidate.r7 = guest(eye);
    MEM_W(0x10, candidate.r29) = static_cast<std::int32_t>(target);
    {
        struct AdapterScope {
            AdapterScope(std::uint32_t volume) {
                adapting_special_path = true;
                active_camera_volume = volume;
            }
            ~AdapterScope() {
                adapting_special_path = false;
                active_camera_volume = 0;
            }
        } adapter_scope{volume};
        get_function(static_cast<std::int32_t>(0x80036D84U))(rdram, &candidate);
    }
    request.stack = 0; // The temporary solver frame and preset are no longer live.
    if (!request.valid || request.eye != eye || request.target != target ||
        !rocket::input::camera_input_owned()) {
        for (unsigned i = 0; i < 3; ++i) {
            MEM_F(i * 4, eye) = saved_eye[i];
            MEM_F(i * 4, target) = saved_target[i];
        }
        return false;
    }
    if (request.exhausted) {
        // A rail view has a moving native candidate of its own. Keeping the
        // normal solver's previous-eye fallback strands the camera at the
        // entrance. Let the native rail candidate proceed to its original
        // smoothing and geometry collision, while retaining analogue ownership.
        for (unsigned i = 0; i < 3; ++i) {
            MEM_F(i * 4, eye) = saved_eye[i];
            MEM_F(i * 4, target) = saved_target[i];
        }
        static unsigned reports = 0;
        if (reports++ < 3)
            std::fprintf(stderr, "[mods-camera] no orbit fits; using moving corridor view with analogue input retained\n");
    }
    static bool reported = false;
    if (!reported) {
        std::fprintf(stderr, "[mods-camera] camera-volume view uses analogue input and vanilla obstacle avoidance\n");
        reported = true;
    }
    return true;
}
}

extern "C" void rocket_mod_camera_volume_update(std::uint8_t* rdram, recomp_context* ctx) {
    // Checked callsite in func_80097D7C. s4 is the rail controller and s1 is
    // its active region here; no caller/register guesses at a shared entrypoint.
    if (static_cast<std::uint32_t>(ctx->r4) != 0x8009F098U || !camera_active()) return;
    const auto control = static_cast<std::uint32_t>(ctx->r20);
    const auto volume = static_cast<std::uint32_t>(ctx->r17);
    if (!guest_range(control, 0x18) || !guest_range(volume, 0xFC) ||
        static_cast<std::uint32_t>(MEM_W(4, guest(control))) != 0x8009F098U ||
        static_cast<std::uint32_t>(MEM_W(8, guest(control))) != volume) return;
    adapt_special_path(rdram, *ctx, volume);
}

extern "C" void rocket_mod_camera_update(std::uint8_t* rdram, recomp_context* ctx) {
    if (static_cast<std::uint32_t>(ctx->r4) != 0x8009F098U || !camera_active()) return;
    smoothing_frame = {};
    auto arm_smoothing = [&] {
        const auto stack = static_cast<std::uint32_t>(ctx->r29);
        if (camera_smoothing < 1 && stack >= 0x80002000U && guest_range(stack - 0x118U, 0x144U)) {
            smoothing_frame.stack = stack - 0x118U;
            smoothing_frame.amount = camera_smoothing;
        }
    };
    if (first_person_enabled && first_person_view(rdram)) {
        // Keep analogue sources away from C-buttons while the original
        // first-person camera handles its pose and transition smoothing.
        rocket::input::set_camera_input_owned(true);
        arm_smoothing();
        return;
    }
    // A normal camera already has a checked candidate. Volume cameras first
    // use the same candidate solver through an isolated guest frame. Neither
    // path overrides a checked eye or the smoothing/collision history.
    if (!request.valid || ctx->r5 == 0 || MEM_W(0, kPlayerObject) == 0 ||
        static_cast<float>(MEM_F(0, 0x8009FE14U)) != request.clock ||
        static_cast<std::uint32_t>(ctx->r6) != request.eye ||
        static_cast<std::uint32_t>(ctx->r7) != request.target) {
        static unsigned reports = 0;
        if (rocket::input::camera_input_owned() && reports++ < 8) {
            std::fprintf(stderr, "[mods-camera] original controls for a separate camera path: flags=%X eye=%08X target=%08X\n",
                static_cast<unsigned>(ctx->r5), static_cast<unsigned>(ctx->r6), static_cast<unsigned>(ctx->r7));
        }
        suspend_camera();
        first_person_clock = -1;
    }
    else arm_smoothing();
}

// These checked boundaries sit around the native spring integration, before
// func_8003A468 performs its final geometry checks. Keep every collision flag,
// candidate and publication step intact; only reduce the lag of the spring.
extern "C" void rocket_mod_camera_smoothing_begin(std::uint8_t* rdram, recomp_context* ctx) {
    if (!smoothing_frame.stack || smoothing_frame.stack != static_cast<std::uint32_t>(ctx->r29) ||
        static_cast<std::uint32_t>(ctx->r16) != 0x8009F098U || !(ctx->r17 & 1) ||
        !camera_enabled || !camera_requested_enabled) return;
    for (unsigned i = 0; i < 3; ++i) smoothing_frame.orbit[i] = MEM_F(0x68 + i * 4, smoothing_frame.stack);
    smoothing_frame.view_yaw = ctx->f24.fl;
    smoothing_frame.view_pitch = ctx->f22.fl;
    smoothing_frame.captured = true;
}
extern "C" void rocket_mod_camera_smoothing_finish(std::uint8_t* rdram, recomp_context* ctx) {
    if (!smoothing_frame.captured || smoothing_frame.stack != static_cast<std::uint32_t>(ctx->r29) ||
        static_cast<std::uint32_t>(ctx->r16) != 0x8009F098U) return;
    const auto frame = smoothing_frame;
    smoothing_frame = {};
    if (!camera_enabled || !camera_requested_enabled) return;
    float orbit[3], velocity[5];
    const float view_yaw = ctx->f26.fl, view_pitch = ctx->f24.fl;
    for (unsigned i = 0; i < 3; ++i) {
        orbit[i] = MEM_F(0xA8 + i * 4, frame.stack);
        if (!std::isfinite(orbit[i]) || !std::isfinite(frame.orbit[i])) return;
    }
    for (unsigned i = 0; i < 5; ++i) {
        velocity[i] = MEM_F(0x124 + i * 4, 0x8009F098U);
        if (!std::isfinite(velocity[i])) return;
    }
    if (!std::isfinite(view_yaw) || !std::isfinite(view_pitch) ||
        !std::isfinite(frame.view_yaw) || !std::isfinite(frame.view_pitch) ||
        orbit[2] <= 0 || frame.orbit[2] <= 0) return;
    const float follow = 1 - frame.amount;
    auto angle = [follow](float from, float to) {
        return std::remainder(from + std::remainder(to - from, 6.283185307F) * follow, 6.283185307F);
    };
    MEM_F(0xA8, frame.stack) = angle(orbit[0], frame.orbit[0]);
    MEM_F(0xAC, frame.stack) = orbit[1] + (frame.orbit[1] - orbit[1]) * follow;
    MEM_F(0xB0, frame.stack) = orbit[2] + (frame.orbit[2] - orbit[2]) * follow;
    ctx->f26.fl = angle(view_yaw, frame.view_yaw);
    ctx->f24.fl = view_pitch + (frame.view_pitch - view_pitch) * follow;
    for (unsigned i = 0; i < 5; ++i) MEM_F(0x124 + i * 4, 0x8009F098U) = velocity[i] * frame.amount;
}

// Checked entry of the player's original first-person look routine. Add the
// mod's angles here, then let the original left-stick update and pitch clamp
// run. Camera placement, transitions and zoom switching remain native.
extern "C" void rocket_mod_camera_first_person(std::uint8_t* rdram, recomp_context* ctx) {
    if (!camera_active() || !first_person_enabled || !first_person_view(rdram)) return;
    const auto player = static_cast<std::uint32_t>(ctx->r4);
    const auto stack = static_cast<std::uint32_t>(ctx->r29);
    if (player != static_cast<std::uint32_t>(MEM_W(0, kPlayerObject)) ||
        MEM_W(0x3AC, ctx->r4) == 4 || stack < 0x80002000U ||
        !guest_range(stack - 0x1000U, 0x1000U)) return;
    const float clock = MEM_F(0, 0x8009FE14U);
    const float native_yaw = MEM_F(0x2D8, player), native_pitch = MEM_F(0x2DC, player);
    if (!std::isfinite(clock) || !std::isfinite(native_yaw) || !std::isfinite(native_pitch)) return;
    if (first_person_player == player && first_person_clock == clock) return;
    const float elapsed = clock - first_person_clock;
    const bool reset = first_person_player != player || first_person_clock < 0 || elapsed <= 0 || elapsed > 0.25F;
    const float dt = reset ? 1.0F / 30.0F : std::clamp(elapsed, 0.001F, 0.1F);
    const auto input = rocket::platform::camera_input();
    const auto packet = guest((stack - 0x100U) & ~15U);
    emit_mouse_look(rdram, *ctx, packet, input);
    // Native yaw and orbit yaw both rotate in the same world direction.
    // Only pitch needs conversion: native positive pitch looks down.
    float yaw = native_yaw, pitch = -native_pitch;
    if (input.recenter) {
        const float heading = MEM_F(0x2BC, player);
        if (std::isfinite(heading)) { yaw = heading; pitch = 0.15F; }
    }
    MEM_W(0, packet) = 1; MEM_W(4, packet) = 48;
    MEM_F(8, packet) = dt; MEM_F(12, packet) = input.x; MEM_F(16, packet) = input.y;
    MEM_W(20, packet) = input.recenter; MEM_W(24, packet) = reset;
    MEM_F(28, packet) = yaw; MEM_F(32, packet) = pitch;
    MEM_W(36, packet) = 0;
    MEM_F(40, packet) = yaw; MEM_F(44, packet) = pitch;
    auto callback = *ctx;
    callback.r29 = packet - 0x40; callback.r4 = packet;
    recomp_trigger_event(rdram, &callback, 3);
    const float output_yaw = MEM_F(40, packet), output_pitch = MEM_F(44, packet);
    if (!MEM_W(36, packet) || !std::isfinite(output_yaw) || !std::isfinite(output_pitch)) return;
    MEM_F(0x2D8, player) = std::remainder(output_yaw, 6.283185307F);
    MEM_F(0x2DC, player) = -std::clamp(output_pitch, -1.0F, 1.0F);
    first_person_clock = clock; first_person_player = player;
    request = {}; previous_clock = -1; last_fitted_distance = 0; fit_hold_until = -1;
    rocket::input::set_camera_input_owned(true);
    static bool reported = false;
    if (!reported) {
        std::fprintf(stderr, "[mods-camera] first-person analogue look uses the original camera view\n");
        reported = true;
    }
}
