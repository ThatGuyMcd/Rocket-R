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
constexpr float kEdgeGuard = 1.05F;
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
    g_expand_enabled.store(
        rocket::graphics::widescreen_active(4.0F / 3.0F),
        std::memory_order_release);
}

extern "C" void rocket_widescreen_frustum_begin(std::uint8_t* rdram,
                                                   recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return;

    // A prior call leaves the camera widened only while no guest code has
    // replaced those exact plane values. Restore that owned state before this
    // call so expansion is idempotent and a live switch back to 4:3 is clean.
    RestorePreviousFrustumIfOwned(rdram);

    // Original 4:3 is deliberately untouched. The SDL owner thread publishes
    // the selected aspect mode atomically, so this guest-thread hook never
    // races N64ModernRuntime's mutable GraphicsConfig object.
    if (!g_expand_enabled.load(std::memory_order_acquire)) return;

    const std::uint32_t camera_address = static_cast<std::uint32_t>(context->r4);
    if (camera_address < kRdramStart || camera_address > kRdramEnd - kCameraBytes) {
        return;
    }
    const gpr camera = GuestAddress(camera_address);

    const float authored_aspect = ReadFloat(rdram, camera, kAspectOffset);
    const float authored_fov_y = ReadFloat(rdram, camera, kFovYRadiansOffset);
    const float fov_y = rocket::graphics::effective_fov_radians(authored_fov_y);
    if (!std::isfinite(authored_aspect) || authored_aspect <= 0.1F ||
        !std::isfinite(fov_y) || fov_y <= 0.01F || fov_y >= kPi - 0.01F) {
        return;
    }

    const float window_aspect = rocket::graphics::selected_aspect(4.0F / 3.0F);
    if (!std::isfinite(window_aspect) ||
        window_aspect <= authored_aspect * 1.0001F) {
        return;
    }

    Vec3 forward{};
    if (!Normalize(ReadVec3(rdram, camera, kForwardOffset), forward)) return;

    std::array<Vec3, 4> planes{};
    std::array<Vec3, 4> unit_planes{};
    std::array<float, 4> plane_lengths{};
    std::array<float, 4> forward_scores{};
    std::array<std::size_t, 4> order{0U, 1U, 2U, 3U};
    for (std::size_t index = 0; index < planes.size(); ++index) {
        planes[index] = ReadVec3(
            rdram, camera, kSidePlaneOffset + static_cast<int>(index) * 12);
        if (!Normalize(planes[index], unit_planes[index], &plane_lengths[index])) {
            return;
        }
        // For a perspective frustum, horizontal side planes have the larger
        // absolute forward component when the authored aspect is wider than
        // 1:1. Selecting them by geometry avoids depending on plane ordering.
        forward_scores[index] = std::fabs(Dot(unit_planes[index], forward));
    }
    std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        return forward_scores[left] > forward_scores[right];
    });
    g_frustum_scope.saved = planes;
    g_frustum_scope.widened = planes;
    g_frustum_scope.camera_address = camera_address;

    // Match RT64's Expand-to-window horizontal FOV and keep a small DKR-R-style
    // guard beyond the visible edge. The guard prevents a model whose bounding
    // sphere sits exactly on the widened plane from flickering as the camera or
    // interpolated presentation moves by a fraction of a pixel.
    const float target_aspect = std::max(authored_aspect, window_aspect) * kEdgeGuard;
    const float vertical_half_fov = fov_y * 0.5F;
    const float tangent = std::tan(vertical_half_fov);
    if (!std::isfinite(tangent) || tangent <= 0.0F) return;
    const float desired_horizontal_half_fov = std::atan(tangent * target_aspect);
    const float desired_forward = std::sin(desired_horizontal_half_fov);
    const float desired_transverse = std::cos(desired_horizontal_half_fov);

    for (std::size_t pair_index = 0; pair_index < 2U; ++pair_index) {
        const std::size_t index = order[pair_index];
        const Vec3 current = unit_planes[index];
        const float forward_component = Dot(current, forward);
        const Vec3 transverse_raw = Add(current, Scale(forward, -forward_component));
        Vec3 transverse{};
        if (!Normalize(transverse_raw, transverse)) continue;

        const float signed_forward = std::copysign(desired_forward, forward_component);
        const Vec3 widened_unit = Add(Scale(transverse, desired_transverse),
                                     Scale(forward, signed_forward));
        const Vec3 widened = Scale(widened_unit, plane_lengths[index]);
        WriteVec3(rdram, camera,
                  kSidePlaneOffset + static_cast<int>(index) * 12, widened);
        g_frustum_scope.widened[index] = widened;
    }
    g_frustum_scope.active = true;

    if (!g_logged_expansion.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[widescreen] CPU object frustum expanded %.3f -> %.3f "
                     "(+5%% edge guard)\n",
                     authored_aspect, window_aspect);
    }
}
