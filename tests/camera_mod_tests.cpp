#include "mods/mod_runtime.hpp"
#include "mods/mod_library.hpp"
#include "mod_camera.generated.hpp"
#include "platform.hpp"
#include "runtime_input.hpp"
#include "librecomp/overlays.hpp"
#include "librecomp/mods.hpp"
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#include <chrono>

namespace {
constexpr std::uint32_t camera = 0x8009F098, controller = 0x80200000;
constexpr std::uint32_t settings = 0x80201000, preset = settings + 0x20;
constexpr std::uint32_t eye = 0x80300000, target = 0x80300020, stack = 0x80310000;
constexpr std::uint32_t scratch = 0x80380000;
constexpr std::uint32_t rail_controller = 0x80220000, rail_volume = 0x80220100;
int checks = 0, callbacks = 0, allocations = 0;
int heap_depth = 0, volume_solves = 0;
int mouse_callbacks = 0;
int first_person_callbacks = 0;
bool first_person_apply = true, first_person_invalid = false;
bool first_person_reset = false, first_person_recenter = false;
float first_person_yaw = 0, first_person_pitch = 0;
float first_person_output_pitch = 0.4F;
bool owned = false, apply = true, invalid_output = false, last_reset = false;
bool runtime_enabled = true;
bool simulate_game_ready = false;
bool fail_allocation = false;
bool exhaust_volume = false;
recomp_func_t* claim = nullptr;
recomp_func_t* claim_first_person = nullptr;
recomp_func_t* smoothing_option = nullptr;
rocket::platform::CameraInput test_input{1, 0, false};
float last_dt = 0, last_yaw = 0, last_pitch = 0, output_pitch = 0.5F;
float expected_radius = std::hypot(6.5F, 2.5F);
gpr guest(std::uint32_t address) { return static_cast<gpr>(static_cast<std::int32_t>(address)); }
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
    ++checks;
}
template<typename T> T read(const std::uint8_t* ram, std::uint32_t address) {
    T value;
    std::memcpy(&value, ram + (address & 0x7FFFFFFFU), sizeof(value));
    return value;
}
template<typename T> void write(std::uint8_t* ram, std::uint32_t address, T value) {
    std::memcpy(ram + (address & 0x7FFFFFFFU), &value, sizeof(value));
}
}

