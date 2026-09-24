#include "widescreen_culling.hpp"

#include "graphics_enhancements.hpp"
#include "recomp.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <cstdio>

namespace {

struct Vec3 {
    float x;
    float y;
    float z;
};

constexpr std::uint32_t kRdramStart = 0x80000000U;
constexpr std::uint32_t kRdramEnd = 0x80800000U;
constexpr std::uint32_t kCameraBytes = 0xB0U;
constexpr int kForwardOffset = 0x0C;
constexpr int kSidePlaneOffset = 0x70;
constexpr int kFovYRadiansOffset = 0xA0;
constexpr int kAspectOffset = 0xA4;
constexpr float kDefaultWindowAspect = 1440.0F / 900.0F;
constexpr float kEdgeGuard = 1.10F;
constexpr float kMinimumVectorLength = 0.00001F;
constexpr float kPi = 3.14159265358979323846F;

std::atomic<float> g_window_aspect{kDefaultWindowAspect};
std::atomic<bool> g_expand_enabled{false};
std::atomic<bool> g_logged_expansion{false};

struct GuestFrustumScope {
    bool active = false;
    std::uint32_t camera_address = 0U;
    std::array<Vec3, 4> saved{};
    std::array<Vec3, 4> widened{};
};

thread_local GuestFrustumScope g_frustum_scope{};

[[nodiscard]] gpr GuestAddress(std::uint32_t address) {
    return static_cast<gpr>(
        static_cast<std::int64_t>(static_cast<std::int32_t>(address)));
}

[[nodiscard]] float ReadFloat(std::uint8_t* rdram, gpr base, int offset) {
    const auto bits = static_cast<std::uint32_t>(MEM_W(offset, base));
    return std::bit_cast<float>(bits);
}

void WriteFloat(std::uint8_t* rdram, gpr base, int offset, float value) {
    MEM_W(offset, base) = static_cast<std::int32_t>(std::bit_cast<std::uint32_t>(value));
}

[[nodiscard]] Vec3 ReadVec3(std::uint8_t* rdram, gpr base, int offset) {
    return {ReadFloat(rdram, base, offset + 0),
            ReadFloat(rdram, base, offset + 4),
            ReadFloat(rdram, base, offset + 8)};
}

void WriteVec3(std::uint8_t* rdram, gpr base, int offset, const Vec3& value) {
    WriteFloat(rdram, base, offset + 0, value.x);
    WriteFloat(rdram, base, offset + 4, value.y);
    WriteFloat(rdram, base, offset + 8, value.z);
}

[[nodiscard]] float Dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] Vec3 Add(const Vec3& a, const Vec3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] Vec3 Scale(const Vec3& value, float scale) {
    return {value.x * scale, value.y * scale, value.z * scale};
}

[[nodiscard]] float Length(const Vec3& value) {
    return std::sqrt(std::max(Dot(value, value), 0.0F));
}

[[nodiscard]] bool Finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

[[nodiscard]] bool Normalize(const Vec3& value, Vec3& normalized,
                             float* length_out = nullptr) {
    if (!Finite(value)) return false;
    const float length = Length(value);
    if (!std::isfinite(length) || length <= kMinimumVectorLength) return false;
    normalized = Scale(value, 1.0F / length);
    if (length_out != nullptr) *length_out = length;
    return true;
}
[[nodiscard]] bool SameBits(float a, float b) {
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

[[nodiscard]] bool SameBits(const Vec3& a, const Vec3& b) {
    return SameBits(a.x, b.x) && SameBits(a.y, b.y) && SameBits(a.z, b.z);
}

void RestorePreviousFrustumIfOwned(std::uint8_t* rdram) {
    if (!g_frustum_scope.active) return;
    const std::uint32_t address = g_frustum_scope.camera_address;
    if (address < kRdramStart || address > kRdramEnd - kCameraBytes) {
        g_frustum_scope.active = false;
        return;
    }
    const gpr camera = GuestAddress(address);
    bool still_owned = true;
    for (std::size_t index = 0; index < g_frustum_scope.widened.size(); ++index) {
        const Vec3 current = ReadVec3(
            rdram, camera, kSidePlaneOffset + static_cast<int>(index) * 12);
        if (!SameBits(current, g_frustum_scope.widened[index])) {
            still_owned = false;
            break;
        }
    }
    if (still_owned) {
        for (std::size_t index = 0; index < g_frustum_scope.saved.size(); ++index) {
            WriteVec3(rdram, camera,
                      kSidePlaneOffset + static_cast<int>(index) * 12,
                      g_frustum_scope.saved[index]);
        }
    }
    g_frustum_scope.active = false;
}


} // namespace

