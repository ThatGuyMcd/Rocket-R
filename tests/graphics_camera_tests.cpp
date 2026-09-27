#include "graphics_enhancements.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace {
constexpr std::uint32_t camera = 0x80200000U;
constexpr std::size_t base = camera & 0x7FFFFFFFU;
constexpr float radians = 3.14159265358979323846F / 180.0F;
int checks = 0;
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
    ++checks;
}
void write(std::uint8_t* ram, unsigned offset, float value) {
    std::memcpy(ram + base + offset, &value, sizeof(value));
}
float read(const std::uint8_t* ram, unsigned offset) {
    float value;
    std::memcpy(&value, ram + base + offset, sizeof(value));
    return value;
}
bool near(float a, float b) { return std::abs(a - b) < 0.00001F; }
void initialize(std::uint8_t* ram, float fov = 45.0F, float aspect = 4.0F / 3.0F) {
    for (unsigned i = 0; i < 16; ++i) write(ram, 0x30 + i * 4, i % 5 == 0 ? 1.0F : 0.0F);
    write(ram, 0xA0, fov * radians);
    write(ram, 0xA4, aspect);
    write(ram, 0xA8, 1.0F);
    write(ram, 0xAC, 200.0F);
}
struct Extents { float x, y, z; };
Extents rectangle(float fov, float aspect, float distance) {
    const float height = std::tan(fov * 0.5F) * distance;
    return {height * aspect, height, distance};
}
bool behind_wall(const Extents& p, const std::array<float, 4>& plane) {
    // All four corners of the near-plane rectangle stay on the camera side.
    for (float x : {-p.x, p.x}) for (float y : {-p.y, p.y}) {
        if (x * plane[0] + y * plane[1] + p.z * plane[2] >= plane[3]) return false;
    }
    return true;
}
}

extern "C" void rocket_presentation_camera_source(std::uint8_t*, recomp_context*, std::uint32_t) {}