namespace rocket::input {
void set_camera_input_owned(bool value) { owned = value; }
bool camera_input_owned() { return owned && runtime_enabled; }
void set_camera_runtime_enabled(bool value) { runtime_enabled=value; }
void set_camera_mouse_supported(bool) {}
bool actions_active=false;
void set_camera_actions_active(bool value) {actions_active=value;}
}
namespace rocket::platform {
CameraInput camera_input() {
    auto sample = test_input;
    test_input.mouse_yaw = test_input.mouse_pitch = 0;
    return sample;
}
}
namespace recomp::mods {
void protect_game_function(std::uint32_t) {}
void configure_profile_paths(const std::filesystem::path&, const std::filesystem::path&) {}
void set_mod_config_value(const std::string&, const std::string&, const ConfigValueVariant&) {}
}
namespace recomp::overlays {
void register_base_events(char const* const*) {}
void register_base_export(const std::string& name, recomp_func_t* func) {
    if (name == "rocket_claim_analogue_camera") claim = func;
    if (name == "rocket_enable_first_person_look") claim_first_person = func;
    if (name == "rocket_set_camera_smoothing") smoothing_option = func;
}
}
void allocate(std::uint8_t*, recomp_context* ctx) {
    ++allocations;
    check((ctx->r4 == 16 || ctx->r4 == 0x80) && static_cast<std::uint32_t>(ctx->r29) < stack - 0x100,
        "preset allocation uses its own stack and the existing guest heap scope");
    ctx->r2 = fail_allocation ? 0 : guest(ctx->r4 == 16 ? scratch : 0x80390000);
}
void push_heap(std::uint8_t*, recomp_context*) { ++heap_depth; }
void pop_heap(std::uint8_t*, recomp_context*) { --heap_depth; }
void volume_solver(std::uint8_t* ram, recomp_context* ctx) {
    ++volume_solves;
    check(heap_depth == 1, "volume adapter keeps its temporary controller in a scoped guest heap");
    const auto control = static_cast<std::uint32_t>(ctx->r4);
    const auto options = static_cast<std::uint32_t>(ctx->r5);
    const auto output = static_cast<std::uint32_t>(ctx->r7);
    const auto focus = read<std::uint32_t>(ram, static_cast<std::uint32_t>(ctx->r29) + 0x10);
    check(read<std::uint32_t>(ram, control + 4) == camera &&
        read<std::uint32_t>(ram, options + 0x1C) == 1,
        "volume view is routed through the original main-camera candidate solver");
    // Simulate the solver's documented hook boundaries, not its geometry.
    auto frame = *ctx;
    frame.r29 -= 0x108;
    const auto local = static_cast<std::uint32_t>(frame.r29);
    write(ram, local + 0x108, control);
    write(ram, local + 0x114, output);
    write(ram, local + 0xB0, options + 0x20);
    frame.r4 = guest(camera);
    frame.r20 = guest(options);
    frame.r30 = guest(focus);
    rocket_mod_camera_prepare(ram, &frame);
    frame.r2 = 3;
    rocket_mod_camera_candidates_begin(ram, &frame);
    if (owned) {
        frame.r16 = guest(rail_volume);
        frame.r2 = 1; // This region excludes the ordinary camera.
        const auto before_region = frame;
        rocket_mod_camera_volume_check(ram, &frame);
        auto expected_region = before_region;
        expected_region.r2 = 0;
        check(std::memcmp(&frame, &expected_region, sizeof(frame)) == 0,
            "active corridor permits its own camera without changing geometry-test registers");
        frame.r16 = guest(rail_volume + 0x100);
        frame.r2 = 1;
        rocket_mod_camera_volume_check(ram, &frame);
        check(frame.r2 == 1, "other exclusion regions remain blocked inside the corridor solve");
    }
    const auto selected = read<std::uint32_t>(ram, local + 0xB0);
    const float heading = read<float>(ram, control + 0x10);
    write(ram, local + 0x18, read<float>(ram, focus) + std::cos(heading) * read<float>(ram, selected));
    write(ram, local + 0x1C, read<float>(ram, focus + 4) + std::sin(heading) * read<float>(ram, selected));
    write(ram, local + 0x20, read<float>(ram, focus + 8) + read<float>(ram, selected + 4));
    if (exhaust_volume) {
        rocket_mod_camera_candidates_exhausted(ram, &frame);
        for (unsigned i = 0; i < 3; ++i)
            write(ram, local + 0x18 + i * 4, read<float>(ram, camera + i * 4));
    }
    rocket_mod_camera_candidates_finish(ram, &frame);
    for (unsigned i = 0; i < 3; ++i) write(ram, output + i * 4, read<float>(ram, local + 0x18 + i * 4));
    *ctx = frame; // The real callee also changes its caller's register copy.
}
extern "C" recomp_func_t* get_function(std::int32_t address) {
    switch (static_cast<std::uint32_t>(address)) {
    case 0x800615A4: return allocate;
    case 0x80061574: return push_heap;
    case 0x800615D4: return pop_heap;
    case 0x80036D84: return volume_solver;
    default: check(false, "unexpected guest function: bridge must use the original candidate solver"); return nullptr;
    }
}
extern "C" void recomp_trigger_event(std::uint8_t* ram, recomp_context* ctx, std::uint32_t event) {
    if (event == 0 && simulate_game_ready) { claim(ram,ctx); claim_first_person(ram,ctx); return; }
    if (event == 3) {
        ++first_person_callbacks;
        const auto packet = static_cast<std::uint32_t>(ctx->r4);
        check(read<std::uint32_t>(ram,packet)==1 && read<std::uint32_t>(ram,packet+4)==48,
            "first-person callback uses its own versioned packet");
        check(ctx->r29 < ctx->r4 && (packet & 15)==0,"first-person callback has an isolated aligned stack");
        first_person_reset = read<std::uint32_t>(ram,packet+24)!=0;
        first_person_recenter = read<std::uint32_t>(ram,packet+20)!=0;
        first_person_yaw = read<float>(ram,packet+28);
        first_person_pitch = read<float>(ram,packet+32);
        write(ram,packet+36,std::uint32_t{first_person_apply});
        write(ram,packet+40,first_person_invalid ? std::numeric_limits<float>::quiet_NaN() : first_person_yaw+0.2F);
        write(ram,packet+44,first_person_output_pitch);
        return;
    }
    if (event == 2) {
        ++mouse_callbacks;
        const auto packet = static_cast<std::uint32_t>(ctx->r4);
        check(read<std::uint32_t>(ram,packet)==1 && read<std::uint32_t>(ram,packet+4)==16,
            "mouse event uses a separate versioned packet without changing the camera ABI");
        check(read<float>(ram,packet+8)==0.15F && read<float>(ram,packet+12)==-0.07F,
            "mouse angular deltas reach the mod unchanged by frame time or stick shaping");
        return;
    }
    if (event != 1) return;
    ++callbacks;
    const auto packet = static_cast<std::uint32_t>(ctx->r4);
    check(read<std::uint32_t>(ram, packet) == 1 && read<std::uint32_t>(ram, packet + 4) == 56,
        "camera packet has the public ABI version and size");
    check(ctx->r29 < ctx->r4 && (packet & 15) == 0, "callback receives its own aligned guest stack");
    last_dt = read<float>(ram, packet + 8);
    last_reset = read<std::uint32_t>(ram, packet + 24) != 0;
    last_yaw = read<float>(ram, packet + 28);
    last_pitch = read<float>(ram, packet + 32);
    check(read<float>(ram, packet + 12) == test_input.x, "analogue input reaches the mod");
    check(std::abs(read<float>(ram, packet + 36) - expected_radius) < 0.001F,
        "distance comes from the unmodified game preset rather than the previous smoothed eye");
    write(ram, packet + 40, static_cast<std::uint32_t>(apply));
    write(ram, packet + 44, invalid_output ? std::numeric_limits<float>::quiet_NaN() : last_yaw + 0.05F);
    write(ram, packet + 48, output_pitch);
    write(ram, packet + 52, read<float>(ram, packet + 36));
}