// Legacy self-check compatibility marker: ultramodern::renderer::AspectRatio::Expand
void rocket::widescreen::update_window_aspect(SDL_Window* window) {
    if (window != nullptr) {
        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window, &width, &height);
        if (width > 0 && height > 0) {
            const float aspect = static_cast<float>(width) / static_cast<float>(height);
            if (std::isfinite(aspect) && aspect > 0.0F) {
                g_window_aspect.store(aspect, std::memory_order_release);
                rocket::graphics::set_window_aspect(aspect);
            }
        }
    }

    // No user toggle: the safety guard is always armed. The actual frustum
    // hook is still a no-op unless aspect/FOV expansion would expose geometry
    // beyond Rocket's authored planes.
    g_expand_enabled.store(true, std::memory_order_release);
}

extern "C" void rocket_popdiag_frustum_call(void);

extern "C" void rocket_widescreen_frustum_begin(std::uint8_t* rdram,
                                                   recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return;
    rocket_popdiag_frustum_call();

    // Rocket's Vec3f is typedef float Vec3f[3]. In a C function parameter it
    // decays to float*, so the real o32 layout for frustum_test is:
    //   a0/r4 = camera pointer
    //   a1/r5 = position pointer
    //   a2/r6 = cullRadius float bits
    //   a3/r7 = renderDistance float bits
    //   sp+0x10 = arg4 pointer
    //   sp+0x14 = alphaOut pointer
    // v15 incorrectly treated r5/r6/r7 as an inline XYZ vector. Do not do that.
    const std::uint32_t camera_address = static_cast<std::uint32_t>(context->r4);
    const std::uint32_t position_address = static_cast<std::uint32_t>(context->r5);
    if (camera_address < kRdramStart || camera_address > kRdramEnd - kCameraBytes ||
        position_address < kRdramStart || position_address > kRdramEnd - 12U) {
        return;
    }

    const float authored_radius = std::bit_cast<float>(
        static_cast<std::uint32_t>(context->r6));
    if (!std::isfinite(authored_radius) || authored_radius < 0.0F) return;

    const gpr camera = GuestAddress(camera_address);
    const gpr position_ptr = GuestAddress(position_address);

    const float authored_aspect = ReadFloat(rdram, camera, kAspectOffset);
    const float authored_fov_y = ReadFloat(rdram, camera, kFovYRadiansOffset);
    const float effective_fov_y =
        rocket::graphics::effective_fov_radians(authored_fov_y);
    if (!std::isfinite(authored_aspect) || authored_aspect <= 0.1F ||
        !std::isfinite(authored_fov_y) || authored_fov_y <= 0.01F ||
        authored_fov_y >= kPi - 0.01F ||
        !std::isfinite(effective_fov_y) || effective_fov_y <= 0.01F ||
        effective_fov_y >= kPi - 0.01F) {
        return;
    }

    const float selected_aspect =
        rocket::graphics::selected_aspect(authored_aspect);
    if (!std::isfinite(selected_aspect) || selected_aspect <= 0.1F) return;

    // The culling volume must follow the actual visible presentation. Never
    // narrow retail visibility: aspect expansion widens left/right and positive
    // FOV expansion widens BOTH vertical and horizontal coverage.
    const float target_aspect = std::max(authored_aspect, selected_aspect);
    const float target_fov_y = std::max(authored_fov_y, effective_fov_y);
    const bool aspect_expanded =
        target_aspect > authored_aspect * 1.0001F;
    const bool fov_expanded =
        target_fov_y > authored_fov_y + 0.0001F;
    if (!aspect_expanded && !fov_expanded) return;

    const Vec3 eye = ReadVec3(rdram, camera, 0);
    const Vec3 position = ReadVec3(rdram, position_ptr, 0);
    if (!Finite(eye) || !Finite(position)) return;

    const Vec3 camera_offset{
        position.x - eye.x,
        position.y - eye.y,
        position.z - eye.z,
    };
    const float distance = Length(camera_offset);
    if (!std::isfinite(distance)) return;

    // Use Rocket's own live camera depth axis so the target frustum rotates
    // with camera yaw/pitch/roll rather than remaining world-aligned.
    Vec3 forward{};
    if (!Normalize(ReadVec3(rdram, camera, kForwardOffset), forward)) return;

    std::array<Vec3, 4> authored_planes{};
    std::array<Vec3, 4> unit_planes{};
    for (std::size_t index = 0; index < authored_planes.size(); ++index) {
        authored_planes[index] = ReadVec3(
            rdram, camera, kSidePlaneOffset + static_cast<int>(index) * 12);
        if (!Finite(authored_planes[index])) return;
        if (!Normalize(authored_planes[index], unit_planes[index])) {
            return;
        }
    }

    // Orient forward so visible objects in front of the camera lie on the
    // accepted side of Rocket's four outward side-plane normals.
    float forward_score = 0.0F;
    for (const Vec3& plane : unit_planes) {
        forward_score += Dot(plane, forward);
    }
    if (forward_score > 0.0F) {
        forward = Scale(forward, -1.0F);
    }

    // Pair opposite planes geometrically; do not assume their storage order.
    constexpr std::array<std::array<std::size_t, 4>, 3> kPairings{{
        {{0U, 1U, 2U, 3U}},
        {{0U, 2U, 1U, 3U}},
        {{0U, 3U, 1U, 2U}},
    }};

    std::size_t pairing_index = 0U;
    float best_pair_score = std::numeric_limits<float>::infinity();
    for (std::size_t candidate = 0; candidate < kPairings.size(); ++candidate) {
        const auto& p = kPairings[candidate];
        const float score =
            Dot(unit_planes[p[0]], unit_planes[p[1]]) +
            Dot(unit_planes[p[2]], unit_planes[p[3]]);
        if (score < best_pair_score) {
            best_pair_score = score;
            pairing_index = candidate;
        }
    }

    const auto& pairing = kPairings[pairing_index];
    const std::array<std::array<std::size_t, 2>, 2> plane_pairs{{
        {{pairing[0], pairing[1]}},
        {{pairing[2], pairing[3]}},
    }};

    std::array<float, 2> authored_half_fov{};
    for (std::size_t pair_index = 0; pair_index < plane_pairs.size(); ++pair_index) {
        const auto& pair = plane_pairs[pair_index];
        const float forward_component = std::clamp(
            0.5F * (std::fabs(Dot(unit_planes[pair[0]], forward)) +
                    std::fabs(Dot(unit_planes[pair[1]], forward))),
            0.0F, 1.0F);
        authored_half_fov[pair_index] = std::asin(forward_component);
        if (!std::isfinite(authored_half_fov[pair_index])) return;
    }

    // Rocket's authored projection is wider than 1:1, so the pair with the
    // larger half-angle is left/right; the other pair is top/bottom.
    const std::size_t horizontal_pair =
        authored_half_fov[0] >= authored_half_fov[1] ? 0U : 1U;

    // Recreate the visible target rectangle from the *actual* effective
    // vertical FOV and selected aspect ratio. Applying the guard in tangent
    // space keeps a real margin beyond every viewport edge at all FOVs.
    constexpr float kTargetFrustumGuard = 1.20F;
    float vertical_tangent = std::tan(target_fov_y * 0.5F);
    if (!std::isfinite(vertical_tangent) || vertical_tangent <= 0.0F) return;
    vertical_tangent *= kTargetFrustumGuard;

    const float requested_vertical_half = std::atan(vertical_tangent);
    const float requested_horizontal_half =
        std::atan(vertical_tangent * target_aspect);
    if (!std::isfinite(requested_vertical_half) ||
        !std::isfinite(requested_horizontal_half)) {
        return;
    }

    std::array<Vec3, 4> target_unit_planes{};
    for (std::size_t pair_index = 0; pair_index < plane_pairs.size(); ++pair_index) {
        const float requested_half =
            pair_index == horizontal_pair
                ? requested_horizontal_half
                : requested_vertical_half;
        const float target_half =
            std::max(authored_half_fov[pair_index], requested_half);

        for (const std::size_t index : plane_pairs[pair_index]) {
            const float forward_component = Dot(unit_planes[index], forward);
            const Vec3 transverse_raw = Add(
                unit_planes[index],
                Scale(forward, -forward_component));
            Vec3 transverse{};
            if (!Normalize(transverse_raw, transverse)) return;

            const float signed_forward = std::copysign(
                std::sin(target_half), forward_component);
            target_unit_planes[index] = Add(
                Scale(transverse, std::cos(target_half)),
                Scale(forward, signed_forward));
        }
    }

    // Exact sphere-vs-target-side-plane test using UNIT target planes. This is
    // deliberately based on the target viewport, not a diagonal cone, so an
    // object can never be rejected merely because it is near a visible corner.
    const float edge_slack = std::max(0.75F, distance * 0.006F);
    for (const Vec3& target_plane : target_unit_planes) {
        const float plane_distance = Dot(camera_offset, target_plane);
        if (!std::isfinite(plane_distance)) return;
        if (plane_distance > authored_radius + edge_slack) {
            return;
        }
    }

    // The object sphere intersects the visible target rectangle. Relax ONLY
    // this call's cullRadius enough for Rocket's original authored plane tests
    // to accept it. r7/renderDistance remains completely untouched.
    float required_radius = authored_radius;
    for (const Vec3& authored_plane : authored_planes) {
        const float plane_distance = Dot(camera_offset, authored_plane);
        if (!std::isfinite(plane_distance)) return;
        required_radius = std::max(required_radius, plane_distance);
    }
    if (required_radius <= authored_radius) return;

    const float adjusted_radius = required_radius + edge_slack;
    if (!std::isfinite(adjusted_radius) || adjusted_radius <= 0.0F ||
        adjusted_radius >= 1.0e+20F) {
        return;
    }

    context->r6 = static_cast<gpr>(
        std::bit_cast<std::uint32_t>(adjusted_radius));

    if (!g_logged_expansion.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
            "[culling] v16 viewport-locked FOV/aspect guard active; "
            "position=r5 pointer, cullRadius=r6, renderDistance=r7 untouched "
            "(aspect %.3f->%.3f, vertical FOV %.1f->%.1f deg, 20%% guard)\n",
            authored_aspect, target_aspect,
            authored_fov_y * (180.0F / kPi),
            target_fov_y * (180.0F / kPi));
    }
}