int main() {
    using namespace rocket::graphics;
    std::vector<std::uint8_t> memory(8 * 1024 * 1024);
    auto* ram = memory.data();
    recomp_context context{};
    context.r4 = static_cast<gpr>(static_cast<std::int32_t>(camera));
    const auto original_context = context;
    initialize(ram);
    Settings options{};
    set_settings(options);
    const auto retail = memory;
    rocket_graphics_camera_begin(ram, &context);
    check(memory == retail, "original FOV and 4:3 remain bit-identical");
    rocket_graphics_camera_end(ram, &context);

    options.fov_offset_degrees = 40.0F;
    options.aspect = AspectPreset::Ratio16x9;
    set_settings(options);
    rocket_graphics_camera_begin(ram, &context);
    const float corrected_near = read(ram, 0xA8);
    const auto original = rectangle(45.0F * radians, 4.0F / 3.0F, 1.0F);
    const auto broken = rectangle(85.0F * radians, 16.0F / 9.0F, 1.0F);
    const auto corrected = rectangle(read(ram, 0xA0), 16.0F / 9.0F, corrected_near);
    check(corrected_near > 0.3F && corrected_near < 0.4F, "wide view uses a finite, useful near distance");
    check(corrected.x <= original.x + 0.00001F && corrected.y <= original.y,
          "85-degree widescreen near rectangle fits the original half-extents");
    check(read(ram, 0xA4) == 4.0F / 3.0F && read(ram, 0xAC) == 200.0F,
          "RT64 aspect expansion is not applied twice and far clip remains intact");
    for (const auto& wall : std::array<std::array<float, 4>, 4>{{
            {1, 0, 0, 0.6F}, {1, 0, 0.1F, 0.75F},
            {0, 1, 0.1F, 0.65F}, {1, 1, 0.1F, 1.1F}}}) {
        check(behind_wall(original, wall) && !behind_wall(broken, wall) && behind_wall(corrected, wall),
              "close wall/ceiling/corner clipped by the wider plane stays in front of the corrected plane");
    }
    rocket_graphics_camera_end(ram, &context);
    check(memory == retail, "projection return restores all guest camera fields without needing an object test");
    check(std::memcmp(&context, &original_context, sizeof(context)) == 0, "projection hooks preserve guest registers");

    for (float authored : {25.0F, 45.0F, 60.0F, 85.0F, 100.0F}) {
        for (float offset : {-20.0F, 0.0F, 10.0F, 40.0F}) {
            for (float aspect : {4.0F / 3.0F, 16.0F / 10.0F, 16.0F / 9.0F, 21.0F / 9.0F, 4.0F}) {
                initialize(ram, authored);
                options.aspect = AspectPreset::Custom;
                options.custom_aspect = aspect;
                options.fov_offset_degrees = offset;
                set_settings(options);
                rocket_graphics_camera_begin(ram, &context);
                const auto before = rectangle(authored * radians, 4.0F / 3.0F, 1.0F);
                const auto after = rectangle(read(ram, 0xA0), aspect, read(ram, 0xA8));
                check(after.x <= before.x + 0.00001F && after.y <= before.y + 0.00001F &&
                      after.z > 0.0F && after.z <= 1.0F,
                      "FOV/aspect sweep preserves both near-plane extents without increasing the near distance");
                rocket_graphics_camera_end(ram, &context);
                check(read(ram, 0xA0) == authored * radians && read(ram, 0xA8) == 1.0F,
                      "every projection restores its authored values");
            }
        }
    }

    initialize(ram);
    options.aspect = AspectPreset::FitWindow;
    options.fov_offset_degrees = 40.0F;
    options.draw_distance_multiplier = 2.0F;
    set_settings(options);
    set_window_aspect(16.0F / 9.0F);
    rocket_graphics_camera_begin(ram, &context);
    check(near(read(ram, 0xA8), corrected_near) && read(ram, 0xAC) == 400.0F,
          "fit-window aspect and extended far clip work together");
    rocket_graphics_camera_begin(ram, &context);
    check(near(read(ram, 0xA8), corrected_near) && read(ram, 0xAC) == 400.0F,
          "repeated begin cannot accumulate near or far scaling");
    rocket_graphics_camera_end(ram, &context);
    set_window_aspect(21.0F / 9.0F);
    rocket_graphics_camera_begin(ram, &context);
    check(read(ram, 0xA8) < corrected_near, "resizing a fit-window view refreshes near clearance");
    rocket_graphics_camera_end(ram, &context);
    options.fov_offset_degrees = 0;
    options.aspect = AspectPreset::Original4x3;
    options.draw_distance_multiplier = 1;
    set_settings(options);
    rocket_graphics_camera_begin(ram, &context);
    check(memory == retail, "switching back to original graphics restores retail projection immediately");
    rocket_graphics_camera_end(ram, &context);

    options.fov_offset_degrees = 40;
    set_settings(options);
    rocket_graphics_camera_begin(ram, &context);
    write(ram, 0xA8, 0.25F);
    write(ram, 0xA0, 60.0F * radians);
    rocket_graphics_camera_end(ram, &context);
    check(read(ram, 0xA8) == 0.25F && read(ram, 0xA0) == 60.0F * radians,
          "restoration respects values subsequently changed by the game");
    for (float invalid : {0.0F, -1.0F, std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN()}) {
        initialize(ram);
        write(ram, 0xA8, invalid);
        rocket_graphics_camera_begin(ram, &context);
        check(std::bit_cast<std::uint32_t>(read(ram, 0xA8)) == std::bit_cast<std::uint32_t>(invalid),
              "invalid authored near planes are left untouched");
        rocket_graphics_camera_end(ram, &context);
    }
    initialize(ram);
    context.r4 = 0x80800000U;
    rocket_graphics_camera_begin(ram, &context);
    rocket_graphics_camera_end(ram, &context);
    check(memory == retail, "invalid guest pointer cannot write camera fields");
    for (float amount : {0.0F, 0.25F, 0.5F, 1.0F}) {
        options.sky_dither_reduction = amount;
        const auto previous_revision = revision();
        set_settings(options);
        check(settings().sky_dither_reduction == amount && revision() > previous_revision,
              "sky reduction must survive normalization and publish live changes");
    }
    options.sky_dither_reduction = 2.0F;
    set_settings(options);
    check(settings().sky_dither_reduction == 1.0F, "sky reduction clamps high values");
    for (float invalid : {-1.0F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        options.sky_dither_reduction = invalid;
        set_settings(options);
        check(settings().sky_dither_reduction == 0.0F, "invalid sky reduction restores original");
    }
    reset_settings();
    check(settings().sky_dither_reduction == 0.0F, "reset must restore the original sky");
    std::printf("Passed %d graphics camera checks.\n", checks);
}