void test_camera_mod_logic();
int main() {
    test_camera_mod_logic();
    // Real RDRAM size catches unsigned guest globals which would otherwise
    // address memory four gigabytes beyond this buffer.
    std::vector<std::uint8_t> memory(8 * 1024 * 1024);
    auto* ram = memory.data();
    recomp_context ctx{};
    ctx.r4 = guest(camera);
    ctx.r5 = guest(0x80210000);
    ctx.r7 = std::bit_cast<std::uint32_t>(6.5F);
    ctx.r20 = guest(settings);
    ctx.r29 = guest(stack);
    ctx.r30 = guest(target);
    const auto initial = ctx;
    float clock = 1;
    write(ram, 0x800AAF5C, std::uint32_t{0x80210000});
    write(ram, controller + 4, camera);
    write(ram, controller + 0x10, 1.0F);
    write(ram, stack + 0x108, controller);
    write(ram, stack + 0x114, eye);
    write(ram, preset, 6.5F);
    write(ram, preset + 4, 2.5F);
    write(ram, preset + 8, std::uint32_t{0x12345678});
    write(ram, preset + 12, std::uint32_t{0x87654321});
    write(ram, eye, 100.0F);
    write(ram, eye + 4, 20.0F);
    write(ram, eye + 8, 30.0F);
    write(ram, target, 10.0F);
    write(ram, target + 4, 20.0F);
    write(ram, target + 8, 30.0F);
    write(ram, camera + 0x144, 123.0F);

    auto prepare_frame = [&] {
        clock += 1.0F / 30.0F;
        write(ram, 0x8009FE14, clock);
        write(ram, stack + 0xB0, preset);
        write(ram, stack + 0x10, 2.5F);
        rocket_mod_camera_prepare(ram, &ctx);
    };
    auto publish = [&] {
        recomp_context published{};
        published.r4 = guest(camera);
        published.r5 = 5;
        published.r6 = guest(eye);
        published.r7 = guest(target);
        published.r29 = guest(stack + 0x108);
        rocket_mod_camera_update(ram, &published);
        return published;
    };

    prepare_frame();
    check(callbacks == 0 && allocations == 0 && !owned, "no mod leaves the game unchanged");
    rocket::mods::register_api();
    check(claim != nullptr, "camera claim is exported");
    rocket::mods::request_camera_enabled(false);
    check(!rocket::input::camera_input_owned(),"off releases input before the next guest camera callback");
    auto tick=ctx; tick.r4=guest(camera);
    claim(ram, &ctx);
    rocket_mod_camera_tick(ram,&tick);
    check(!rocket::mods::camera_status().enabled,"main update applies disable before selecting a camera controller");
    prepare_frame();
    check(callbacks==0 && allocations==0 && !owned && !rocket::input::actions_active,
        "a camera mod loaded in standby leaves native look and inputs untouched");
    rocket::mods::request_camera_enabled(true);
    check(!owned, "claiming alone cannot disable the right stick");
    prepare_frame();
    check(callbacks == 1 && owned && last_reset, "first valid gameplay request acquires the camera");
    check(std::abs(read<float>(ram, controller + 0x10) - 1.05F) < 0.001F,
        "analogue heading goes into the original controller before candidate selection");
    check(read<std::uint32_t>(ram, stack + 0xB0) == scratch,
        "candidate selection uses a scoped preset copy");
    const float radius = std::hypot(6.5F, 2.5F);
    check(std::abs(read<float>(ram, scratch) - std::cos(output_pitch) * radius) < 0.001F &&
        std::abs(read<float>(ram, scratch + 4) - std::sin(output_pitch) * radius) < 0.001F,
        "every vanilla fallback candidate receives the requested vertical orbit");
    check(read<float>(ram, preset) == 6.5F && read<float>(ram, preset + 4) == 2.5F &&
        read<std::uint32_t>(ram, scratch + 8) == 0x12345678 &&
        read<std::uint32_t>(ram, scratch + 12) == 0x87654321,
        "shared zoom presets remain intact and unrelated preset fields survive");
    auto expected = initial;
    expected.r7 = std::bit_cast<std::uint32_t>(radius + 1);
    check(std::memcmp(&ctx, &expected, sizeof(ctx)) == 0,
        "only the intended object-filter argument changes in the interrupted registers");
    check(read<float>(ram, stack + 0x10) == radius + 1,
        "object filter covers the entire requested orbit");
    check(read<float>(ram, eye) == 100 && read<float>(ram, camera + 0x144) == 123,
        "request hook never changes camera positions or collision history");

    check(smoothing_option!=nullptr,"camera follow smoothing is available as an optional mod API");
    auto check_smoothing = [&](int percent, bool interrupted=false) {
        auto option=initial; option.r4=percent;
        smoothing_option(ram,&option);
        auto published=publish();
        auto spring=published;
        spring.r16=guest(camera); spring.r17=5; spring.r29-=0x118;
        const auto local=static_cast<std::uint32_t>(spring.r29);
        write(ram,local+0x68,-3.05F); write(ram,local+0x6C,0.4F); write(ram,local+0x70,7.0F);
        spring.f24.fl=-3.05F; spring.f22.fl=0.3F;
        rocket_mod_camera_smoothing_begin(ram,&spring);
        // The native spring has produced its provisional orbit. The finish
        // hook must act here, with the original geometry checks still ahead.
        write(ram,local+0xA8,3.05F); write(ram,local+0xAC,0.1F); write(ram,local+0xB0,5.0F);
        spring.f26.fl=3.05F; spring.f24.fl=0.1F;
        for(unsigned i=0;i<5;++i) write(ram,camera+0x124+i*4,2.0F);
        const auto saved=spring;
        if(interrupted) rocket::mods::request_camera_enabled(false);
        rocket_mod_camera_smoothing_finish(ram,&spring);
        const float amount=interrupted ? 1 : std::min(percent,100)/100.0F;
        check(std::abs(read<float>(ram,local+0xAC)-(0.4F-0.3F*amount))<0.00001F &&
            std::abs(read<float>(ram,local+0xB0)-(7.0F-2.0F*amount))<0.00001F,
            "momentum slider continuously blends native follow lag from direct to original");
        const float expected_turn=std::remainder(-3.05F-3.05F,6.283185307F)*(1-amount);
        check(std::abs(std::remainder(spring.f26.fl-3.05F,6.283185307F)-expected_turn)<0.00001F &&
            std::abs(std::remainder(read<float>(ram,local+0xA8)-3.05F,6.283185307F)-expected_turn)<0.00001F,
            "follow smoothing crosses the angle wrap by the short arc without reversing");
        auto expected=saved; expected.f26=spring.f26; expected.f24=spring.f24;
        check(std::memcmp(&spring,&expected,sizeof(spring))==0 && spring.r17==5,
            "smoothing preserves native collision flags, pointers and unrelated registers");
        check(read<float>(ram,eye)==100 && read<float>(ram,camera+0x144)==123,
            "adjustable momentum cannot bypass collision by writing the published eye or history");
        check(std::abs(read<float>(ram,camera+0x124)-2.0F*amount)<0.00001F &&
            std::abs(read<float>(ram,camera+0x134)-2.0F*amount)<0.00001F,
            "reducing momentum removes old spring velocity instead of carrying a delayed turn");
        if(amount==1) check(std::memcmp(&spring,&saved,sizeof(spring))==0,
            "original smoothing and disabled mods leave native state unchanged");
        for(unsigned i=0;i<5;++i) write(ram,camera+0x124+i*4,0.0F);
        if(interrupted) { rocket::mods::request_camera_enabled(true); rocket_mod_camera_tick(ram,&tick); }
    };
    for(int value : {100,0,25,50,75,100,150}) check_smoothing(value);
    check_smoothing(0,true);
    { auto option=initial; option.r4=100; smoothing_option(ram,&option); }

    // Simulate the game's own obstacle selection changing both heading and eye.
    write(ram, controller + 0x10, -0.8F);
    write(ram, eye, 12.0F);
    write(ram, eye + 4, 24.0F);
    write(ram, eye + 8, 33.0F);
    publish();
    check(owned && read<float>(ram, eye) == 12 && read<float>(ram, eye + 4) == 24 &&
        read<float>(ram, eye + 8) == 33 && read<float>(ram, camera + 0x144) == 123,
        "late hook preserves the obstacle-checked eye and the game's smoothing history");
    prepare_frame();
    check(!last_reset && std::abs(last_dt - 1.0F / 30.0F) < 0.0001F,
        "next callback receives gameplay delta time");
    check(last_yaw == -0.8F, "the next mod step starts at the game's obstacle-corrected heading");
    const int before_duplicate = callbacks;
    write(ram, stack + 0xB0, preset);
    rocket_mod_camera_prepare(ram, &ctx);
    check(callbacks == before_duplicate && owned, "same-frame updates cannot integrate stick input twice");

    output_pitch = -0.3F;
    prepare_frame();
    check(read<float>(ram, scratch + 4) < 0 && read<float>(ram, stack + 0x10) > 0,
        "looking below the target cannot invert the object-filter bounding box");
    output_pitch = 20.0F;
    prepare_frame();
    check(std::abs(read<float>(ram, scratch + 4) - std::sin(1.1F) * radius) < 0.001F,
        "unsafe vertical angles are bounded before reaching the game");
    output_pitch = 0.5F;

    auto published = publish();
    published.r4 = guest(0x80220000);
    rocket_mod_camera_update(ram, &published);
    check(owned, "other camera objects do not revoke gameplay input ownership");
    published.r4 = guest(camera);
    published.r5 = 0;
    rocket_mod_camera_update(ram, &published);
    check(!owned, "fixed-camera placement releases the right stick");
    prepare_frame();
    check(owned && last_reset, "returning to third person resets the mod");
    clock += 1.0F / 30.0F;
    write(ram, 0x8009FE14, clock);
    publish();
    check(!owned, "a camera mode without the early hook keeps its original controls");
    prepare_frame();
    check(owned, "normal mode can reacquire input");
    published = publish();
    published.r6 = guest(eye + 0x100);
    rocket_mod_camera_update(ram, &published);
    check(!owned, "a different main-camera path cannot reuse an earlier request");

    write(ram, settings, std::uint32_t{0x20});
    const int before_locked = callbacks;
    prepare_frame();
    check(owned && callbacks == before_locked + 1,
        "a heading lock within third-person mode cannot hand the stick to vanilla C buttons");
    write(ram, stack + 0x18, 10.0F);
    write(ram, stack + 0x1C, 25.0F);
    rocket_mod_camera_candidates_finish(ram, &ctx);
    write(ram, controller + 0x10, 0.0F); // The original heading lock runs again.
    prepare_frame();
    check(owned && std::abs(last_yaw - 1.5707963268F) < 0.001F,
        "locked headings integrate from the last checked angle instead of restarting each frame");
    test_input.recenter = true;
    write(ram, controller + 0x10, -0.4F);
    write(ram, 0x80210000 + 0x2BC, 1.2F);
    prepare_frame();
    check(std::abs(std::remainder(last_yaw-1.2F,6.283185307F)) > 3.1415F,
        "recenter requests behind Rocket even when a corridor locks the old heading");
    check(read<float>(ram, eye)==12 && read<float>(ram,eye+4)==24,
        "recenter requests a heading without teleporting the obstacle-checked eye");
    write(ram, settings, std::uint32_t{0});
    for (float heading : {0.0F, 1.5707963268F, -1.5707963268F, 3.1415926536F}) {
        write(ram, 0x80210000 + 0x2BC, heading);
        prepare_frame();
        check(std::cos(last_yaw)*std::cos(heading)+std::sin(last_yaw)*std::sin(heading)<-0.9999F,
            "third-person recenter points behind Rocket at every cardinal heading");
    }
    write(ram,0x80210000+0x2BC,std::numeric_limits<float>::quiet_NaN());
    write(ram,controller+0x10,-0.4F);
    prepare_frame();
    check(last_yaw==-0.4F,"invalid player heading keeps the current safe camera angle");
    write(ram,0x80210000+0x2BC,0.0F);
    test_input.recenter = false;
    write(ram, settings, std::uint32_t{0});
    write(ram, 0x800AAF5C, std::uint32_t{0});
    const int before_leaving = callbacks;
    prepare_frame();
    check(!owned && callbacks == before_leaving, "leaving gameplay cannot invoke a camera mod");
    write(ram, 0x800AAF5C, std::uint32_t{0x80210000});
    invalid_output = true;
    const int before_invalid = allocations;
    const float original_yaw = read<float>(ram, controller + 0x10);
    prepare_frame();
    check(!owned && allocations == before_invalid && read<float>(ram, controller + 0x10) == original_yaw,
        "nonfinite mod output cannot change game state or consume input");
    invalid_output = false;
    apply = false;
    prepare_frame();
    check(!owned && allocations == before_invalid, "declined callbacks preserve the original camera");
    apply = true;
    fail_allocation = true;
    prepare_frame();
    check(!owned && read<std::uint32_t>(ram, stack + 0xB0) == preset &&
        read<float>(ram, controller + 0x10) == original_yaw,
        "failed scratch allocation does not partially apply an orbit");
    fail_allocation = false;
    ctx.r30 = guest(0x80800000);
    prepare_frame();
    check(!owned, "out-of-bounds guest pointers are rejected");
    ctx = initial;
    write(ram, controller + 0x10, std::numeric_limits<float>::quiet_NaN());
    prepare_frame();
    check(!owned, "invalid game heading cannot enter the mod callback");

    write(ram, controller + 0x10, 0.0F);
    // Model a floor plane with the real retry bridge. The native game still
    // owns geometry; this fixture verifies candidate policy and feedback.
    output_pitch=-0.25F;
    prepare_frame();
    auto accept_floor_candidate = [&] {
        ctx.r2=3;
        rocket_mod_camera_candidates_begin(ram,&ctx);
        unsigned attempts=0;
        while(read<float>(ram,scratch+4)<0.5F && attempts<12) {
            check(rocket_mod_camera_retry(ram,&ctx)==1,"blocked floor view can retry at a safer height");
            ++attempts;
        }
        check(read<float>(ram,scratch+4)>=0.5F && attempts<12,
            "a ground-blocked low orbit finds a raised candidate without getting stuck");
        check(std::abs(std::hypot(read<float>(ram,scratch),read<float>(ram,scratch+4))-radius)<0.001F,
            "floor avoidance preserves zoom instead of collapsing into Rocket");
        const float angle=read<float>(ram,controller+0x10);
        write(ram,stack+0x18,read<float>(ram,target)+std::cos(angle)*read<float>(ram,scratch));
        write(ram,stack+0x1C,read<float>(ram,target+4)+std::sin(angle)*read<float>(ram,scratch));
        write(ram,stack+0x20,read<float>(ram,target+8)+read<float>(ram,scratch+4));
        rocket_mod_camera_candidates_finish(ram,&ctx);
    };
    accept_floor_candidate();
    const float safe_pitch=std::atan2(read<float>(ram,scratch+4),read<float>(ram,scratch));
    prepare_frame();
    check(std::abs(last_pitch-safe_pitch)<0.00001F,"collision-corrected elevation returns to the mod on the next frame");
    for(unsigned i=0;i<20;++i) {
        output_pitch=last_pitch-0.05F;
        prepare_frame();
        accept_floor_candidate();
    }
    output_pitch=-0.25F;
    prepare_frame(); ctx.r2=3; rocket_mod_camera_candidates_begin(ram,&ctx);
    unsigned blocked_attempts=0;
    while(rocket_mod_camera_retry(ram,&ctx)) {
        check(++blocked_attempts<=12,"floor and ceiling blockage cannot create an unbounded retry loop");
    }
    check(std::abs(std::atan2(read<float>(ram,scratch+4),read<float>(ram,scratch))+0.25F)<0.001F,
        "an unsuccessful height probe restores the requested pitch for the original corridor fallback");
    published=publish(); published.r5=0; rocket_mod_camera_update(ram,&published);
    output_pitch=0.5F;
    prepare_frame();
    ctx.r2 = 3; // Original broad-phase filter's return value.
    rocket_mod_camera_candidates_begin(ram, &ctx);
    const float wanted_heading = read<float>(ram, controller + 0x10);
    const float original_eye = read<float>(ram, eye);
    ctx.r2 = 0; // Original candidate reached the blocked branch.
    const auto before_retry = ctx;
    check(rocket_mod_camera_retry(ram, &ctx) == 1,
        "a blocked orbit retries closer before the original code changes heading");
    check(std::abs(std::hypot(read<float>(ram, scratch), read<float>(ram, scratch + 4)) - radius * 0.75F) < 0.001F,
        "distance retry reduces the candidate radius");
    auto expected_retry = before_retry;
    expected_retry.r2 = 3;
    check(std::memcmp(&ctx, &expected_retry, sizeof(ctx)) == 0,
        "retry preserves guest state and restores the original filtered count");
    check(read<float>(ram, controller + 0x10) == wanted_heading && read<float>(ram, eye) == original_eye,
        "tight-space retries cannot flip the heading or write the published camera");
    const float fitted = std::hypot(read<float>(ram, scratch), read<float>(ram, scratch + 4));
    rocket_mod_camera_candidates_finish(ram, &ctx); // Original checks accepted this candidate.
    prepare_frame();
    check(std::abs(std::hypot(read<float>(ram, scratch), read<float>(ram, scratch + 4)) - fitted) < 0.001F,
        "a short hold prevents the camera pumping in a narrow passage");
    rocket_mod_camera_candidates_finish(ram, &ctx);
    // Advance in ordinary game steps: no clock discontinuity/reset.
    for (unsigned frame = 0; frame < 14; ++frame) {
        prepare_frame();
        rocket_mod_camera_candidates_finish(ram, &ctx);
    }
    const float previous_fit = std::hypot(read<float>(ram, scratch), read<float>(ram, scratch + 4));
    prepare_frame();
    const float outward_probe = std::hypot(read<float>(ram, scratch), read<float>(ram, scratch + 4));
    check(outward_probe > previous_fit && outward_probe - previous_fit < 0.07F,
        "outward recovery is limited to two world units per second");
    ctx.r2 = 3;
    rocket_mod_camera_candidates_begin(ram, &ctx);
    check(rocket_mod_camera_retry(ram, &ctx) == 1 &&
        std::abs(std::hypot(read<float>(ram, scratch), read<float>(ram, scratch + 4)) - previous_fit) < 0.001F,
        "a blocked outward probe returns to the last fit without an extra zoom-in step");
    unsigned retries = 0;
    while (rocket_mod_camera_retry(ram, &ctx)) ++retries;
    check(retries <= 6 && std::abs(std::hypot(read<float>(ram, scratch), read<float>(ram, scratch + 4)) - 2.0F) < 0.001F,
        "distance retries are bounded and leave impossible spaces to the original fallback");
    auto unrelated = ctx;
    unrelated.r29 += 0x1000;
    check(rocket_mod_camera_retry(ram, &unrelated) == 0, "other camera calls cannot enter the retry loop");

    published = publish();
    published.r5 = 0;
    rocket_mod_camera_update(ram, &published);
    write(ram, controller + 0x10, 0.0F);
    prepare_frame();
    write(ram, stack + 0x18, 10.0F + std::cos(0.05F) * 6.5F);
    write(ram, stack + 0x1C, 20.0F + std::sin(0.05F) * 6.5F);
    rocket_mod_camera_candidates_finish(ram, &ctx);
    write(ram, 0x8009F094, std::uint32_t{0x80201000});
    write(ram, camera + 0x1E4, std::uint32_t{0x80210000});
    constexpr std::uint32_t volume_eye = 0x80320000, volume_target = 0x80320020;
    auto volume_frame = [&] {
        clock += 1.0F / 30.0F;
        write(ram, 0x8009FE14, clock);
        write(ram, volume_target, 10.0F);
        write(ram, volume_target + 4, 20.0F);
        write(ram, volume_target + 8, 30.0F);
        write(ram, volume_eye, 3.5F); // Native volume wants the opposite side.
        write(ram, volume_eye + 4, 20.0F);
        write(ram, volume_eye + 8, 32.5F);
    };
    auto volume = published;
    volume.r5 = 0xF;
    volume.r6 = guest(volume_eye);
    volume.r7 = guest(volume_target);
    volume.r20 = guest(rail_controller);
    volume.r17 = guest(rail_volume);
    write(ram, rail_controller + 4, camera);
    write(ram, rail_controller + 8, rail_volume);
    auto update_volume = [&] {
        rocket_mod_camera_volume_update(ram, &volume);
        rocket_mod_camera_update(ram, &volume);
    };
    const auto volume_registers = volume;
    volume_frame();
    update_volume();
    check(owned && volume_solves == 1 && heap_depth == 0,
        "entering a camera volume keeps analogue input and balances temporary allocations");
    check(std::abs(last_yaw - 0.05F) < 0.001F && read<float>(ram, volume_eye) > 10,
        "entering a volume preserves the checked heading instead of jumping to its opposite default");
    check(std::memcmp(&volume, &volume_registers, sizeof(volume)) == 0 &&
        read<float>(ram, volume_target) == 10 && read<float>(ram, camera + 0x144) == 123,
        "volume adapter preserves caller registers, target and collision history");
    volume_frame();
    update_volume();
    check(owned && std::abs(last_yaw - 0.10F) < 0.001F,
        "successive volume frames continue the orbit despite recreated temporary controllers");
    write(ram, controller + 0x10, -2.0F);
    prepare_frame();
    check(std::abs(last_yaw - 0.15F) < 0.001F && owned,
        "leaving the volume carries its checked heading back to the normal controller");
    const int before_same_frame_volume = callbacks;
    update_volume();
    check(callbacks == before_same_frame_volume && owned,
        "two camera paths in one frame cannot integrate analogue input twice");
    volume_frame();
    write(ram, 0x8009F094, std::uint32_t{0});
    update_volume();
    check(!owned && read<float>(ram, volume_eye) == 3.5F && heap_depth == 0,
        "invalid world state cannot enter the temporary solver");
    write(ram, 0x8009F094, std::uint32_t{0x80201000});
    apply = false;
    volume_frame();
    update_volume();
    check(!owned && read<float>(ram, volume_eye) == 3.5F && heap_depth == 0,
        "a declined volume callback restores the untouched native view");
    apply = true;
    volume.r5 = 0xD;
    volume_frame();
    update_volume();
    check(owned && heap_depth == 0, "both colliding camera-volume modes support analogue input");
    // Reproduce a corridor in which every orbit candidate is rejected. The
    // original solver falls back to its previous eye at the entrance, but the
    // rail camera has a fresh moving position that must reach its own smoother.
    exhaust_volume = true;
    volume_frame();
    write(ram, camera, -40.0F);
    update_volume();
    check(owned && read<float>(ram, volume_eye) == 3.5F && heap_depth == 0,
        "an exhausted orbit uses the native corridor candidate instead of the stationary previous eye");
    volume_frame();
    write(ram, volume_eye, 5.5F);
    write(ram, volume_target, 12.0F);
    update_volume();
    check(owned && read<float>(ram, volume_eye) == 5.5F && read<float>(ram, volume_target) == 12.0F,
        "exhausted corridor camera continues following the moving native view on subsequent frames");
    auto region_check = ctx;
    region_check.r16 = guest(rail_volume);
    region_check.r2 = 1;
    rocket_mod_camera_volume_check(ram, &region_check);
    check(region_check.r2 == 1, "active-region exemption cannot escape its scoped corridor call");
    exhaust_volume = false;
    volume_frame();
    write(ram, rail_controller + 8, std::uint32_t{0});
    update_volume();
    check(!owned && read<float>(ram, volume_eye) == 3.5F,
        "mismatched rail-controller identity cannot exempt an arbitrary region");
    test_input.mouse_yaw=0.15F; test_input.mouse_pitch=-0.07F;
    prepare_frame();
    check(mouse_callbacks==1 && owned, "mouse look and the original camera callback run together");
    rocket_mod_camera_prepare(ram,&ctx);
    check(mouse_callbacks==1,"multiple camera paths in one frame cannot apply mouse input twice");
    prepare_frame();
    check(mouse_callbacks==1,"consumed mouse movement cannot repeat on the next guest frame");
    for (int zoom : {1, 2, 0}) {
        const float horizontal = zoom==0 ? 6.5F : zoom==1 ? 11.0F : 20.0F;
        const float height = zoom==0 ? 2.5F : zoom==1 ? 3.0F : 4.0F;
        write(ram,controller+0xC,zoom);
        write(ram,preset,horizontal); write(ram,preset+4,height);
        expected_radius = std::hypot(horizontal,height);
        prepare_frame();
        check(std::abs(std::hypot(read<float>(ram,scratch),read<float>(ram,scratch+4))-expected_radius)<0.001F,
            "changing native zoom levels starts the requested radius without the corridor recovery delay");
        check(read<float>(ram,preset)==horizontal && read<float>(ram,preset+4)==height,
            "cycling zoom leaves the original preset table intact");
    }
    constexpr std::uint32_t player = 0x80210000;
    write(ram,player+0x50C,std::uint32_t{1});
    write(ram,camera+0x1E0,camera+0x274);
    write(ram,player+0x2D8,0.7F); write(ram,player+0x2DC,-0.2F);
    write(ram,player+0x2BC,1.2F);
    auto first_person_context = initial;
    first_person_context.r4=guest(player);
    const auto saved_first_person_context=first_person_context;
    rocket_mod_camera_first_person(ram,&first_person_context);
    check(first_person_callbacks==0 && read<float>(ram,player+0x2D8)==0.7F,
        "older mods leave first-person input unchanged unless they opt in");
    check(claim_first_person!=nullptr,"first-person opt-in export is registered");
    claim_first_person(ram,&first_person_context);
    test_input.mouse_yaw=0.15F; test_input.mouse_pitch=-0.07F;
    rocket_mod_camera_first_person(ram,&first_person_context);
    check(first_person_callbacks==1 && mouse_callbacks==2 && first_person_reset && owned,
        "first-person mode accepts one modern look callback and one mouse sample");
    check(std::abs(first_person_yaw-0.7F)<0.001F && std::abs(first_person_pitch-0.2F)<0.001F &&
        std::abs(read<float>(ram,player+0x2D8)-0.9F)<0.001F && read<float>(ram,player+0x2DC)==-0.4F,
        "first-person horizontal turn reaches native yaw in the same direction as third-person orbit");
    check(std::memcmp(&first_person_context,&saved_first_person_context,sizeof(first_person_context))==0 &&
        read<float>(ram,camera+0x144)==123,
        "first-person look changes no caller registers or camera collision history");
    rocket_mod_camera_first_person(ram,&first_person_context);
    check(first_person_callbacks==1 && mouse_callbacks==2,"first-person look integrates only once per guest frame");
    publish();
    check(owned,"native first-person camera publication keeps modern input ownership");
    clock+=1.0F/30.0F; write(ram,0x8009FE14,clock);
    test_input.recenter=true;
    first_person_output_pitch=10;
    rocket_mod_camera_first_person(ram,&first_person_context);
    check(first_person_recenter && !first_person_reset && std::abs(first_person_yaw-1.2F)<0.001F && first_person_pitch==0.15F,
        "first-person recenter starts from Rocket's heading and original entry pitch");
    check(read<float>(ram,player+0x2DC)==-1.0F,"first-person pitch retains the game's original limit");
    test_input.recenter=false;
    const float saved_first_person_yaw=read<float>(ram,player+0x2D8);
    clock+=1.0F/30.0F; write(ram,0x8009FE14,clock); first_person_invalid=true;
    rocket_mod_camera_first_person(ram,&first_person_context);
    check(read<float>(ram,player+0x2D8)==saved_first_person_yaw,"invalid mod output cannot corrupt first-person angles");
    first_person_invalid=false; first_person_apply=false;
    rocket_mod_camera_first_person(ram,&first_person_context);
    check(read<float>(ram,player+0x2D8)==saved_first_person_yaw,"a declined first-person callback leaves the original input routine intact");
    first_person_apply=true;
    const int before_first_person_locked=first_person_callbacks;
    write(ram,player+0x3AC,std::uint32_t{4});
    rocket_mod_camera_first_person(ram,&first_person_context);
    check(first_person_callbacks==before_first_person_locked,"original locked player mode still prevents first-person look");
    write(ram,player+0x3AC,std::uint32_t{0});
    write(ram,camera+0x1E0,camera+0x318);
    rocket_mod_camera_first_person(ram,&first_person_context);
    check(first_person_callbacks==before_first_person_locked,"only the actual first-person controller can receive first-person events");
    write(ram,player+0x50C,std::uint32_t{0});
    prepare_frame();
    check(owned && last_reset,"leaving first person resets the orbit state and returns to normal collision-aware look");
    const int callbacks_before_off=callbacks;
    rocket::mods::request_camera_enabled(false);
    check(!rocket::input::camera_input_owned(),"live disable releases acquired input immediately");
    rocket_mod_camera_tick(ram,&tick);
    prepare_frame();
    check(!owned && !rocket::input::actions_active && callbacks==callbacks_before_off,
        "switching the mod off releases camera input and stops orbit callbacks");
    rocket::mods::request_camera_enabled(true);
    prepare_frame();
    check(owned && rocket::input::actions_active && last_reset && callbacks==callbacks_before_off+1,
        "switching back on starts from the current native view without a restart");
    write(ram,player+0x50C,std::uint32_t{1});
    write(ram,camera+0x1E0,camera+0x274);
    const float first_person_before_off=read<float>(ram,player+0x2D8);
    const int first_person_count_before_off=first_person_callbacks;
    rocket::mods::request_camera_enabled(false);
    rocket_mod_camera_first_person(ram,&first_person_context);
    check(!owned && first_person_callbacks==first_person_count_before_off &&
        read<float>(ram,player+0x2D8)==first_person_before_off,
        "switching off in first person preserves native angles and restores native input");
    rocket::mods::request_camera_enabled(true);
    rocket_mod_camera_first_person(ram,&first_person_context);
    check(owned && first_person_reset && first_person_callbacks==first_person_count_before_off+1,
        "first-person modern look can also be enabled during gameplay");
    rocket::mods::request_camera_enabled(false);
    rocket::mods::request_camera_enabled(true);
    rocket_mod_camera_tick(ram,&tick);
    check(rocket::input::camera_input_owned(),"rapid off/on before a guest update cannot leave input disabled");
    // Exercise the real library -> runtime handler -> guest boundary path, not
    // just a direct atomic request or a mock library callback in isolation.
    const auto temp = std::filesystem::temp_directory_path() /
        ("rocket-camera-switch-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto& lib = rocket::mods::library();
    lib.open(temp,"1.0.1");rocket::mods::configure_library();
    lib.install_bytes(rocket::generated::kCameraMod,".nrm");
    lib.set_enabled("rocket_modern_camera",false);
    rocket::mods::prepare_runtime();
    simulate_game_ready=true;rocket::mods::game_ready(ram,&ctx);simulate_game_ready=false;
    check(!rocket::mods::camera_status().enabled,"actual included package starts disabled in standby");
    lib.set_active_enabled("rocket_modern_camera",true);
    rocket_mod_camera_tick(ram,&tick);
    rocket_mod_camera_first_person(ram,&first_person_context);
    check(rocket::input::camera_input_owned(),"library UI action enables the resident camera through the runtime handler");
    const auto before_disabled=first_person_callbacks;
    lib.set_active_enabled("rocket_modern_camera",false);
    check(!rocket::input::camera_input_owned(),"library UI action immediately releases mouse/controller ownership");
    rocket_mod_camera_tick(ram,&tick);rocket_mod_camera_first_person(ram,&first_person_context);
    check(first_person_callbacks==before_disabled && !rocket::mods::camera_status().enabled,
        "library disable stops real callback dispatch and reports the applied state");
    lib.set_active_enabled("rocket_modern_camera",true);
    rocket_mod_camera_tick(ram,&tick);rocket_mod_camera_first_person(ram,&first_person_context);
    check(first_person_callbacks==before_disabled+1 && first_person_reset,"library re-enable resets the camera without reloading the mod");
    lib.finish_session();
    std::filesystem::remove_all(temp);
    std::printf("Passed %d camera bridge checks.\n", checks);
}
