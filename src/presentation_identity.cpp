#include "presentation_identity.hpp"
#include "sky_presentation.hpp"
#include "graphics_enhancements.hpp"

#include "recomp.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <iterator>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// === ROCKET-R INTERPOLATION V35 SPECIFIC SUBMODEL MATRICES + RANGE-STABLE SKY ===
// === ROCKET-R INTERPOLATION V36 GLOBAL MATRIX OWNERSHIP ===
// Coverage-first extension: exact model allocation ranges, every non-RenderEntry
// CPU matrix producer, and camera-owned matrices. Existing v35/v5/v6 matching
// remains intact and exact v35 Submodel ownership keeps highest priority.
// Additive coverage only: the proven v5/v6 RenderEntry matcher remains authoritative
// for all existing draws. v35 overlays an identity only when Rocket itself proves
// the exact GameObject/Submodel that authored a matrix. Unknowns still fail closed.

constexpr std::uint32_t kRdramMask = 0x007FFFFFU;
constexpr std::uint32_t kRdramStart = 0x80000000U;
constexpr std::uint32_t kRdramEnd = 0x807FFFFFU;
constexpr std::uint32_t kCurGfxTaskAddress = 0x800A5DBCU;
constexpr std::uint32_t kGfxTaskCtxSizeOffset = 0x004U;
constexpr std::uint32_t kGfxTaskCtxDlStartOffset = 0x008U;
constexpr std::uint32_t kGfxTaskDlStartOffset = 0x014U;
constexpr std::uint32_t kGfxContextDlHeadAddress = 0x800A5DB0U; // v33 sky/background command-range capture
constexpr std::uint32_t kMtxBytes = 0x40U;
constexpr std::uint32_t kSubmodelBytes = 0x28U;
constexpr std::uint32_t kGameObjectClassOffset = 0x000U;
constexpr std::uint32_t kGameObjectMaterialCallbackOffset = 0x06CU;
constexpr std::uint32_t kCollectibleMaterialCallback = 0x8006BDF0U;
constexpr std::uint32_t kSubmodelPresentationModeOffset = 0x20U;
constexpr std::uint8_t kCameraRelativeSubmodelModeMin = 1U;
constexpr std::uint8_t kCameraRelativeSubmodelModeMax = 5U;
constexpr std::uint32_t kGameObjectSubmodelsOffset = 0x0F4U;
constexpr std::uint32_t kGameObjectSubmodelCountOffset = 0x0F8U;
constexpr std::uint32_t kGameObjectPositionOffset = 0x03CU;
constexpr std::uint32_t kGfxContextMtxHeadAddress = 0x800A5DB4U;
constexpr std::uint32_t kGfxTaskPerspectiveMtxOffset = 0x018U;
constexpr std::uint32_t kGfxTaskViewMtxOffset = 0x058U;
constexpr std::uint32_t kGfxTaskIdentityModelMtxOffset = 0x098U;
constexpr std::size_t kMaximumModelRangeMatrices = 256U;
constexpr std::size_t kMaximumDirectRangeMatrices = 8U;
constexpr std::uint64_t kMaximumTrackAge = 1U;
constexpr std::size_t kMaximumPendingTasks = 8U;
// Rocket v4.2 allowed a previous owner to be selected from as far as 768 world
// units away and from up to four authored frames ago. That is too permissive
// for repeated scenery/render-entry keys: one bad association is enough for an
// interpolated transform to throw a triangle through the camera plane. v5 only
// carries identity across adjacent authored frames and refuses large jumps.
constexpr float kMaximumTrackDistance = 384.0F;
constexpr float kOrdinalPenalty = 64.0F;
constexpr float kAmbiguityAbsoluteMargin = 64.0F;
constexpr float kAmbiguityRelativeMargin = 0.20F;
constexpr float kPositionEpsilon = 0.0001F;

struct Vec3 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

struct RecordedEntry {
    std::uint64_t key = 0U;
    std::uint32_t gfx = 0U;
    std::uint32_t mtx1 = 0U;
    std::uint32_t mtx2 = 0U;
    std::uint32_t callsite = 0U;
    std::uint32_t ordinal = 0U;
    std::uint8_t alpha = 0xFFU;
    Vec3 position{};
    bool position_valid = false;
    Vec3 mtx1_position{};
    Vec3 mtx2_position{};
    bool mtx1_position_valid = false;
    bool mtx2_position_valid = false;
    bool dynamic_gfx = false;
    std::uint32_t track_token = 0U;
};

struct Track {
    std::uint64_t key = 0U;
    std::uint32_t token = 0U;
    std::uint32_t ordinal = 0U;
    Vec3 position{};
    Vec3 velocity{};
    bool position_valid = false;
    std::uint64_t last_frame = 0U;
    bool claimed = false;
};

struct Candidate {
    std::size_t entry = 0U;
    std::size_t track = 0U;
    float cost = 0.0F;
    bool operator<(const Candidate& rhs) const { return cost < rhs.cost; }
};

struct SharedMatrixSample {
    std::uint64_t signature = 0U;
    std::uint32_t address = 0U;
    std::uint32_t physical = 0U;
    std::uint32_t role_mask = 0U;
    Vec3 position{};
    bool position_valid = false;
    bool dynamic_gfx = false;
    std::uint32_t track_token = 0U;
};

struct SharedMatrixTrack {
    std::uint64_t signature = 0U;
    std::uint32_t token = 0U;
    Vec3 position{};
    bool position_valid = false;
    std::uint64_t last_frame = 0U;
    bool claimed = false;
};

struct SharedCandidate {
    std::size_t sample = 0U;
    std::size_t track = 0U;
    float cost = 0.0F;
};

struct ModelRangeCapture {
    std::uint32_t start_head = 0U;
    std::uint32_t object = 0U;
    std::uint32_t caller = 0U;
    std::size_t specific_begin = 0U;
    bool valid = false;
};

struct ModelRangeSample {
    std::uint32_t address = 0U;
    std::uint32_t identity = 0U;
};

struct ObjectLifetimeState {
    std::uint32_t token = 0U;
    std::uint64_t fingerprint = 0U;
    Vec3 position{};
    bool position_valid = false;
    std::uint64_t last_frame = 0U;
};

struct DirectCapture {
    std::uint32_t start_head = 0U;
    std::uint32_t caller = 0U;
    std::uint32_t owner = 0U;
    bool valid = false;
};

struct DirectMatrixSample {
    std::uint64_t key = 0U;
    std::uint32_t address = 0U;
    std::uint32_t kind = 0U;
    std::uint32_t role = 0U;
    Vec3 position{};
    bool position_valid = false;
    std::uint32_t track_token = 0U;
};

struct DirectContinuity {
    std::uint64_t key = 0U;
    std::uint32_t token = 0U;
    Vec3 position{};
    bool position_valid = false;
    std::uint64_t last_frame = 0U;
    bool claimed = false;
};

struct DirectCandidate {
    std::size_t sample = 0U;
    std::size_t track = 0U;
    float cost = 0.0F;
};

struct CameraMatrixSample {
    std::uint32_t address = 0U;
    std::uint32_t identity = 0U;
};

struct CameraContinuityState {
    std::uint32_t camera = 0U;
    std::uint32_t token = 0U;
    std::uint64_t last_frame = 0U;
};

struct SpecificMatrixSample {
    std::uint64_t key = 0U;
    std::uint32_t address = 0U;
    std::uint32_t owner = 0U;
    bool rigid_decompose = false;
    bool interpolate_shape = false;
    bool force_snap = false;
    bool secondary_transform = false;
    bool collectible_billboard = false;
    std::uint8_t submodel_mode = 0U;
    Vec3 position{};
    bool position_valid = false;
    std::uint32_t track_token = 0U;
};

struct SpecificContinuity {
    std::uint64_t key = 0U;
    std::uint32_t token = 0U;
    Vec3 position{};
    bool position_valid = false;
    std::uint64_t last_frame = 0U;
    bool claimed = false;
};

struct SubmodelMatrixCapture {
    std::uint64_t key = 0U;
    std::uint32_t owner = 0U;
    bool rigid_decompose = false;
    bool collectible_billboard = false;
    std::uint8_t submodel_mode = 0U;
    bool valid = false;
    Vec3 instance_position{};
    bool instance_position_valid = false;
    bool projected_shadow = false;
};

struct BindingRecord {
    rocket::presentation::MatrixBinding binding{};
};

using MatrixMap = std::unordered_map<std::uint32_t, BindingRecord>;

// ROCKET-R SKYBOX INTERPOLATION V33
struct BackgroundCommandRange {
    std::uint32_t begin = 0U;
    std::uint32_t end = 0U;
    bool scrolling_rectangle = false;
    float current_row_offset = 0;
    float previous_row_offset = 0;
};

struct SkyCapture {
    rocket::presentation::SkyRows rows{};
    std::uint32_t texture = 0, camera = 0;
    std::uint64_t frame = 0;
    bool valid = false;
};
SkyCapture g_sky_capture{}, g_previous_sky{};
std::uint32_t g_trace_wheel_matrix = 0;

struct SubmittedFrame {
    std::uint32_t display_list = 0U;
    std::uint32_t task_address = 0U;
    std::uint32_t context_dl_start = 0U;
    std::uint32_t context_size = 0U;
    std::uint64_t sequence = 0U;
    std::vector<BackgroundCommandRange> background_ranges{};
    MatrixMap matrices{};
    std::uint32_t camera_token = 0U;
};

std::mutex g_mutex;
std::vector<RecordedEntry> g_entries;
std::vector<Track> g_tracks;
std::vector<SharedMatrixTrack> g_shared_matrix_tracks;
std::vector<SpecificMatrixSample> g_specific_samples;
std::vector<SpecificContinuity> g_specific_continuity;
std::vector<ModelRangeSample> g_model_range_samples;
std::unordered_map<std::uint32_t, ObjectLifetimeState> g_object_lifetimes;
std::vector<DirectMatrixSample> g_direct_samples;
std::vector<DirectContinuity> g_direct_continuity;
std::vector<CameraMatrixSample> g_camera_samples;
CameraContinuityState g_camera_continuity{};
std::uint32_t g_recording_camera_token = 0U;
std::vector<SubmittedFrame> g_submitted;
std::uint64_t g_frame = 0U;
std::uint64_t g_submission_sequence = 1U;
std::uint32_t g_next_token = 1U;
std::uint32_t g_empty_frames = 0U;
std::unordered_map<std::uint64_t, std::uint32_t> g_key_ordinals;
std::vector<BackgroundCommandRange> g_background_ranges;
bool g_background_capture_active = false;
std::uint32_t g_background_capture_begin = 0U;
thread_local MatrixMap g_active_matrices;
std::vector<std::pair<std::uint32_t,std::uint32_t>> g_mod_matrices;
thread_local SubmodelMatrixCapture g_submodel_matrix_capture{};
thread_local ModelRangeCapture g_model_range_captures[16]{};
thread_local std::size_t g_model_range_depth = 0U;
thread_local std::size_t g_model_range_overflow_depth = 0U;
thread_local DirectCapture g_direct_captures[3]{};
// The pinned recompiler uses host calls and does not maintain guest r31 at
// JAL sites. Policy hooks pass the verified instruction address explicitly.
struct CallsiteCapture {
    const recomp_context* context = nullptr;
    std::uint32_t target = 0U;
    std::uint32_t address = 0U;
};
thread_local CallsiteCapture g_pending_callsite{};
thread_local CallsiteCapture g_entered_callsite{};

[[nodiscard]] std::uint32_t EntryCallsite(
    const recomp_context* context, std::uint32_t target) {
    return context != nullptr && g_entered_callsite.context == context &&
        g_entered_callsite.target == target ? g_entered_callsite.address : 0U;
}
thread_local std::vector<BackgroundCommandRange> g_active_background_ranges;
thread_local std::uint32_t g_active_context_dl_start = 0U;
thread_local std::uint32_t g_active_context_size = 0U;
thread_local std::uint32_t g_active_camera_token = 0U;
thread_local std::uint32_t g_active_task_address = 0U;
// While an RT64 Rocket task is being decoded, the semantic sidecar owns the
// matching policy for every model matrix. Unknown matrices must therefore snap
// instead of escaping back into RT64's anonymous automatic matcher.
thread_local bool g_active_task_fail_closed = false;

std::atomic<std::uint64_t> g_trace_entries{0U};
std::atomic<std::uint64_t> g_trace_matches{0U};
std::atomic<std::uint64_t> g_trace_new_tracks{0U};
std::atomic<std::uint64_t> g_trace_conflicts{0U};
std::atomic<std::uint64_t> g_trace_task_matches{0U};
std::atomic<std::uint64_t> g_trace_task_misses{0U};
std::atomic<std::uint64_t> g_trace_ambiguous_rejects{0U};
std::atomic<std::uint64_t> g_trace_shared_samples{0U};
std::atomic<std::uint64_t> g_trace_shared_matches{0U};
std::atomic<std::uint64_t> g_trace_shared_new_tracks{0U};
std::atomic<std::uint64_t> g_trace_shared_rejects{0U};
std::atomic<std::uint64_t> g_trace_specific_samples{0U};
std::atomic<std::uint64_t> g_trace_specific_matches{0U};
std::atomic<std::uint64_t> g_trace_specific_new{0U};
std::atomic<std::uint64_t> g_trace_specific_conflicts{0U};
std::atomic<std::uint64_t> g_trace_v45_mode0{0U};
std::atomic<std::uint64_t> g_trace_v45_scoped_submodels{0U};
std::atomic<std::uint64_t> g_trace_v45_ambiguous{0U};
std::atomic<std::uint64_t> g_trace_v45_invalid{0U};
std::atomic<std::uint64_t> g_trace_unowned_matrix_snaps{0U};
std::atomic<std::uint64_t> g_trace_background_identities{0U};
std::atomic<std::uint64_t> g_trace_v36_object_lifetimes{0U};
std::atomic<std::uint64_t> g_trace_v36_model_range_matrices{0U};
std::atomic<std::uint64_t> g_trace_v36_model_range_specific_overlap{0U};
std::atomic<std::uint64_t> g_trace_v36_direct_samples{0U};
std::atomic<std::uint64_t> g_trace_v36_direct_matches{0U};
std::atomic<std::uint64_t> g_trace_v36_direct_new{0U};
std::atomic<std::uint64_t> g_trace_v36_camera_matrices{0U};
std::atomic<std::uint64_t> g_trace_v36_camera_epochs{0U};
std::atomic<std::uint64_t> g_trace_v36_unowned_camera{0U};
std::atomic<std::uint64_t> g_trace_v36_unowned_arena{0U};
std::atomic<std::uint64_t> g_trace_v36_unowned_other{0U};
std::atomic<std::uint64_t> g_trace_v37_collectible_rigid{0U};
std::atomic<std::uint64_t> g_trace_v38_collectible_vertex{0U};
std::atomic<std::uint64_t> g_trace_v38_mode0{0U};
std::atomic<std::uint64_t> g_trace_v38_mode1{0U};
std::atomic<std::uint64_t> g_trace_v38_mode2{0U};
std::atomic<std::uint64_t> g_trace_v38_mode3{0U};
std::atomic<std::uint64_t> g_trace_v38_mode4{0U};
std::atomic<std::uint64_t> g_trace_v38_mode5{0U};
std::atomic<std::uint64_t> g_trace_v38_mode_other{0U};
std::atomic<std::uint64_t> g_trace_v39_owner_position{0U};
std::atomic<std::uint64_t> g_trace_v39_matrix_fallback{0U};
std::atomic<std::uint64_t> g_trace_v39_matches{0U};
std::atomic<std::uint64_t> g_trace_v39_new{0U};
std::atomic<std::uint64_t> g_trace_v39_conflicts{0U};
// === ROCKET-R INTERPOLATION V42 TINKER TOKEN POLICY HOOKS ===
thread_local std::uint32_t g_tinker_token_draw_owner = 0U;
thread_local Vec3 g_tinker_token_draw_position{};
thread_local bool g_tinker_token_draw_position_valid = false;
std::atomic<std::uint64_t> g_trace_v42_token_draws{0U};
std::atomic<std::uint64_t> g_trace_v42_token_matrices{0U};
// === ROCKET-R INTERPOLATION V43 SHARED MODE0 DIRECT MATRICES ===
struct SharedMode0ScopeV43 {
    std::uint32_t model = 0U;
    std::uint32_t origin = 0U;
    std::uint32_t owner_hint = 0U;
    std::uint32_t caller = 0U;
    std::uint32_t stack = 0U;
    std::uint32_t claimed_matrix = 0U;
    std::uint64_t instance_key = 0U;
    Vec3 position{};
    bool position_valid = false;
    bool active = false;
    bool projected_shadow = false;
    bool player_wheel = false;
    std::size_t specific_begin = 0U;
    std::vector<std::uint64_t> secondary_pairs;
};
thread_local SharedMode0ScopeV43 g_shared_mode0_v43{};
std::atomic<std::uint64_t> g_trace_sidecar_mismatches{0U};
std::atomic<std::uint64_t> g_coverage_semantic_bindings{0U};
std::atomic<std::uint64_t> g_coverage_snapped_bindings{0U};
std::atomic<std::uint64_t> g_coverage_dynamic_vertex_bindings{0U};

[[nodiscard]] bool TraceEnabled() {
    const char* value = std::getenv("ROCKET_INTERPOLATION_TRACE");
    return value != nullptr && value[0] != '\0' &&
           !(value[0] == '0' && value[1] == '\0');
}


[[nodiscard]] gpr RdramAddress(std::uint32_t address) {
    return static_cast<gpr>(
        static_cast<std::int64_t>(static_cast<std::int32_t>(address)));
}

[[nodiscard]] bool ValidRange(std::uint32_t address, std::uint32_t size) {
    const std::uint32_t physical = address & kRdramMask;
    return address >= kRdramStart && address <= kRdramEnd &&
           size <= (kRdramMask + 1U) && physical <= kRdramMask + 1U - size;
}

[[nodiscard]] std::uint32_t Physical(std::uint32_t address) {
    return address & kRdramMask;
}

[[nodiscard]] std::uint32_t ReadU32(std::uint8_t* rdram,
                                    std::uint32_t address) {
    return static_cast<std::uint32_t>(MEM_W(0, RdramAddress(address)));
}

// === ROCKET-R INTERPOLATION V37 COLLECTIBLE RIGID ROTATION ===
// Rocket's model parser invokes the GameObject class callback at +0x6C for
// material commands. func_8006BDF0 is the decomp-identified token/collectible
// callback, so this is a semantic class test rather than a draw-order guess.
[[nodiscard]] bool IsRigidCollectibleObject(std::uint8_t*,
                                             std::uint32_t) {
    // === ROCKET-R V42 RETIRED V37 CLASS-CALLBACK GUESS ===
    // v40 proved func_8006BDF0 is the Tinker Token renderer itself, not a
    // GameObject class material callback. Keep this legacy path fail-closed;
    // v42's explicit func_8006BDF0 policy hooks are authoritative.
    return false;
}

[[nodiscard]] bool ReadCurrentDlHeadPhysical(std::uint8_t* rdram,
                                                   std::uint32_t& out) {
    if (rdram == nullptr || !ValidRange(kGfxContextDlHeadAddress, 4U)) return false;
    const std::uint32_t address = ReadU32(rdram, kGfxContextDlHeadAddress);
    if (!ValidRange(address, 8U)) return false;
    out = Physical(address);
    return true;
}

struct BackgroundCommandLocation {
    std::uint32_t range_index = 0U;
    std::uint32_t offset = 0U;
};

[[nodiscard]] bool BackgroundRangeLocation(
    std::uint32_t address, BackgroundCommandLocation& out) {
    const std::uint32_t physical = Physical(address);
    for (std::size_t index = 0; index < g_active_background_ranges.size(); ++index) {
        const BackgroundCommandRange& range = g_active_background_ranges[index];
        if (physical >= range.begin && physical < range.end) {
            out.range_index = static_cast<std::uint32_t>(index);
            out.offset = physical - range.begin;
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::uint32_t CanonicalActiveDlAddress(std::uint32_t address) {
    const std::uint32_t physical = Physical(address);
    if (g_active_context_size != 0U) {
        const std::uint64_t begin = g_active_context_dl_start;
        const std::uint64_t end = begin + static_cast<std::uint64_t>(g_active_context_size);
        if (physical >= begin && static_cast<std::uint64_t>(physical) < end) {
            return 0x80000000U | (physical - g_active_context_dl_start);
        }
    }
    return physical;
}

[[nodiscard]] std::uint32_t CanonicalBackgroundTarget(std::uint32_t address) {
    BackgroundCommandLocation nested{};
    if (BackgroundRangeLocation(address, nested)) {
        // Nested dynamic background command: make it relative to the captured
        // background producer rather than to the alternating whole task arena.
        return 0x40000000U |
               ((nested.range_index & 0x3FU) << 20U) |
               (nested.offset & 0x000FFFFFU);
    }

    const std::uint32_t physical = Physical(address);
    if (g_active_context_size != 0U) {
        const std::uint64_t begin = g_active_context_dl_start;
        const std::uint64_t end = begin + static_cast<std::uint64_t>(g_active_context_size);
        if (physical >= begin && static_cast<std::uint64_t>(physical) < end) {
            // The source command's range-relative offset already disambiguates
            // generated sky calls. Do not let an unrelated whole-arena offset
            // make the logical background identity jump between authored frames.
            return 0x80000000U;
        }
    }
    return CanonicalActiveDlAddress(address);
}


[[nodiscard]] std::uint16_t ReadU16(std::uint8_t* rdram,
                                    std::uint32_t address) {
    return static_cast<std::uint16_t>(MEM_H(0, RdramAddress(address)));
}

[[nodiscard]] std::int16_t ReadS16(std::uint8_t* rdram,
                                   std::uint32_t address) {
    return static_cast<std::int16_t>(MEM_H(0, RdramAddress(address)));
}

[[nodiscard]] bool Finite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] float DistanceSquared(const Vec3& a, const Vec3& b) {
    const float x = a.x - b.x;
    const float y = a.y - b.y;
    const float z = a.z - b.z;
    return x * x + y * y + z * z;
}

[[nodiscard]] Vec3 Add(const Vec3& a, const Vec3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] Vec3 Sub(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

[[nodiscard]] Vec3 Scale(const Vec3& v, float s) {
    return {v.x * s, v.y * s, v.z * s};
}

// Nintendo 64 Mtx: 16 signed integer halves followed by 16 unsigned
// fractional halves. Rocket/RT64 use row-vector transforms, so translation is
// the final row (elements 12..14). The address is only sampled on the guest
// thread; the renderer later consumes the frozen semantic binding instead.
[[nodiscard]] bool ReadMtxTranslation(std::uint8_t* rdram,
                                      std::uint32_t address, Vec3& out) {
    if (!ValidRange(address, kMtxBytes)) return false;
    float values[3]{};
    for (std::uint32_t axis = 0; axis < 3U; ++axis) {
        const std::uint32_t element = 12U + axis;
        const std::int16_t integer = ReadS16(
            rdram, address + element * sizeof(std::uint16_t));
        const std::uint16_t fraction = ReadU16(
            rdram, address + 0x20U + element * sizeof(std::uint16_t));
        values[axis] = static_cast<float>(integer) +
                       static_cast<float>(fraction) / 65536.0F;
    }
    out = {values[0], values[1], values[2]};
    return Finite(out);
}

// Only affine, nondegenerate orthogonal bases are safe rigid transforms.
// N64 fixed-point quantization needs a small relative orthogonality tolerance.
[[nodiscard]] bool ReadGuestMatrix(std::uint8_t* rdram, std::uint32_t address, float (&matrix)[16]) {
    if (!ValidRange(address, kMtxBytes)) return false;
    for (std::uint32_t i = 0; i < 16U; ++i) {
        matrix[i] = static_cast<float>(ReadS16(rdram, address + 2U * i)) +
            static_cast<float>(ReadU16(rdram, address + 32U + 2U * i)) / 65536.0F;
    }
    return true;
}

[[nodiscard]] bool SupportsRigidInterpolation(const float (&matrix)[16]) {
    if (matrix[3] != 0.0F || matrix[7] != 0.0F || matrix[11] != 0.0F || matrix[15] != 1.0F) return false;
    float length_squared[3]{};
    for (unsigned row = 0; row < 3; ++row) {
        for (unsigned column = 0; column < 3; ++column)
            length_squared[row] += matrix[4U * row + column] * matrix[4U * row + column];
        if (length_squared[row] < 0.000001F) return false;
    }
    for (unsigned a = 0; a < 3; ++a) {
        for (unsigned b = a + 1; b < 3; ++b) {
            float dot = 0.0F;
            for (unsigned column = 0; column < 3; ++column)
                dot += matrix[4U * a + column] * matrix[4U * b + column];
            if (std::abs(dot) > 0.02F * std::sqrt(length_squared[a] * length_squared[b])) return false;
        }
    }
    return true;
}

[[nodiscard]] bool SupportsRigidInterpolation(std::uint8_t* rdram, std::uint32_t address) {
    float matrix[16]{};
    return ReadGuestMatrix(rdram, address, matrix) && SupportsRigidInterpolation(matrix);
}

[[nodiscard]] bool SupportsRigidPair(std::uint8_t* rdram, std::uint32_t primary, std::uint32_t secondary) {
    float first[16]{}, second[16]{}, combined[16]{};
    if (!ReadGuestMatrix(rdram, primary, first) || !ReadGuestMatrix(rdram, secondary, second)) return false;
    // RT64's G_MTX_MUL composes secondary * primary. A nonuniform scale and
    // rotation can produce shear even when each input separately is rigid.
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 4; ++column)
            for (unsigned k = 0; k < 4; ++k)
                combined[row * 4U + column] += second[row * 4U + k] * first[k * 4U + column];
    return SupportsRigidInterpolation(combined);
}

[[nodiscard]] bool SupportsWheelDecomposition(const float (&m)[16]) {
    for (float value : m) if (!std::isfinite(value)) return false;
    if (m[3] != 0 || m[7] != 0 || m[11] != 0 || m[15] != 1) return false;
    const float det = m[0]*(m[5]*m[10]-m[6]*m[9]) - m[1]*(m[4]*m[10]-m[6]*m[8]) + m[2]*(m[4]*m[9]-m[5]*m[8]);
    float lengths = 1;
    for (unsigned row=0; row<3; ++row) {
        const auto i = row*4;
        lengths *= std::sqrt(m[i]*m[i]+m[i+1]*m[i+1]+m[i+2]*m[i+2]);
    }
    return lengths > 1e-9F && std::abs(det) > lengths * 1e-4F;
}

[[nodiscard]] bool SupportsWheelDecomposition(std::uint8_t* rdram, std::uint32_t address) {
    float matrix[16]{};
    return ReadGuestMatrix(rdram,address,matrix) && SupportsWheelDecomposition(matrix);
}

[[nodiscard]] bool SupportsWheelPair(std::uint8_t* rdram, std::uint32_t primary, std::uint32_t secondary) {
    float first[16]{}, second[16]{}, combined[16]{};
    if (!ReadGuestMatrix(rdram,primary,first) || !ReadGuestMatrix(rdram,secondary,second)) return false;
    for (unsigned row=0; row<4; ++row) for (unsigned column=0; column<4; ++column)
        for (unsigned k=0; k<4; ++k) combined[row*4+column] += second[row*4+k] * first[k*4+column];
    return SupportsWheelDecomposition(combined);
}

[[nodiscard]] std::uint32_t ReadCurrentGfxTask(std::uint8_t* rdram) {
    if (!ValidRange(kCurGfxTaskAddress, 4U)) return 0U;
    const std::uint32_t task = ReadU32(rdram, kCurGfxTaskAddress);
    return ValidRange(task, 0x18U) ? task : 0U;
}

struct GfxSemanticRef {
    std::uint32_t key = 0U;
    bool dynamic = false;
};

[[nodiscard]] GfxSemanticRef CanonicalGfxRef(
    std::uint8_t* rdram, std::uint32_t gfx) {
    GfxSemanticRef result{Physical(gfx), false};
    const std::uint32_t task = ReadCurrentGfxTask(rdram);
    if (task == 0U || gfx == 0U) return result;
    const std::uint32_t buffer = ReadU32(rdram, task + kGfxTaskCtxDlStartOffset);
    const std::uint32_t bytes = ReadU32(rdram, task + kGfxTaskCtxSizeOffset);
    if (!ValidRange(buffer, 8U) || bytes == 0U || bytes > 0x20000U) return result;
    const std::uint32_t p = Physical(gfx);
    const std::uint32_t begin = Physical(buffer);
    const std::uint32_t end = begin + bytes;
    if (p >= begin && p < end) {
        // Dynamic display lists live in Rocket's alternating task buffers. The
        // absolute RDRAM address therefore changes even when the same logical
        // sky/attachment draw is authored at the same command position. Use a
        // marked buffer-relative offset so the semantic key survives the flip.
        result.key = 0x80000000U | (p - begin);
        result.dynamic = true;
    }
    return result;
}

[[nodiscard]] std::uint64_t Mix64(std::uint64_t value) {
    value ^= value >> 30U;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27U;
    value *= 0x94D049BB133111EBULL;
    value ^= value >> 31U;
    return value;
}

[[nodiscard]] std::uint64_t EntryKey(std::uint32_t callsite,
                                     std::uint32_t gfx, bool has_mtx2,
                                     std::uint8_t alpha) {
    std::uint64_t value = static_cast<std::uint64_t>(Physical(callsite));
    value = Mix64(value ^ (static_cast<std::uint64_t>(Physical(gfx)) << 1U));
    value = Mix64(value ^ (static_cast<std::uint64_t>(has_mtx2) << 55U));
    // Alpha is deliberately coarse. Opaque/translucent classes must not share
    // history, while normal per-frame alpha animation should remain continuous.
    const std::uint64_t alpha_class = (alpha == 0xFFU) ? 1U : 2U;
    return Mix64(value ^ (alpha_class << 61U));
}

[[nodiscard]] std::uint32_t NormalizeIdentity(std::uint64_t value) {
    std::uint32_t id = static_cast<std::uint32_t>(Mix64(value));
    if (id == 0U || id == 0xFFFFFFFFU) id ^= 0x51A7C3D9U;
    if (id == 0U || id == 0xFFFFFFFFU) id = 1U;
    return id;
}

[[nodiscard]] std::uint32_t NextSpecificPresentationToken() {
    for (;;) {
        const std::uint32_t token = g_next_token++;
        if (token != 0U && token != 0xFFFFFFFFU) return token;
    }
}

[[nodiscard]] std::uint32_t MatrixIdentity(std::uint32_t token,
                                           std::uint32_t role) {
    return NormalizeIdentity((static_cast<std::uint64_t>(token) << 32U) |
                             static_cast<std::uint64_t>(role + 1U));
}

[[nodiscard]] bool ReadGuestVec3(std::uint8_t* rdram,
                                    std::uint32_t address, Vec3& out) {
    if (rdram == nullptr || !ValidRange(address, 12U)) return false;
    out.x = std::bit_cast<float>(ReadU32(rdram, address + 0U));
    out.y = std::bit_cast<float>(ReadU32(rdram, address + 4U));
    out.z = std::bit_cast<float>(ReadU32(rdram, address + 8U));
    return Finite(out);
}

[[nodiscard]] std::uint32_t AcquireObjectLifetimeTokenLocked(
    std::uint8_t* rdram, std::uint32_t object) {
    if (!ValidRange(object, kGameObjectSubmodelCountOffset + 4U)) return 0U;
    const std::uint32_t object_class = ReadU32(rdram, object + kGameObjectClassOffset);
    const std::uint32_t submodels = ReadU32(rdram, object + kGameObjectSubmodelsOffset);
    const std::uint32_t count = ReadU32(rdram, object + kGameObjectSubmodelCountOffset);
    std::uint64_t fingerprint = Mix64(static_cast<std::uint64_t>(object_class));
    fingerprint = Mix64(fingerprint ^ (static_cast<std::uint64_t>(submodels) << 1U));
    fingerprint = Mix64(fingerprint ^ (static_cast<std::uint64_t>(count) << 33U));

    Vec3 position{};
    const bool position_valid = ReadGuestVec3(
        rdram, object + kGameObjectPositionOffset, position);
    ObjectLifetimeState& state = g_object_lifetimes[Physical(object)];

    bool restart = state.token == 0U || state.fingerprint != fingerprint;
    if (!restart && state.last_frame != g_frame) {
        if (state.last_frame + 1U != g_frame) {
            restart = true;
        } else if (state.position_valid != position_valid) {
            restart = true;
        } else if (position_valid) {
            const float dist2 = DistanceSquared(position, state.position);
            const float limit2 = kMaximumTrackDistance * kMaximumTrackDistance;
            if (!std::isfinite(dist2) || dist2 > limit2) restart = true;
        }
    }
    if (restart) {
        state.token = NextSpecificPresentationToken();
        g_trace_v36_object_lifetimes.fetch_add(1U, std::memory_order_relaxed);
    }
    state.fingerprint = fingerprint;
    state.position = position;
    state.position_valid = position_valid;
    state.last_frame = g_frame;
    return state.token;
}



[[nodiscard]] rocket::presentation::MatrixBinding IgnoredBinding() {
    // G_EX_ID_IGNORE is zero. Keeping an explicit zero-ID binding in the
    // task-local sidecar is intentional: RT64 must snap this matrix to the
    // newest authored endpoint instead of falling back to automatic matching.
    return {};
}

void AddBinding(MatrixMap& map, std::unordered_set<std::uint32_t>& conflicts,
                std::uint32_t address,
                const rocket::presentation::MatrixBinding& binding) {
    if (address == 0U || !ValidRange(address, kMtxBytes)) return;
    const std::uint32_t physical = Physical(address);
    if (conflicts.contains(physical)) {
        map[physical] = BindingRecord{IgnoredBinding()};
        return;
    }
    const auto found = map.find(physical);
    if (found == map.end()) {
        map.emplace(physical, BindingRecord{binding});
        return;
    }
    if (found->second.binding.identity != binding.identity) {
        // v4.2 erased the semantic binding here. That silently handed the
        // matrix back to RT64's anonymous matcher, which can pair repeated
        // scenery with the wrong previous transform. Preserve an explicit
        // IGNORE binding instead so ambiguity always fails closed.
        found->second = BindingRecord{IgnoredBinding()};
        conflicts.insert(physical);
        g_trace_conflicts.fetch_add(1U, std::memory_order_relaxed);
    }
}

void ExpireTracks() {
    std::erase_if(g_tracks, [](const Track& track) {
        return g_frame > track.last_frame + kMaximumTrackAge;
    });
    std::erase_if(g_shared_matrix_tracks, [](const SharedMatrixTrack& track) {
        return g_frame > track.last_frame + kMaximumTrackAge;
    });
}

struct SharedMatrixAccumulator {
    std::uint32_t address = 0U;
    std::uint32_t physical = 0U;
    std::uint32_t role_mask = 0U;
    std::vector<std::uint64_t> descriptors{};
    Vec3 position{};
    bool position_valid = false;
    bool position_conflict = false;
    bool dynamic_gfx = false;
};

[[nodiscard]] std::uint64_t SharedMatrixSignature(
    SharedMatrixAccumulator accumulator) {
    std::sort(accumulator.descriptors.begin(), accumulator.descriptors.end());
    std::uint64_t hash = Mix64(0x5348415245444D54ULL ^
                               static_cast<std::uint64_t>(accumulator.role_mask));
    hash = Mix64(hash ^ static_cast<std::uint64_t>(accumulator.descriptors.size()));
    for (const std::uint64_t descriptor : accumulator.descriptors) {
        hash = Mix64(hash ^ descriptor);
    }
    return hash;
}

void AddSharedOccurrence(
    std::unordered_map<std::uint32_t, SharedMatrixAccumulator>& accumulators,
    std::uint32_t address, std::uint32_t role, std::uint64_t entry_key,
    const Vec3& position, bool position_valid, bool dynamic_gfx) {
    if (address == 0U || !ValidRange(address, kMtxBytes)) return;
    const std::uint32_t physical = Physical(address);
    auto& accumulator = accumulators[physical];
    if (accumulator.address == 0U) {
        accumulator.address = address;
        accumulator.physical = physical;
    }
    accumulator.role_mask |= role;
    accumulator.dynamic_gfx = accumulator.dynamic_gfx || dynamic_gfx;
    accumulator.descriptors.push_back(Mix64(
        entry_key ^ (static_cast<std::uint64_t>(role) << 60U)));
    if (position_valid) {
        if (!accumulator.position_valid) {
            accumulator.position = position;
            accumulator.position_valid = true;
        } else if (DistanceSquared(accumulator.position, position) >
                   kPositionEpsilon * kPositionEpsilon) {
            // The same physical matrix slot was observed with different
            // contents during one authored frame. Never interpolate it: this
            // is either allocator reuse or a late overwrite, both of which
            // invalidate a shared semantic owner.
            accumulator.position_conflict = true;
        }
    }
}

std::vector<SharedMatrixSample> BuildSharedMatrixSamples() {
    std::unordered_map<std::uint32_t, SharedMatrixAccumulator> accumulators;
    accumulators.reserve(g_entries.size() * 2U);
    for (const RecordedEntry& entry : g_entries) {
        AddSharedOccurrence(accumulators, entry.mtx1, 1U, entry.key,
                            entry.mtx1_position, entry.mtx1_position_valid,
                            entry.dynamic_gfx);
        AddSharedOccurrence(accumulators, entry.mtx2, 2U, entry.key,
                            entry.mtx2_position, entry.mtx2_position_valid,
                            entry.dynamic_gfx);
    }

    std::vector<SharedMatrixSample> samples;
    samples.reserve(accumulators.size());
    for (auto& [physical, accumulator] : accumulators) {
        if (accumulator.descriptors.size() < 2U ||
            accumulator.position_conflict) {
            continue;
        }
        SharedMatrixSample sample{};
        sample.signature = SharedMatrixSignature(accumulator);
        sample.address = accumulator.address;
        sample.physical = physical;
        sample.role_mask = accumulator.role_mask;
        sample.position = accumulator.position;
        sample.position_valid = accumulator.position_valid;
        sample.dynamic_gfx = accumulator.dynamic_gfx;
        samples.push_back(sample);
    }
    return samples;
}

// Index only eligible preceding-frame tracks. Preserve their vector order so
// equal-distance candidates retain exactly the same ambiguity behaviour.
template<class T, class Key>
auto PreviousTrackIndex(const std::vector<T>& tracks, Key key) {
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> index;
    index.reserve(tracks.size());
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const auto& track = tracks[i];
        if (!track.claimed && track.last_frame < g_frame && g_frame - track.last_frame == 1U)
            index[key(track)].push_back(i);
    }
    return index;
}

void MatchSharedMatrixSamples(std::vector<SharedMatrixSample>& samples) {
    for (SharedMatrixTrack& track : g_shared_matrix_tracks) track.claimed = false;
    if (samples.empty()) return;
    const auto previous = PreviousTrackIndex(g_shared_matrix_tracks,
        [](const auto& track) { return track.signature; });

    std::vector<SharedCandidate> candidates;
    for (std::size_t si = 0; si < samples.size(); ++si) {
        const SharedMatrixSample& sample = samples[si];
        const auto eligible = previous.find(sample.signature);
        if (eligible == previous.end()) continue;
        for (std::size_t ti : eligible->second) {
            const SharedMatrixTrack& track = g_shared_matrix_tracks[ti];
            if (track.signature != sample.signature || track.claimed ||
                track.last_frame >= g_frame ||
                g_frame - track.last_frame != 1U) {
                continue;
            }

            float cost = 0.25F;
            if (sample.position_valid && track.position_valid) {
                const float dist2 = DistanceSquared(sample.position, track.position);
                const float limit2 = kMaximumTrackDistance * kMaximumTrackDistance;
                if (!std::isfinite(dist2) || dist2 > limit2) continue;
                cost = dist2;
            } else if (sample.position_valid != track.position_valid) {
                continue;
            }
            candidates.push_back({si, ti, cost});
        }
    }

    const float inf = std::numeric_limits<float>::infinity();
    const std::size_t none = std::numeric_limits<std::size_t>::max();
    std::vector<float> sample_best(samples.size(), inf);
    std::vector<float> sample_second(samples.size(), inf);
    std::vector<std::size_t> sample_best_track(samples.size(), none);
    std::vector<float> track_best(g_shared_matrix_tracks.size(), inf);
    std::vector<float> track_second(g_shared_matrix_tracks.size(), inf);
    std::vector<std::size_t> track_best_sample(g_shared_matrix_tracks.size(), none);

    auto update_best = [](float cost, std::size_t index,
                          float& best, float& second, std::size_t& best_index) {
        if (cost < best) {
            second = best;
            best = cost;
            best_index = index;
        } else if (cost < second) {
            second = cost;
        }
    };
    for (const SharedCandidate& candidate : candidates) {
        update_best(candidate.cost, candidate.track,
                    sample_best[candidate.sample], sample_second[candidate.sample],
                    sample_best_track[candidate.sample]);
        update_best(candidate.cost, candidate.sample,
                    track_best[candidate.track], track_second[candidate.track],
                    track_best_sample[candidate.track]);
    }

    auto clearly_better = [](float best, float second) {
        if (!std::isfinite(best)) return false;
        if (!std::isfinite(second)) return true;
        const float required = std::max(
            kAmbiguityAbsoluteMargin, best * kAmbiguityRelativeMargin);
        return second > best + required;
    };

    std::vector<bool> matched(samples.size(), false);
    for (std::size_t si = 0; si < samples.size(); ++si) {
        const std::size_t ti = sample_best_track[si];
        if (ti == none || ti >= g_shared_matrix_tracks.size()) continue;
        if (track_best_sample[ti] != si ||
            !clearly_better(sample_best[si], sample_second[si]) ||
            !clearly_better(track_best[ti], track_second[ti])) {
            g_trace_shared_rejects.fetch_add(1U, std::memory_order_relaxed);
            continue;
        }
        SharedMatrixTrack& track = g_shared_matrix_tracks[ti];
        if (track.claimed) continue;
        samples[si].track_token = track.token;
        track.claimed = true;
        matched[si] = true;
        g_trace_shared_matches.fetch_add(1U, std::memory_order_relaxed);
    }

    for (std::size_t si = 0; si < samples.size(); ++si) {
        SharedMatrixSample& sample = samples[si];
        if (!matched[si]) {
            std::uint32_t token = g_next_token++;
            if (token == 0U || token == 0xFFFFFFFFU) token = g_next_token++;
            sample.track_token = token;
            SharedMatrixTrack track{};
            track.signature = sample.signature;
            track.token = token;
            track.position = sample.position;
            track.position_valid = sample.position_valid;
            track.last_frame = g_frame;
            track.claimed = true;
            g_shared_matrix_tracks.push_back(track);
            g_trace_shared_new_tracks.fetch_add(1U, std::memory_order_relaxed);
        }
    }

    std::unordered_map<std::uint32_t, std::size_t> tokens;
    tokens.reserve(g_shared_matrix_tracks.size());
    for (std::size_t i = 0; i < g_shared_matrix_tracks.size(); ++i)
        tokens.emplace(g_shared_matrix_tracks[i].token, i);
    for (const SharedMatrixSample& sample : samples) {
        const auto found = tokens.find(sample.track_token);
        if (found == tokens.end()) continue;
        auto& track = g_shared_matrix_tracks[found->second];
        track.position = sample.position;
        track.position_valid = sample.position_valid;
        track.last_frame = g_frame;
    }

    g_trace_shared_samples.fetch_add(samples.size(), std::memory_order_relaxed);
}

// ROCKET-R INTERPOLATION V35: high-confidence Submodel matrices are matched
// separately from the stable v5/v6 RenderEntry heuristic. This cannot make an
// existing generic match less conservative: it only overrides a physical slot
// when Rocket's own func_8001EA18 proved the exact GameObject/Submodel owner.
// v36 exact allocation-range layer. The range is captured inside Rocket's
// real func_8001ECEC model renderer. It covers parent/attachment matrices that
// never reach func_8001EA18 while deliberately leaving v35's exact returned
// Submodel matrix as the highest-priority owner for any overlapping slot.
void FinalizeModelRangeBindings(MatrixMap& out) {
    std::unordered_set<std::uint32_t> specific_owned;
    specific_owned.reserve(g_specific_samples.size());
    for (const SpecificMatrixSample& sample : g_specific_samples) {
        if (sample.address != 0U && ValidRange(sample.address, kMtxBytes)) {
            specific_owned.insert(Physical(sample.address));
        }
    }

    std::unordered_map<std::uint32_t, std::uint32_t> claims;
    claims.reserve(g_model_range_samples.size());
    for (const ModelRangeSample& sample : g_model_range_samples) {
        if (sample.address == 0U || sample.identity == 0U ||
            sample.identity == 0xFFFFFFFFU || !ValidRange(sample.address, kMtxBytes)) {
            continue;
        }
        const std::uint32_t physical = Physical(sample.address);
        if (specific_owned.contains(physical)) {
            g_trace_v36_model_range_specific_overlap.fetch_add(1U, std::memory_order_relaxed);
            continue;
        }
        const auto prior = claims.find(physical);
        if (prior != claims.end() && prior->second != sample.identity) {
            // Keep conflicts sticky: A/B/A claims must not revive A.
            prior->second = 0U;
            out.insert_or_assign(physical, BindingRecord{IgnoredBinding()});
            continue;
        }
        claims[physical] = sample.identity;
        rocket::presentation::MatrixBinding binding{};
        binding.identity = sample.identity;
        out.insert_or_assign(physical, BindingRecord{binding});
    }
}

// Scoped fallback for the only two non-camera CPU matrix producers outside the
// RenderEntry path. Repeated calls from the same producer are paired by the same
// mutual-nearest + ambiguity-margin policy as the stable v5/v6 matcher. This
// deliberately avoids draw-order/occurrence identities when effects appear or
// disappear between authored frames.
void FinalizeDirectMatrixBindings(MatrixMap& out) {
    std::erase_if(g_direct_continuity, [](const DirectContinuity& item) {
        return g_frame > item.last_frame + kMaximumTrackAge;
    });
    for (DirectContinuity& item : g_direct_continuity) item.claimed = false;
    const auto previous = PreviousTrackIndex(g_direct_continuity,
        [](const auto& track) { return track.key; });

    std::vector<DirectCandidate> candidates;
    candidates.reserve(g_direct_samples.size() * 4U);
    for (std::size_t si = 0U; si < g_direct_samples.size(); ++si) {
        const DirectMatrixSample& sample = g_direct_samples[si];
        const auto eligible = previous.find(sample.key);
        if (eligible == previous.end()) continue;
        for (std::size_t ti : eligible->second) {
            const DirectContinuity& item = g_direct_continuity[ti];
            if (item.key != sample.key || item.claimed ||
                item.last_frame >= g_frame || g_frame - item.last_frame != 1U) {
                continue;
            }
            if (item.position_valid != sample.position_valid) continue;
            float cost = 0.25F;
            if (sample.position_valid) {
                const float dist2 = DistanceSquared(sample.position, item.position);
                const float limit2 = kMaximumTrackDistance * kMaximumTrackDistance;
                if (!std::isfinite(dist2) || dist2 > limit2) continue;
                cost = dist2;
            }
            candidates.push_back({si, ti, cost});
        }
    }

    const float inf = std::numeric_limits<float>::infinity();
    const std::size_t none = std::numeric_limits<std::size_t>::max();
    std::vector<float> sample_best(g_direct_samples.size(), inf);
    std::vector<float> sample_second(g_direct_samples.size(), inf);
    std::vector<std::size_t> sample_best_track(g_direct_samples.size(), none);
    std::vector<float> track_best(g_direct_continuity.size(), inf);
    std::vector<float> track_second(g_direct_continuity.size(), inf);
    std::vector<std::size_t> track_best_sample(g_direct_continuity.size(), none);

    auto update_best = [](float cost, std::size_t index,
                          float& best, float& second, std::size_t& best_index) {
        if (cost < best) {
            second = best;
            best = cost;
            best_index = index;
        } else if (cost < second) {
            second = cost;
        }
    };
    for (const DirectCandidate& candidate : candidates) {
        update_best(candidate.cost, candidate.track,
                    sample_best[candidate.sample], sample_second[candidate.sample],
                    sample_best_track[candidate.sample]);
        update_best(candidate.cost, candidate.sample,
                    track_best[candidate.track], track_second[candidate.track],
                    track_best_sample[candidate.track]);
    }
    auto clearly_better = [](float best, float second) {
        if (!std::isfinite(best)) return false;
        if (!std::isfinite(second)) return true;
        const float required = std::max(
            kAmbiguityAbsoluteMargin, best * kAmbiguityRelativeMargin);
        return second > best + required;
    };

    std::vector<bool> matched(g_direct_samples.size(), false);
    for (std::size_t si = 0U; si < g_direct_samples.size(); ++si) {
        const std::size_t ti = sample_best_track[si];
        if (ti == none || ti >= g_direct_continuity.size()) continue;
        if (track_best_sample[ti] != si ||
            !clearly_better(sample_best[si], sample_second[si]) ||
            !clearly_better(track_best[ti], track_second[ti])) {
            continue;
        }
        DirectContinuity& item = g_direct_continuity[ti];
        if (item.claimed) continue;
        g_direct_samples[si].track_token = item.token;
        item.claimed = true;
        matched[si] = true;
        g_trace_v36_direct_matches.fetch_add(1U, std::memory_order_relaxed);
    }

    for (std::size_t si = 0U; si < g_direct_samples.size(); ++si) {
        DirectMatrixSample& sample = g_direct_samples[si];
        if (!matched[si]) {
            DirectContinuity item{};
            item.key = sample.key;
            item.token = NextSpecificPresentationToken();
            item.position = sample.position;
            item.position_valid = sample.position_valid;
            item.last_frame = g_frame;
            item.claimed = true;
            sample.track_token = item.token;
            g_direct_continuity.push_back(item);
            g_trace_v36_direct_new.fetch_add(1U, std::memory_order_relaxed);
        } else {
            const auto found = std::find_if(
                g_direct_continuity.begin(), g_direct_continuity.end(),
                [&](const DirectContinuity& item) {
                    return item.token == sample.track_token;
                });
            if (found != g_direct_continuity.end()) {
                found->position = sample.position;
                found->position_valid = sample.position_valid;
                found->last_frame = g_frame;
            }
        }
        if (sample.track_token == 0U || !ValidRange(sample.address, kMtxBytes)) continue;
        rocket::presentation::MatrixBinding binding{};
        binding.identity = MatrixIdentity(
            sample.track_token, 0x50U + sample.kind * 8U + sample.role);
        out.insert_or_assign(Physical(sample.address), BindingRecord{binding});
    }
}

void FinalizeCameraMatrixBindings(MatrixMap& out) {
    for (const CameraMatrixSample& sample : g_camera_samples) {
        if (sample.identity == 0U || sample.identity == 0xFFFFFFFFU ||
            !ValidRange(sample.address, kMtxBytes)) continue;
        rocket::presentation::MatrixBinding binding{};
        binding.identity = sample.identity;
        out.insert_or_assign(Physical(sample.address), BindingRecord{binding});
    }
}

void FinalizeSpecificMatrixBindings(MatrixMap& out) {
    std::erase_if(g_specific_continuity, [](const SpecificContinuity& item) {
        return g_frame > item.last_frame + kMaximumTrackAge;
    });
    for (SpecificContinuity& item : g_specific_continuity) item.claimed = false;
    const auto previous = PreviousTrackIndex(g_specific_continuity,
        [](const auto& track) { return track.key; });

    std::unordered_map<std::uint32_t, std::uint32_t> specific_claims;
    specific_claims.reserve(g_specific_samples.size());
    std::unordered_map<std::uint64_t, std::size_t> instance_claims;
    for (const auto& sample : g_specific_samples) ++instance_claims[sample.key];

    for (SpecificMatrixSample& sample : g_specific_samples) {
        if (sample.address == 0U || !ValidRange(sample.address, kMtxBytes)) continue;
        // Never turn draw order into object identity. Two separate matrices
        // with the same semantic owner are ambiguous, even if one disappears
        // next frame. Explicitly rejected ownership must override generic matching.
        if (sample.force_snap || instance_claims[sample.key] != 1U) {
            (sample.force_snap ? g_trace_v45_invalid : g_trace_v45_ambiguous)
                .fetch_add(1U, std::memory_order_relaxed);
            const auto physical = Physical(sample.address);
            specific_claims[physical] = 0U;
            out.insert_or_assign(physical, BindingRecord{IgnoredBinding()});
            continue;
        }
        SpecificContinuity* match = nullptr;
        bool ambiguous = false;
        const auto eligible = previous.find(sample.key);
        const std::vector<std::size_t> empty;
        for (std::size_t ti : eligible == previous.end() ? empty : eligible->second) {
            auto& item = g_specific_continuity[ti];
            if (item.key != sample.key || item.claimed ||
                item.last_frame >= g_frame || g_frame - item.last_frame != 1U) {
                continue;
            }
            if (sample.position_valid != item.position_valid) continue;
            if (sample.position_valid) {
                const float dist2 = DistanceSquared(sample.position, item.position);
                const float limit2 = kMaximumTrackDistance * kMaximumTrackDistance;
                if (!std::isfinite(dist2) || dist2 > limit2) continue;
            }
            if (match != nullptr) {
                ambiguous = true;
                break;
            }
            match = &item;
        }

        if (match != nullptr && !ambiguous) {
            sample.track_token = match->token;
            match->position = sample.position;
            match->position_valid = sample.position_valid;
            match->last_frame = g_frame;
            match->claimed = true;
            g_trace_specific_matches.fetch_add(1U, std::memory_order_relaxed);
            if (sample.rigid_decompose) {
                g_trace_v39_matches.fetch_add(1U, std::memory_order_relaxed);
            }
        } else {
            SpecificContinuity item{};
            item.key = sample.key;
            item.token = NextSpecificPresentationToken();
            item.position = sample.position;
            item.position_valid = sample.position_valid;
            item.last_frame = g_frame;
            item.claimed = true;
            sample.track_token = item.token;
            g_specific_continuity.push_back(item);
            g_trace_specific_new.fetch_add(1U, std::memory_order_relaxed);
            if (sample.rigid_decompose) {
                g_trace_v39_new.fetch_add(1U, std::memory_order_relaxed);
            }
        }

        if (sample.track_token == 0U || sample.address == 0U ||
            !ValidRange(sample.address, kMtxBytes)) continue;
        const std::uint32_t physical = Physical(sample.address);
        rocket::presentation::MatrixBinding binding{};
        binding.identity = MatrixIdentity(sample.track_token, 0x40U);
        binding.interpolate_vertices = sample.collectible_billboard;
        if (binding.interpolate_vertices) {
            g_trace_v38_collectible_vertex.fetch_add(1U, std::memory_order_relaxed);
        }
        binding.interpolate_texcoords = false;
        binding.interpolate_tiles = false;
        binding.rigid_decompose = sample.rigid_decompose;
        binding.interpolate_shape = sample.interpolate_shape;

        const auto prior = specific_claims.find(physical);
        if (prior == specific_claims.end()) {
            specific_claims.emplace(physical, binding.identity);
            // A proven Submodel owner is more specific than a RenderEntry-level
            // heuristic, so it intentionally overlays that one exact matrix.
            out.insert_or_assign(physical, BindingRecord{binding});
        } else if (prior->second != binding.identity) {
            // specific physical-slot ownership conflict: never guess.
            out.insert_or_assign(physical, BindingRecord{IgnoredBinding()});
            g_trace_specific_conflicts.fetch_add(1U, std::memory_order_relaxed);
            if (sample.rigid_decompose) {
                g_trace_v39_conflicts.fetch_add(1U, std::memory_order_relaxed);
            }
        }
    }
}

void FinalizeTracksAndBindings(MatrixMap& out) {
    ExpireTracks();
    for (Track& track : g_tracks) track.claimed = false;
    const auto previous = PreviousTrackIndex(g_tracks,
        [](const auto& track) { return track.key; });

    std::vector<Candidate> candidates;
    candidates.reserve(g_entries.size() * 4U);
    for (std::size_t ei = 0; ei < g_entries.size(); ++ei) {
        const RecordedEntry& entry = g_entries[ei];
        const auto eligible = previous.find(entry.key);
        if (eligible == previous.end()) continue;
        for (std::size_t ti : eligible->second) {
            const Track& track = g_tracks[ti];
            if (track.key != entry.key || track.claimed ||
                track.last_frame >= g_frame) {
                continue;
            }

            // Identity is continuous only across adjacent authored frames.
            // Reappearing/cull-restored geometry starts a new presentation
            // lifetime rather than interpolating across missing endpoints.
            const std::uint64_t gap = g_frame - track.last_frame;
            if (gap != 1U) continue;

            float cost = std::numeric_limits<float>::max();
            if (entry.position_valid && track.position_valid) {
                const float dist2 = DistanceSquared(entry.position, track.position);
                const float limit2 = kMaximumTrackDistance * kMaximumTrackDistance;
                if (!std::isfinite(dist2) || dist2 > limit2) continue;

                const float ordinal_delta = static_cast<float>(
                    entry.ordinal > track.ordinal
                        ? entry.ordinal - track.ordinal
                        : track.ordinal - entry.ordinal);
                cost = dist2 + ordinal_delta * ordinal_delta * kOrdinalPenalty;
            } else if (entry.ordinal == track.ordinal) {
                cost = 0.25F;
            } else {
                continue;
            }
            candidates.push_back({ei, ti, cost});
        }
    }

    // Greedy nearest-neighbour matching is unsafe for repeated render entries:
    // two equally plausible fence/billboard/scenery instances can swap owners
    // for one frame. v5 accepts a pair only when it is the MUTUAL best match and
    // clearly separated from the next candidate on both sides. Anything
    // ambiguous snaps to the authored endpoint with a fresh identity.
    const float inf = std::numeric_limits<float>::infinity();
    const std::size_t none = std::numeric_limits<std::size_t>::max();
    std::vector<float> entry_best(g_entries.size(), inf);
    std::vector<float> entry_second(g_entries.size(), inf);
    std::vector<std::size_t> entry_best_track(g_entries.size(), none);
    std::vector<float> track_best(g_tracks.size(), inf);
    std::vector<float> track_second(g_tracks.size(), inf);
    std::vector<std::size_t> track_best_entry(g_tracks.size(), none);

    auto update_best = [](float cost, std::size_t index,
                          float& best, float& second, std::size_t& best_index) {
        if (cost < best) {
            second = best;
            best = cost;
            best_index = index;
        } else if (cost < second) {
            second = cost;
        }
    };

    for (const Candidate& candidate : candidates) {
        update_best(candidate.cost, candidate.track,
                    entry_best[candidate.entry], entry_second[candidate.entry],
                    entry_best_track[candidate.entry]);
        update_best(candidate.cost, candidate.entry,
                    track_best[candidate.track], track_second[candidate.track],
                    track_best_entry[candidate.track]);
    }

    auto clearly_better = [](float best, float second) {
        if (!std::isfinite(best)) return false;
        if (!std::isfinite(second)) return true;
        const float required = std::max(
            kAmbiguityAbsoluteMargin, best * kAmbiguityRelativeMargin);
        return second > best + required;
    };

    std::vector<bool> entry_claimed(g_entries.size(), false);
    for (std::size_t ei = 0; ei < g_entries.size(); ++ei) {
        const std::size_t ti = entry_best_track[ei];
        if (ti == none || ti >= g_tracks.size()) continue;
        if (track_best_entry[ti] != ei ||
            !clearly_better(entry_best[ei], entry_second[ei]) ||
            !clearly_better(track_best[ti], track_second[ti])) {
            g_trace_ambiguous_rejects.fetch_add(1U, std::memory_order_relaxed);
            continue;
        }

        RecordedEntry& entry = g_entries[ei];
        Track& track = g_tracks[ti];
        if (track.claimed) continue;
        entry.track_token = track.token;
        entry_claimed[ei] = true;
        track.claimed = true;
        g_trace_matches.fetch_add(1U, std::memory_order_relaxed);
    }

    for (std::size_t ei = 0; ei < g_entries.size(); ++ei) {
        RecordedEntry& entry = g_entries[ei];
        if (!entry_claimed[ei]) {
            std::uint32_t token = g_next_token++;
            if (token == 0U || token == 0xFFFFFFFFU) token = g_next_token++;
            entry.track_token = token;
            Track track{};
            track.key = entry.key;
            track.token = token;
            track.ordinal = entry.ordinal;
            track.position = entry.position;
            track.position_valid = entry.position_valid;
            track.last_frame = g_frame;
            track.claimed = true;
            g_tracks.push_back(track);
            g_trace_new_tracks.fetch_add(1U, std::memory_order_relaxed);
        }
    }

    // Update existing tracks only after all assignments are frozen. Velocity is
    // diagnostic/history only in v5; owner selection never extrapolates it.
    std::unordered_map<std::uint32_t, std::size_t> tokens;
    tokens.reserve(g_tracks.size());
    for (std::size_t i = 0; i < g_tracks.size(); ++i) tokens.emplace(g_tracks[i].token, i);
    for (const RecordedEntry& entry : g_entries) {
        const auto found = tokens.find(entry.track_token);
        if (found == tokens.end()) continue;
        Track& track = g_tracks[found->second];
        if (entry.position_valid && track.position_valid &&
            track.last_frame < g_frame) {
            const float gap = static_cast<float>(g_frame - track.last_frame);
            if (gap > 0.0F) {
                track.velocity = Scale(Sub(entry.position, track.position), 1.0F / gap);
            }
        } else if (!track.position_valid) {
            track.velocity = {};
        }
        track.position = entry.position;
        track.position_valid = entry.position_valid;
        track.ordinal = entry.ordinal;
        track.last_frame = g_frame;
    }

    // v6: a large class of Rocket's articulated presentation uses one parent
    // matrix for several render entries (body attachments, wheel pieces, head
    // hardware and skybox panels are examples). v5 correctly treated the
    // repeated physical address as a conflict because each child entry had a
    // different token. That was safe, but it forced the shared parent matrix
    // to snap at 30 Hz. Give the shared matrix itself a stable semantic owner
    // derived from the complete set of entries that reference it.
    std::vector<SharedMatrixSample> shared_samples = BuildSharedMatrixSamples();
    MatchSharedMatrixSamples(shared_samples);
    std::unordered_map<std::uint32_t, rocket::presentation::MatrixBinding>
        shared_bindings;
    shared_bindings.reserve(shared_samples.size());
    for (const SharedMatrixSample& sample : shared_samples) {
        if (sample.track_token == 0U) continue;
        rocket::presentation::MatrixBinding binding{};
        binding.identity = MatrixIdentity(
            sample.track_token, 0x20U + sample.role_mask);
        // Dynamic task-buffer display lists are Rocket's generated geometry
        // path (sky panels, tractor beam/sphere, particles and similar effects).
        // RT64 still requires equal vertex counts before computing velocities, so
        // enabling interpolation here is global but remains topology fail-closed.
        binding.interpolate_vertices = sample.dynamic_gfx;
        binding.interpolate_texcoords = sample.dynamic_gfx;
        binding.interpolate_tiles = false;
        shared_bindings.emplace(sample.physical, binding);
    }

    std::unordered_set<std::uint32_t> conflicts;
    for (const RecordedEntry& entry : g_entries) {
        if (entry.track_token == 0U) continue;
        rocket::presentation::MatrixBinding root{};
        root.identity = MatrixIdentity(entry.track_token, 0U);
        root.interpolate_vertices = entry.dynamic_gfx;
        root.interpolate_texcoords = entry.dynamic_gfx;
        root.interpolate_tiles = false;

        const std::uint32_t root_physical = Physical(entry.mtx1);
        if (entry.mtx1 == 0U ||
            shared_bindings.find(root_physical) == shared_bindings.end()) {
            AddBinding(out, conflicts, entry.mtx1, root);
        }

        if (entry.mtx2 != 0U) {
            const std::uint32_t child_physical = Physical(entry.mtx2);
            if (shared_bindings.find(child_physical) == shared_bindings.end()) {
                rocket::presentation::MatrixBinding child = root;
                child.identity = MatrixIdentity(entry.track_token, 1U);
                AddBinding(out, conflicts, entry.mtx2, child);
            }
        }
    }

    // Shared owners are authored once after per-entry bindings. No conflicting
    // child can erase them because every occurrence of the same physical slot
    // was already included in the shared signature above.
    for (const SharedMatrixSample& sample : shared_samples) {
        const auto found = shared_bindings.find(sample.physical);
        if (found != shared_bindings.end()) {
            AddBinding(out, conflicts, sample.address, found->second);
        }
    }
}

void MaybeTraceSummary() {
    if (!TraceEnabled() || (g_frame % 120U) != 0U) return;
    std::fprintf(stderr,
        "[rocket-presentation] frame=%llu entries=%llu matched=%llu new=%llu conflicts=%llu ambiguous=%llu shared=%llu shared-match=%llu shared-new=%llu shared-reject=%llu task-match=%llu task-miss=%llu sidecar-mismatch=%llu tracks=%zu shared-tracks=%zu pending=%zu\n",
        static_cast<unsigned long long>(g_frame),
        static_cast<unsigned long long>(g_trace_entries.exchange(0U)),
        static_cast<unsigned long long>(g_trace_matches.exchange(0U)),
        static_cast<unsigned long long>(g_trace_new_tracks.exchange(0U)),
        static_cast<unsigned long long>(g_trace_conflicts.exchange(0U)),
        static_cast<unsigned long long>(g_trace_ambiguous_rejects.exchange(0U)),
        static_cast<unsigned long long>(g_trace_shared_samples.exchange(0U)),
        static_cast<unsigned long long>(g_trace_shared_matches.exchange(0U)),
        static_cast<unsigned long long>(g_trace_shared_new_tracks.exchange(0U)),
        static_cast<unsigned long long>(g_trace_shared_rejects.exchange(0U)),
        static_cast<unsigned long long>(g_trace_task_matches.exchange(0U)),
        static_cast<unsigned long long>(g_trace_task_misses.exchange(0U)),
        static_cast<unsigned long long>(g_trace_sidecar_mismatches.exchange(0U)),
        g_tracks.size(), g_shared_matrix_tracks.size(), g_submitted.size());

    const std::uint64_t v35_samples =
        g_trace_specific_samples.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v35_matches =
        g_trace_specific_matches.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v35_new =
        g_trace_specific_new.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v35_conflicts =
        g_trace_specific_conflicts.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v35_unowned =
        g_trace_unowned_matrix_snaps.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v35_background =
        g_trace_background_identities.exchange(0U, std::memory_order_relaxed);
    char v35_line[512]{};
    std::snprintf(v35_line, sizeof(v35_line),
        "[rocket-interpolation-v35] frame=%llu specific-samples=%llu specific-match=%llu specific-new=%llu specific-conflict=%llu background-id=%llu unowned-snap=%llu continuity=%zu\n",
        static_cast<unsigned long long>(g_frame),
        static_cast<unsigned long long>(v35_samples),
        static_cast<unsigned long long>(v35_matches),
        static_cast<unsigned long long>(v35_new),
        static_cast<unsigned long long>(v35_conflicts),
        static_cast<unsigned long long>(v35_background),
        static_cast<unsigned long long>(v35_unowned),
        g_specific_continuity.size());
    std::fputs(v35_line, stderr);
    if (std::FILE* v35_file = std::fopen("Rocket-R-interpolation-coverage.log", "a")) {
        std::fputs(v35_line, v35_file);
        std::fclose(v35_file);
    }

    const std::uint64_t v36_lifetimes =
        g_trace_v36_object_lifetimes.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v36_ranges =
        g_trace_v36_model_range_matrices.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v36_overlap =
        g_trace_v36_model_range_specific_overlap.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v36_direct =
        g_trace_v36_direct_samples.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v36_direct_match =
        g_trace_v36_direct_matches.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v36_direct_new =
        g_trace_v36_direct_new.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v36_camera =
        g_trace_v36_camera_matrices.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v36_epochs =
        g_trace_v36_camera_epochs.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v36_unowned_camera =
        g_trace_v36_unowned_camera.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v36_unowned_arena =
        g_trace_v36_unowned_arena.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v36_unowned_other =
        g_trace_v36_unowned_other.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v36_unowned_total =
        v36_unowned_camera + v36_unowned_arena + v36_unowned_other;
    char v36_line[768]{};
    std::snprintf(v36_line, sizeof(v36_line),
        "[rocket-interpolation-v36] frame=%llu object-lifetime-new=%llu model-range=%llu range-v35-overlap=%llu direct-samples=%llu direct-match=%llu direct-new=%llu camera-matrices=%llu camera-epochs=%llu unowned-total=%llu unowned-camera=%llu unowned-arena=%llu unowned-other=%llu direct-continuity=%zu objects=%zu\n",
        static_cast<unsigned long long>(g_frame),
        static_cast<unsigned long long>(v36_lifetimes),
        static_cast<unsigned long long>(v36_ranges),
        static_cast<unsigned long long>(v36_overlap),
        static_cast<unsigned long long>(v36_direct),
        static_cast<unsigned long long>(v36_direct_match),
        static_cast<unsigned long long>(v36_direct_new),
        static_cast<unsigned long long>(v36_camera),
        static_cast<unsigned long long>(v36_epochs),
        static_cast<unsigned long long>(v36_unowned_total),
        static_cast<unsigned long long>(v36_unowned_camera),
        static_cast<unsigned long long>(v36_unowned_arena),
        static_cast<unsigned long long>(v36_unowned_other),
        g_direct_continuity.size(), g_object_lifetimes.size());
    std::fputs(v36_line, stderr);
    if (std::FILE* v36_file = std::fopen("Rocket-R-interpolation-coverage.log", "a")) {
        std::fputs(v36_line, v36_file);
        std::fclose(v36_file);
    }

    const std::uint64_t v37_collectible_rigid =
        g_trace_v37_collectible_rigid.exchange(0U, std::memory_order_relaxed);
    char v37_line[256]{};
    std::snprintf(v37_line, sizeof(v37_line),
        "[rocket-interpolation-v37] frame=%llu collectible-rigid-matrices=%llu\n",
        static_cast<unsigned long long>(g_frame),
        static_cast<unsigned long long>(v37_collectible_rigid));
    std::fputs(v37_line, stderr);
    if (std::FILE* v37_file = std::fopen("Rocket-R-interpolation-coverage.log", "a")) {
        std::fputs(v37_line, v37_file);
        std::fclose(v37_file);
    }

    // === ROCKET-R INTERPOLATION V38 COLLECTIBLE BILLBOARD VERTEX COVERAGE ===
    const std::uint64_t v38_vertex =
        g_trace_v38_collectible_vertex.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v38_m0 =
        g_trace_v38_mode0.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v38_m1 =
        g_trace_v38_mode1.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v38_m2 =
        g_trace_v38_mode2.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v38_m3 =
        g_trace_v38_mode3.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v38_m4 =
        g_trace_v38_mode4.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v38_m5 =
        g_trace_v38_mode5.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v38_other =
        g_trace_v38_mode_other.exchange(0U, std::memory_order_relaxed);
    char v38_line[384]{};
    std::snprintf(v38_line, sizeof(v38_line),
        "[rocket-interpolation-v38] frame=%llu collectible-vertex=%llu modes=0:%llu,1:%llu,2:%llu,3:%llu,4:%llu,5:%llu,other:%llu\n",
        static_cast<unsigned long long>(g_frame),
        static_cast<unsigned long long>(v38_vertex),
        static_cast<unsigned long long>(v38_m0),
        static_cast<unsigned long long>(v38_m1),
        static_cast<unsigned long long>(v38_m2),
        static_cast<unsigned long long>(v38_m3),
        static_cast<unsigned long long>(v38_m4),
        static_cast<unsigned long long>(v38_m5),
        static_cast<unsigned long long>(v38_other));
    std::fputs(v38_line, stderr);
    if (std::FILE* v38_file = std::fopen("Rocket-R-interpolation-coverage.log", "a")) {
        std::fputs(v38_line, v38_file);
        std::fclose(v38_file);
    }

    const std::uint64_t v39_owner_pos =
        g_trace_v39_owner_position.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v39_fallback =
        g_trace_v39_matrix_fallback.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v39_matches =
        g_trace_v39_matches.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v39_new =
        g_trace_v39_new.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v39_conflicts =
        g_trace_v39_conflicts.exchange(0U, std::memory_order_relaxed);
    char v39_line[320]{};
    std::snprintf(v39_line, sizeof(v39_line),
        "[rocket-interpolation-v39] frame=%llu owner-pos=%llu matrix-fallback=%llu collectible-match=%llu collectible-new=%llu collectible-conflict=%llu\n",
        static_cast<unsigned long long>(g_frame),
        static_cast<unsigned long long>(v39_owner_pos),
        static_cast<unsigned long long>(v39_fallback),
        static_cast<unsigned long long>(v39_matches),
        static_cast<unsigned long long>(v39_new),
        static_cast<unsigned long long>(v39_conflicts));
    std::fputs(v39_line, stderr);
    if (std::FILE* v39_file =
            std::fopen("Rocket-R-interpolation-coverage.log", "a")) {
        std::fputs(v39_line, v39_file);
        std::fclose(v39_file);
    }


    const std::uint64_t v42_token_draws =
        g_trace_v42_token_draws.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t v42_token_matrices =
        g_trace_v42_token_matrices.exchange(0U, std::memory_order_relaxed);
    char v42_line[256]{};
    std::snprintf(v42_line, sizeof(v42_line),
        "[rocket-interpolation-v42] frame=%llu token-draws=%llu mode0-matrices=%llu\n",
        static_cast<unsigned long long>(g_frame),
        static_cast<unsigned long long>(v42_token_draws),
        static_cast<unsigned long long>(v42_token_matrices));
    std::fputs(v42_line, stderr);
    if (std::FILE* v42_file =
            std::fopen("Rocket-R-interpolation-coverage.log", "a")) {
        std::fputs(v42_line, v42_file);
        std::fclose(v42_file);
    }
    char v45_line[320]{};
    std::snprintf(v45_line, sizeof(v45_line),
        "[rocket-interpolation-v45] frame=%llu mode0=%llu scoped-submodels=%llu ambiguous-snaps=%llu invalid-rigid-snaps=%llu\n",
        static_cast<unsigned long long>(g_frame),
        static_cast<unsigned long long>(g_trace_v45_mode0.exchange(0U, std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_trace_v45_scoped_submodels.exchange(0U, std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_trace_v45_ambiguous.exchange(0U, std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_trace_v45_invalid.exchange(0U, std::memory_order_relaxed)));
    std::fputs(v45_line, stderr);
    if (std::FILE* file = std::fopen("Rocket-R-interpolation-coverage.log", "a")) {
        std::fputs(v45_line, file);
        std::fclose(file);
    }
}

} // namespace

rocket::presentation::CoverageStats rocket::presentation::coverage_stats() {
    CoverageStats stats{};
    stats.entries = g_trace_entries.load(std::memory_order_relaxed);
    stats.matches = g_trace_matches.load(std::memory_order_relaxed);
    stats.new_tracks = g_trace_new_tracks.load(std::memory_order_relaxed);
    stats.conflicts = g_trace_conflicts.load(std::memory_order_relaxed);
    stats.ambiguous = g_trace_ambiguous_rejects.load(std::memory_order_relaxed);
    stats.shared_samples = g_trace_shared_samples.load(std::memory_order_relaxed);
    stats.shared_matches = g_trace_shared_matches.load(std::memory_order_relaxed);
    stats.shared_rejects = g_trace_shared_rejects.load(std::memory_order_relaxed);
    stats.task_matches = g_trace_task_matches.load(std::memory_order_relaxed);
    stats.task_misses = g_trace_task_misses.load(std::memory_order_relaxed);
    stats.sidecar_mismatches = g_trace_sidecar_mismatches.load(std::memory_order_relaxed);
    stats.semantic_bindings = g_coverage_semantic_bindings.load(std::memory_order_relaxed);
    stats.snapped_bindings = g_coverage_snapped_bindings.load(std::memory_order_relaxed);
    stats.dynamic_vertex_bindings = g_coverage_dynamic_vertex_bindings.load(std::memory_order_relaxed);
    return stats;
}

rocket::presentation::TaskIdentityScope::TaskIdentityScope(
    std::uint8_t* rdram_snapshot, std::uint32_t display_list_address) {
    g_active_matrices.clear();
    g_active_background_ranges.clear();
    g_active_context_dl_start = 0U;
    g_active_context_size = 0U;
    g_active_camera_token = 0U;
    g_active_task_address = 0U;
    g_active_task_fail_closed = true;
    const std::uint32_t physical = Physical(display_list_address);

    std::uint32_t task_address = 0U;
    std::uint32_t context_dl_start = 0U;
    std::uint32_t context_size = 0U;
    if (rdram_snapshot != nullptr) {
        const std::uint32_t task = ReadCurrentGfxTask(rdram_snapshot);
        if (task != 0U) {
            task_address = Physical(task);
            const std::uint32_t buffer = ReadU32(
                rdram_snapshot, task + kGfxTaskCtxDlStartOffset);
            context_dl_start = ValidRange(buffer, 8U) ? Physical(buffer) : 0U;
            context_size = ReadU32(
                rdram_snapshot, task + kGfxTaskCtxSizeOffset);
        }
    }

    std::scoped_lock lock(g_mutex);
    if (g_submitted.empty()) {
        g_trace_task_misses.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    // The renderer queue is FIFO. Never scan forward for another sidecar with
    // the same recycled display-list address: doing so can attach frame N+2's
    // identities to frame N and produce exactly the one-frame geometry burst
    // seen in the recording. The immutable snapshot must agree with the oldest
    // submitted sidecar or this task is treated as untrusted.
    SubmittedFrame& frame = g_submitted.front();
    const bool match = frame.display_list == physical &&
        (frame.task_address == 0U || task_address == 0U ||
         frame.task_address == task_address) &&
        (frame.context_dl_start == 0U || context_dl_start == 0U ||
         frame.context_dl_start == context_dl_start) &&
        (frame.context_size == 0U || context_size == 0U ||
         frame.context_size == context_size);
    if (!match) {
        g_trace_task_misses.fetch_add(1U, std::memory_order_relaxed);
        g_trace_sidecar_mismatches.fetch_add(1U, std::memory_order_relaxed);
        if (TraceEnabled()) {
            std::fprintf(stderr,
                "[rocket-presentation] sidecar mismatch: expected dl=%06X task=%06X ctx=%06X/%u got dl=%06X task=%06X ctx=%06X/%u; pending history reset\n",
                frame.display_list, frame.task_address, frame.context_dl_start,
                frame.context_size, physical, task_address, context_dl_start,
                context_size);
        }
        g_submitted.clear();
        g_specific_continuity.clear();
        g_direct_continuity.clear();
        g_object_lifetimes.clear();
        g_camera_continuity = {};
        g_recording_camera_token = 0U;
        g_tracks.clear();
        g_shared_matrix_tracks.clear();
        return;
    }

    g_active_matrices = std::move(frame.matrices);
    g_active_background_ranges = std::move(frame.background_ranges);
    g_active_context_dl_start = frame.context_dl_start;
    g_active_context_size = frame.context_size;
    g_active_camera_token = frame.camera_token;
    g_active_task_address = frame.task_address;
    g_submitted.erase(g_submitted.begin());
    g_trace_task_matches.fetch_add(1U, std::memory_order_relaxed);
}

rocket::presentation::TaskIdentityScope::~TaskIdentityScope() {
    g_active_matrices.clear();
    g_active_background_ranges.clear();
    g_active_context_dl_start = 0U;
    g_active_context_size = 0U;
    g_active_camera_token = 0U;
    g_active_task_address = 0U;
    g_active_task_fail_closed = false;
}

void rocket::presentation::mod_matrix(std::uint32_t address,std::uint32_t handle) {
    if(handle==0||!ValidRange(address,64))return;
    std::scoped_lock lock(g_mutex);
    if(g_mod_matrices.size()<256)g_mod_matrices.emplace_back(Physical(address),handle);
}

bool rocket::presentation::matrix_binding(
    std::uint32_t physical_matrix_address, MatrixBinding& binding) {
    const auto found = g_active_matrices.find(physical_matrix_address & kRdramMask);
    if (found != g_active_matrices.end()) {
        binding = found->second.binding;
        // identity==0 is an explicit G_EX_ID_IGNORE binding, not a miss.
        if (binding.identity == 0U) {
            g_coverage_snapped_bindings.fetch_add(1U, std::memory_order_relaxed);
        } else if (binding.identity != 0xFFFFFFFFU) {
            g_coverage_semantic_bindings.fetch_add(1U, std::memory_order_relaxed);
            if (binding.interpolate_vertices) {
                g_coverage_dynamic_vertex_bindings.fetch_add(1U, std::memory_order_relaxed);
            }
        }
        return binding.identity != 0xFFFFFFFFU;
    }

    // The task scope is authoritative. If Rocket did not prove the semantic
    // owner of this matrix, do not let RT64 guess from draw order/material
    // similarity. Present the newest authored transform for this matrix.
    if (g_active_task_fail_closed) {
        const std::uint32_t physical = physical_matrix_address & kRdramMask;
        const bool camera_matrix = g_active_task_address != 0U &&
            (physical == g_active_task_address + kGfxTaskPerspectiveMtxOffset ||
             physical == g_active_task_address + kGfxTaskViewMtxOffset ||
             physical == g_active_task_address + kGfxTaskIdentityModelMtxOffset);
        const std::uint64_t arena_begin = g_active_context_dl_start;
        const std::uint64_t arena_end = arena_begin +
            static_cast<std::uint64_t>(g_active_context_size);
        if (camera_matrix) {
            g_trace_v36_unowned_camera.fetch_add(1U, std::memory_order_relaxed);
        } else if (g_active_context_size != 0U && physical >= arena_begin &&
                   static_cast<std::uint64_t>(physical) < arena_end) {
            g_trace_v36_unowned_arena.fetch_add(1U, std::memory_order_relaxed);
        } else {
            g_trace_v36_unowned_other.fetch_add(1U, std::memory_order_relaxed);
        }
        g_trace_unowned_matrix_snaps.fetch_add(1U, std::memory_order_relaxed);
        binding = IgnoredBinding();
        g_coverage_snapped_bindings.fetch_add(1U, std::memory_order_relaxed);
        return true;
    }
    return false;
}

extern "C" bool rocket_presentation_matrix_binding(
    std::uint32_t physical_matrix_address,
    RocketPresentationMatrixBinding* out_binding) {
    if (out_binding == nullptr) return false;
    rocket::presentation::MatrixBinding binding{};
    if (!rocket::presentation::matrix_binding(physical_matrix_address, binding)) {
        return false;
    }
    out_binding->identity = binding.identity;
    out_binding->interpolate_vertices = binding.interpolate_vertices ? 1U : 0U;
    out_binding->interpolate_texcoords = binding.interpolate_texcoords ? 1U : 0U;
    out_binding->interpolate_tiles = binding.interpolate_tiles ? 1U : 0U;
    // reserved bit 0 is v37 rigid-decomposition intent for exact collectibles.
    out_binding->reserved = (binding.rigid_decompose ? 1U : 0U) | (binding.interpolate_shape ? 2U : 0U);
    return true;
}

// ROCKET-R SKYBOX INTERPOLATION V33
extern "C" bool rocket_presentation_background_display_list(
    std::uint32_t physical_command_address,
    std::uint32_t physical_target_address,
    std::uint32_t* out_identity) {
    if (out_identity == nullptr || !g_active_task_fail_closed) return false;
    BackgroundCommandLocation command{};
    if (!BackgroundRangeLocation(physical_command_address, command)) return false;

    // v35 range-stable background identity: source location is relative to the
    // captured pre-world background range, not to the alternating whole task
    // arena. A variable amount of earlier frame work can no longer rename the sky.
    const std::uint32_t target_key =
        CanonicalBackgroundTarget(physical_target_address);
    std::uint64_t key = Mix64(0x524F434B4554534BULL ^
        (static_cast<std::uint64_t>(command.range_index) << 32U) ^
        static_cast<std::uint64_t>(command.offset));
    key = Mix64(key ^ (static_cast<std::uint64_t>(target_key) << 1U));
    if (g_active_camera_token != 0U) {
        key = Mix64(key ^
            (static_cast<std::uint64_t>(g_active_camera_token) << 24U));
    }
    const std::uint32_t identity = NormalizeIdentity(key);
    *out_identity = identity;
    if (identity != 0U && identity != 0xFFFFFFFFU) {
        g_trace_background_identities.fetch_add(1U, std::memory_order_relaxed);
        return true;
    }
    return false;
}

extern "C" void rocket_presentation_frame_begin(std::uint8_t*,
                                                  recomp_context*) {
    std::scoped_lock lock(g_mutex);
    g_mod_matrices.clear();
    g_specific_samples.clear();
    g_submodel_matrix_capture = {};
    g_model_range_samples.clear();
    g_model_range_depth = 0U;
    g_model_range_overflow_depth = 0U;
    g_direct_samples.clear();
    g_pending_callsite = {};
    g_entered_callsite = {};
    g_shared_mode0_v43 = {};
    g_tinker_token_draw_owner = 0U;
    g_tinker_token_draw_position_valid = false;
    for (DirectCapture& capture : g_direct_captures) capture = {};
    g_camera_samples.clear();
    g_recording_camera_token = 0U;
    g_background_ranges.clear();
    g_background_capture_active = false;
    g_background_capture_begin = 0U;
    ++g_frame;
    g_sky_capture = {};
    g_trace_wheel_matrix = 0;
    g_entries.clear();
    g_key_ordinals.clear();
    MaybeTraceSummary();
}

extern "C" void rocket_presentation_background_begin(std::uint8_t* rdram,
                                                       recomp_context*) {
    std::uint32_t head = 0U;
    if (!ReadCurrentDlHeadPhysical(rdram, head)) return;
    std::scoped_lock lock(g_mutex);
    g_background_capture_active = true;
    g_background_capture_begin = head;
}

extern "C" void rocket_presentation_background_end(std::uint8_t* rdram,
                                                     recomp_context*) {
    std::uint32_t head = 0U;
    if (!ReadCurrentDlHeadPhysical(rdram, head)) return;
    std::scoped_lock lock(g_mutex);
    if (!g_background_capture_active) return;
    const std::uint32_t begin = g_background_capture_begin;
    g_background_capture_active = false;
    g_background_capture_begin = 0U;
    if (head <= begin) return;
    // Rocket's authored command arena is small; reject a nonsensical wrap or
    // stale pointer instead of classifying unrelated world/HUD commands.
    if ((head - begin) > 0x00080000U) return;
    g_background_ranges.push_back(BackgroundCommandRange{begin, head});
}

extern "C" void rocket_presentation_sky_rows(std::uint8_t* rdram, recomp_context* context) {
    // func_8008AEA0, before 0x8008B034: s3 is the sky texture header,
    // t1 is the clamped load row and f0 is the unrounded camera displacement.
    const auto texture = static_cast<std::uint32_t>(context->r19);
    if (!ValidRange(texture, 0x18U)) return;
    const int height = ReadU16(rdram, texture + 2U);
    const int loaded = static_cast<int>(context->r20);
    if (height < 241 || loaded < 240 || loaded > 4096) return;
    const auto data = ReadU32(rdram, texture + 0x10U);
    const bool continuous = g_previous_sky.valid && g_previous_sky.frame + 1U == g_frame &&
        g_previous_sky.texture == data && g_previous_sky.camera == g_recording_camera_token;
    // The game's camera field has already been restored after projection setup.
    // Project pitch using the rendered FOV, so the horizon follows the world.
    const auto camera = static_cast<std::uint32_t>(context->r16);
    float displacement = context->f0.fl;
    if (ValidRange(camera, 0xA4U)) {
        const float authored_fov = std::bit_cast<float>(ReadU32(rdram, camera + 0xA0U));
        const float rendered_fov = rocket::graphics::effective_fov_radians(authored_fov);
        displacement = rocket::presentation::sky_pitch_displacement(displacement, authored_fov, rendered_fov);
    }
    const float requested = std::clamp(float((height - 240) / 2) - displacement, 0.0F, float(height - 241));
    g_sky_capture.rows = rocket::presentation::sky_rows(static_cast<int>(requested),
        requested, g_previous_sky.rows.current,
        height, loaded, continuous);
    if (TraceEnabled() && continuous && std::abs(requested - g_previous_sky.rows.current) > 8.0F) {
        std::fprintf(stderr, "[rocket-sky-motion] row=%.2f previous=%.2f load=%d height=%d rows=%d reset=%d\n",
            requested, g_previous_sky.rows.current, g_sky_capture.rows.load, height, loaded,
            int(g_sky_capture.rows.previous == g_sky_capture.rows.current));
    }
    g_sky_capture.texture = data;
    g_sky_capture.camera = g_recording_camera_token;
    g_sky_capture.frame = g_frame;
    g_sky_capture.valid = true;
    // Keep the visible rows unchanged while making room for interpolation taps.
    context->r9 = g_sky_capture.rows.load;
}

extern "C" void rocket_presentation_sky_uv(std::uint8_t*, recomp_context* context) {
    if (g_sky_capture.valid) {
        context->r2 += (g_sky_capture.rows.origin - g_sky_capture.rows.load) * 32;
    }
}

extern "C" void rocket_presentation_sky_end(std::uint8_t* rdram, recomp_context* context) {
    rocket_presentation_background_end(rdram, context);
    if (!g_sky_capture.valid || g_background_ranges.empty()) return;
    auto& range = g_background_ranges.back();
    range.scrolling_rectangle = true;
    range.current_row_offset = g_sky_capture.rows.current - g_sky_capture.rows.origin;
    range.previous_row_offset = g_sky_capture.rows.previous - g_sky_capture.rows.origin;
    g_previous_sky = g_sky_capture;
}

extern "C" bool rocket_presentation_sky_rectangle(std::uint32_t command, float* current, float* previous) {
    BackgroundCommandLocation location{};
    if (!current || !previous || !g_active_task_fail_closed || !BackgroundRangeLocation(command, location)) return false;
    const auto& range = g_active_background_ranges[location.range_index];
    if (!range.scrolling_rectangle) return false;
    *current = range.current_row_offset;
    *previous = range.previous_row_offset;
    g_trace_background_identities.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

extern "C" void rocket_presentation_callsite(
    recomp_context* context, std::uint32_t target, std::uint32_t address) {
    g_pending_callsite = {context, target, address};
}

extern "C" void rocket_presentation_enter_call(
    recomp_context* context, std::uint32_t target) {
    // Consume once, including on mismatch. An uninstrumented/indirect call
    // must never borrow a previous call's provenance.
    const CallsiteCapture pending = std::exchange(g_pending_callsite, {});
    g_entered_callsite = {};
    if (context != nullptr && pending.context == context && pending.target == target) {
        g_entered_callsite = pending;
    }
}

extern "C" void rocket_presentation_model_range_begin(
    std::uint8_t* rdram, recomp_context* context) {
    if (g_model_range_overflow_depth != 0U) {
        ++g_model_range_overflow_depth;
        return;
    }
    if (g_model_range_depth >= 16U) {
        ++g_model_range_overflow_depth;
        return;
    }
    ModelRangeCapture& capture = g_model_range_captures[g_model_range_depth++];
    capture = {};
    if (rdram == nullptr || context == nullptr ||
        !ValidRange(kGfxContextMtxHeadAddress, 4U)) return;
    const std::uint32_t head = ReadU32(rdram, kGfxContextMtxHeadAddress);
    if (!ValidRange(head - kMtxBytes, kMtxBytes)) return;
    capture.object = static_cast<std::uint32_t>(context->r4);
    if (!ValidRange(capture.object, kGameObjectSubmodelCountOffset + 4U)) return;
    capture.start_head = head;
    capture.caller = EntryCallsite(context, 0x8001ECECU);
    if (capture.caller == 0U) return;
    {
        std::scoped_lock lock(g_mutex);
        capture.specific_begin = g_specific_samples.size();
    }
    capture.valid = true;
}

extern "C" void rocket_presentation_model_range_end(
    std::uint8_t* rdram, recomp_context* context) {
    if (g_model_range_overflow_depth != 0U) {
        --g_model_range_overflow_depth;
        return;
    }
    if (g_model_range_depth == 0U) return;
    const ModelRangeCapture capture =
        g_model_range_captures[--g_model_range_depth];
    if (!capture.valid || rdram == nullptr || context == nullptr) return;

    const std::uint32_t current = ReadU32(rdram, kGfxContextMtxHeadAddress);
    if (current > capture.start_head) return;
    const std::uint32_t bytes = capture.start_head - current;
    if ((bytes % kMtxBytes) != 0U) return;
    const std::size_t matrix_count = bytes / kMtxBytes;
    if (matrix_count == 0U || matrix_count > kMaximumModelRangeMatrices) return;

    // The return hook runs after s3/r19 has been restored to its caller's
    // value. Use the argument saved at entry and still require an exact
    // Submodel capture from this invocation/range to prove ownership.
    const std::uint32_t object = capture.object;
    if (!ValidRange(object, kGameObjectSubmodelCountOffset + 4U)) return;
    const std::uint32_t submodels = ReadU32(rdram, object + kGameObjectSubmodelsOffset);
    const std::uint32_t count = ReadU32(rdram, object + kGameObjectSubmodelCountOffset);
    if (count == 0U || count > 512U ||
        !ValidRange(submodels, count * kSubmodelBytes)) return;

    std::scoped_lock lock(g_mutex);
    const std::uint32_t owner_physical = Physical(object);
    bool owner_proven = false;
    std::uint64_t proof_key = 0U;
    const std::size_t first = std::min(
        capture.specific_begin, g_specific_samples.size());
    const std::uint32_t range_begin = Physical(current);
    const std::uint32_t range_end = Physical(capture.start_head);
    for (std::size_t index = first; index < g_specific_samples.size(); ++index) {
        const SpecificMatrixSample& exact = g_specific_samples[index];
        const std::uint32_t physical = Physical(exact.address);
        if (exact.owner == owner_physical && physical >= range_begin &&
            physical < range_end) {
            owner_proven = true;
            proof_key = exact.key;
            break;
        }
    }
    if (!owner_proven || proof_key == 0U) return;

    const std::uint32_t lifetime = AcquireObjectLifetimeTokenLocked(rdram, object);
    if (lifetime == 0U) return;
    std::uint64_t invocation_key = Mix64(0x4D4F44454C524E47ULL ^
        static_cast<std::uint64_t>(lifetime));
    invocation_key = Mix64(invocation_key ^
        (static_cast<std::uint64_t>(capture.caller) << 1U));
    // Reuse v35's exact proved Submodel key as the invocation discriminator.
    // This is stronger than a new per-frame ordinal and avoids adding another
    // independent draw-order assumption to the broad object range.
    invocation_key = Mix64(invocation_key ^ (proof_key << 1U));

    for (std::size_t ordinal = 0U; ordinal < matrix_count; ++ordinal) {
        const std::uint32_t address = capture.start_head -
            static_cast<std::uint32_t>((ordinal + 1U) * kMtxBytes);
        if (!ValidRange(address, kMtxBytes)) continue;
        const std::uint64_t identity_key = Mix64(invocation_key ^
            (static_cast<std::uint64_t>(ordinal) << 56U));
        const std::uint32_t identity = NormalizeIdentity(identity_key);
        g_model_range_samples.push_back({address, identity});
        g_trace_v36_model_range_matrices.fetch_add(1U, std::memory_order_relaxed);
    }
}

extern "C" void rocket_presentation_direct_begin(
    std::uint8_t* rdram, recomp_context* context, std::uint32_t kind) {
    if (kind == 0U || kind >= 3U) return;
    g_direct_captures[kind] = {};
    if (rdram == nullptr || context == nullptr ||
        !ValidRange(kGfxContextMtxHeadAddress, 4U)) return;
    const std::uint32_t head = ReadU32(rdram, kGfxContextMtxHeadAddress);
    if (!ValidRange(head - kMtxBytes, kMtxBytes)) return;
    DirectCapture capture{};
    capture.start_head = head;
    capture.caller = EntryCallsite(context, kind == 1U ? 0x800476CCU : 0x8004A4F0U);
    if (capture.caller == 0U) return;
    // func_8004A4F0's r4 is a durable effect/renderer owner. The translation
    // helper's r4 often points at transient stack data, so do not key on it.
    const std::uint32_t owner_candidate = static_cast<std::uint32_t>(context->r4);
    capture.owner = kind == 2U && ValidRange(owner_candidate, 4U)
        ? Physical(owner_candidate) : 0U;
    capture.valid = true;
    g_direct_captures[kind] = capture;
}

extern "C" void rocket_presentation_direct_end(
    std::uint8_t* rdram, recomp_context* context, std::uint32_t kind) {
    if (kind == 0U || kind >= 3U) return;
    const DirectCapture capture = std::exchange(
        g_direct_captures[kind], DirectCapture{});
    if (!capture.valid || rdram == nullptr || context == nullptr) return;
    const std::uint32_t current = ReadU32(rdram, kGfxContextMtxHeadAddress);
    if (current > capture.start_head) return;
    const std::uint32_t bytes = capture.start_head - current;
    if ((bytes % kMtxBytes) != 0U) return;
    const std::size_t matrix_count = bytes / kMtxBytes;
    if (matrix_count == 0U || matrix_count > kMaximumDirectRangeMatrices) return;

    std::scoped_lock lock(g_mutex);
    std::uint64_t base = Mix64(0x4449524543544D58ULL ^
        static_cast<std::uint64_t>(kind));
    base = Mix64(base ^ (static_cast<std::uint64_t>(capture.caller) << 8U));
    base = Mix64(base ^ (static_cast<std::uint64_t>(capture.owner) << 24U));

    for (std::size_t ordinal = 0; ordinal < matrix_count; ++ordinal) {
        const std::uint32_t address = capture.start_head -
            static_cast<std::uint32_t>((ordinal + 1U) * kMtxBytes);
        if (!ValidRange(address, kMtxBytes)) continue;
        DirectMatrixSample sample{};
        sample.key = Mix64(base ^ (static_cast<std::uint64_t>(ordinal) << 56U));
        sample.address = address;
        sample.kind = kind;
        sample.role = static_cast<std::uint32_t>(ordinal);
        sample.position_valid = ReadMtxTranslation(rdram, address, sample.position);
        g_direct_samples.push_back(sample);
        g_trace_v36_direct_samples.fetch_add(1U, std::memory_order_relaxed);
    }
}

extern "C" void rocket_presentation_camera_source(
    std::uint8_t*, recomp_context* context, std::uint32_t discontinuity) {
    if (context == nullptr) return;
    const std::uint32_t camera = static_cast<std::uint32_t>(context->r4);
    std::scoped_lock lock(g_mutex);
    g_recording_camera_token = 0U;
    if (!ValidRange(camera, 0xB0U)) return;

    bool new_epoch = g_camera_continuity.token == 0U ||
        g_camera_continuity.camera != Physical(camera);
    if (g_camera_continuity.last_frame != g_frame) {
        if (g_camera_continuity.last_frame + 1U != g_frame || discontinuity != 0U) {
            new_epoch = true;
        }
    }
    if (new_epoch) {
        g_camera_continuity.token = NextSpecificPresentationToken();
        g_trace_v36_camera_epochs.fetch_add(1U, std::memory_order_relaxed);
    }
    g_camera_continuity.camera = Physical(camera);
    g_camera_continuity.last_frame = g_frame;
    g_recording_camera_token = g_camera_continuity.token;
}

extern "C" void rocket_presentation_camera_matrices(
    std::uint8_t* rdram, recomp_context*) {
    if (rdram == nullptr) return;
    std::scoped_lock lock(g_mutex);
    if (g_recording_camera_token == 0U) return;
    const std::uint32_t task = ReadCurrentGfxTask(rdram);
    if (task == 0U) return;
    const std::uint32_t offsets[] = {
        kGfxTaskPerspectiveMtxOffset,
        kGfxTaskViewMtxOffset,
        kGfxTaskIdentityModelMtxOffset,
    };
    for (std::uint32_t role = 0U; role < 3U; ++role) {
        const std::uint32_t address = task + offsets[role];
        if (!ValidRange(address, kMtxBytes)) continue;
        const std::uint32_t identity = MatrixIdentity(
            g_recording_camera_token, 0x70U + role);
        g_camera_samples.push_back({address, identity});
        g_trace_v36_camera_matrices.fetch_add(1U, std::memory_order_relaxed);
    }
}

extern "C" void rocket_presentation_submodel_matrix_begin(
    std::uint8_t* rdram, recomp_context* context) {
    g_submodel_matrix_capture = {};
    if (rdram == nullptr || context == nullptr) return;

    const std::uint32_t object = static_cast<std::uint32_t>(context->r4);
    const std::uint32_t submodel = static_cast<std::uint32_t>(context->r5);
    if (!ValidRange(object, kGameObjectSubmodelCountOffset + 4U) ||
        !ValidRange(submodel, kSubmodelBytes)) return;

    const std::uint32_t submodels =
        ReadU32(rdram, object + kGameObjectSubmodelsOffset);
    const std::uint32_t count =
        ReadU32(rdram, object + kGameObjectSubmodelCountOffset);
    if (count == 0U || count > 512U || submodel < submodels ||
        !ValidRange(submodels, count * kSubmodelBytes)) return;
    const std::uint32_t delta = submodel - submodels;
    if ((delta % kSubmodelBytes) != 0U) return;
    const std::uint32_t index = delta / kSubmodelBytes;
    if (index >= count) return;

    const std::uint32_t object_class =
        ReadU32(rdram, object + kGameObjectClassOffset);
    const std::uint32_t submodel_gfx = ReadU32(rdram, submodel);
    const bool scoped_instance = g_shared_mode0_v43.active &&
        g_shared_mode0_v43.model == Physical(object);
    std::uint64_t base_key = Mix64(0x5355424D41545258ULL ^
        (scoped_instance ? g_shared_mode0_v43.instance_key : Physical(object)));
    base_key = Mix64(base_key ^ (static_cast<std::uint64_t>(index) << 32U));
    base_key = Mix64(base_key ^
        (static_cast<std::uint64_t>(object_class) << 1U));
    base_key = Mix64(base_key ^
        (static_cast<std::uint64_t>(submodel_gfx) << 17U));

    std::scoped_lock lock(g_mutex);
    std::uint64_t key = base_key;
    if (key == 0U) key = 1U;
    const bool collectible = IsRigidCollectibleObject(rdram, object);
    const std::uint8_t submodel_mode = static_cast<std::uint8_t>(
        MEM_B(0, RdramAddress(submodel + kSubmodelPresentationModeOffset)));
    const bool collectible_billboard = collectible &&
        submodel_mode >= kCameraRelativeSubmodelModeMin &&
        submodel_mode <= kCameraRelativeSubmodelModeMax;

    if (collectible) {
        switch (submodel_mode) {
            case 0U: g_trace_v38_mode0.fetch_add(1U, std::memory_order_relaxed); break;
            case 1U: g_trace_v38_mode1.fetch_add(1U, std::memory_order_relaxed); break;
            case 2U: g_trace_v38_mode2.fetch_add(1U, std::memory_order_relaxed); break;
            case 3U: g_trace_v38_mode3.fetch_add(1U, std::memory_order_relaxed); break;
            case 4U: g_trace_v38_mode4.fetch_add(1U, std::memory_order_relaxed); break;
            case 5U: g_trace_v38_mode5.fetch_add(1U, std::memory_order_relaxed); break;
            default: g_trace_v38_mode_other.fetch_add(1U, std::memory_order_relaxed); break;
        }
    }

    g_submodel_matrix_capture = {
        key, Physical(object), collectible, collectible_billboard,
        submodel_mode, true};
    if (scoped_instance) {
        g_submodel_matrix_capture.projected_shadow = g_shared_mode0_v43.projected_shadow;
        g_submodel_matrix_capture.instance_position = g_shared_mode0_v43.position;
        g_submodel_matrix_capture.instance_position_valid = g_shared_mode0_v43.position_valid;
        g_trace_v45_scoped_submodels.fetch_add(1U, std::memory_order_relaxed);
    }
}

extern "C" void rocket_presentation_submodel_matrix_end(
    std::uint8_t* rdram, recomp_context* context) {
    const SubmodelMatrixCapture capture = std::exchange(
        g_submodel_matrix_capture, SubmodelMatrixCapture{});
    if (!capture.valid || rdram == nullptr || context == nullptr) return;

    const std::uint32_t matrix = static_cast<std::uint32_t>(context->r2);
    if (!ValidRange(matrix, kMtxBytes)) return;
    SpecificMatrixSample sample{};
    sample.key = capture.key;
    sample.address = matrix;
    sample.owner = capture.owner;
    sample.rigid_decompose = !capture.projected_shadow && SupportsRigidInterpolation(rdram, matrix);
    // A surface projection is a valid interpolated matrix, even though it
    // cannot be decomposed as a rigid object. Keep its semantic identity.
    sample.collectible_billboard = capture.collectible_billboard;
    sample.submodel_mode = capture.submodel_mode;
    // === ROCKET-R INTERPOLATION V39 COLLECTIBLE OWNER-POSITION CONTINUITY ===
    // Collectible matrices can be camera-relative, so their rendered matrix
    // translation is not a safe lifetime/continuity metric during camera pans.
    // Track the real GameObject world position instead. The matrix itself is
    // still the transform RT64 interpolates; only the identity continuity
    // guard changes.
    if (capture.instance_position_valid) {
        sample.position = capture.instance_position;
        sample.position_valid = true;
    } else if (sample.rigid_decompose && capture.owner != 0U) {
        const std::uint32_t owner_address = 0x80000000U | capture.owner;
        sample.position_valid = ReadGuestVec3(
            rdram, owner_address + kGameObjectPositionOffset, sample.position);
        if (sample.position_valid) {
            g_trace_v39_owner_position.fetch_add(1U, std::memory_order_relaxed);
        } else {
            sample.position_valid = ReadMtxTranslation(rdram, matrix, sample.position);
            g_trace_v39_matrix_fallback.fetch_add(1U, std::memory_order_relaxed);
        }
    } else {
        sample.position_valid = ReadMtxTranslation(rdram, matrix, sample.position);
    }

    std::scoped_lock lock(g_mutex);
    g_specific_samples.push_back(sample);
    g_trace_specific_samples.fetch_add(1U, std::memory_order_relaxed);
    if (sample.rigid_decompose) {
        g_trace_v37_collectible_rigid.fetch_add(1U, std::memory_order_relaxed);
    }
}

extern "C" void rocket_presentation_tinker_token_draw_begin(
    std::uint8_t* rdram, recomp_context* context) {
    g_tinker_token_draw_owner = 0U;
    g_tinker_token_draw_position = {};
    g_tinker_token_draw_position_valid = false;
    if (rdram == nullptr || context == nullptr) return;

    // At 0x8006BE94 func_8006BDF0 still holds the original token instance
    // in s0/r16. Its world position is the vec3 at +0x30.
    const std::uint32_t token = static_cast<std::uint32_t>(context->r16);
    if (!ValidRange(token, 0x3CU)) return;

    g_tinker_token_draw_owner = Physical(token);
    g_tinker_token_draw_position_valid =
        ReadGuestVec3(rdram, token + 0x30U, g_tinker_token_draw_position);
    g_trace_v42_token_draws.fetch_add(1U, std::memory_order_relaxed);
}

extern "C" void rocket_presentation_tinker_token_draw_end(
    std::uint8_t*, recomp_context*) {
    g_tinker_token_draw_owner = 0U;
    g_tinker_token_draw_position = {};
    g_tinker_token_draw_position_valid = false;
}

extern "C" void rocket_presentation_shared_mode0_begin(
    std::uint8_t* rdram, recomp_context* context) {
    g_shared_mode0_v43 = {};
    if (rdram == nullptr || context == nullptr) return;

    const std::uint32_t model =
        static_cast<std::uint32_t>(context->r4);
    if (!ValidRange(model, 0x78U)) return;

    std::uint32_t origin =
        static_cast<std::uint32_t>(context->r7);
    if (origin == 0U) {
        // Retail func_8001ECEC falls back to model + 0x6C when a3 is null.
        origin = model + 0x6CU;
    }
    if (!ValidRange(origin, 12U)) return;

    g_shared_mode0_v43.model = Physical(model);
    g_shared_mode0_v43.origin = Physical(origin);
    g_shared_mode0_v43.caller =
        EntryCallsite(context, 0x8001ECECU);
    if (g_shared_mode0_v43.caller == 0U) return;
    // At this checked draw s3 names Rocket and +0x268 selects his rolling wheel.
    // The secondary matrix carries angle +0x7D8, advanced by speed/radius in
    // func_8005CF48. Its squash/rotation pair can form a non-orthogonal basis.
    const auto player = static_cast<std::uint32_t>(context->r19);
    g_shared_mode0_v43.player_wheel = g_shared_mode0_v43.caller == 0x800589ECU &&
        ValidRange(player, 0x26CU) && ReadU32(rdram, player + 0x268U) == model;
    g_shared_mode0_v43.stack =
        Physical(static_cast<std::uint32_t>(context->r29));

    // Prefer a persistent render-origin pointer as the instance owner.
    // Caller-stack temporary vectors are not persistent, so use the model
    // object for those. Tinker Tokens have a stronger explicit owner and
    // override this inside the matrix hook.
    const std::uint32_t origin_phys = Physical(origin);
    const std::uint32_t stack_phys = g_shared_mode0_v43.stack;
    const std::uint32_t distance = origin_phys > stack_phys
        ? origin_phys - stack_phys
        : stack_phys - origin_phys;
    const bool stack_temporary = distance < 0x00010000U;
    g_shared_mode0_v43.owner_hint =
        stack_temporary ? Physical(model) : origin_phys;

    // Shadow models are shared assets and their projected origins are often
    // caller-stack scratch. Capture the actual instance before the callee
    // replaces these saved registers (verified retail JAL provenance only).
    std::uint32_t shadow_origin = 0U;
    g_shared_mode0_v43.projected_shadow = true;
    switch (g_shared_mode0_v43.caller) {
        case 0x8007C298U: // cached shadow result passed in a3
            shadow_origin = origin;
            break;
        case 0x8007C3CCU:
        case 0x8007C504U: // s1 = owning GameObject + 0x6C
            shadow_origin = static_cast<std::uint32_t>(context->r17);
            break;
        case 0x8007C654U: // s2 = original world-position argument
            shadow_origin = static_cast<std::uint32_t>(context->r18);
            break;
        case 0x8007CA04U: // s3 = owning GameObject
            shadow_origin = static_cast<std::uint32_t>(context->r19) + 0x6CU;
            break;
        default: g_shared_mode0_v43.projected_shadow = false; break;
    }
    if (g_shared_mode0_v43.projected_shadow) {
        if (!ValidRange(shadow_origin, 12U)) return;
        g_shared_mode0_v43.owner_hint = Physical(shadow_origin);
    }

    // Both shared mode-0 and camera-relative Submodels belong to the DRAW
    // INSTANCE. Tokens share model/submodel assets, so their model pointer
    // plus an occurrence number cannot identify the token being rendered.
    const std::uint32_t owner = g_tinker_token_draw_owner != 0U
        ? g_tinker_token_draw_owner : g_shared_mode0_v43.owner_hint;
    auto key = Mix64(static_cast<std::uint64_t>(owner));
    key = Mix64(key ^ (static_cast<std::uint64_t>(Physical(model)) << 1U));
    key = Mix64(key ^ (static_cast<std::uint64_t>(
        g_shared_mode0_v43.projected_shadow || stack_temporary ? 0U : origin_phys) << 21U));
    g_shared_mode0_v43.instance_key = Mix64(key ^
        (static_cast<std::uint64_t>(g_shared_mode0_v43.caller) << 37U));

    g_shared_mode0_v43.position_valid =
        ReadGuestVec3(rdram, shadow_origin != 0U ? shadow_origin : origin, g_shared_mode0_v43.position);
    if (g_tinker_token_draw_position_valid) {
        g_shared_mode0_v43.position = g_tinker_token_draw_position;
        g_shared_mode0_v43.position_valid = true;
    }
    g_shared_mode0_v43.active = true;
    std::scoped_lock lock(g_mutex);
    g_shared_mode0_v43.specific_begin = g_specific_samples.size();
}

extern "C" void rocket_presentation_shared_mode0_end(
    std::uint8_t*, recomp_context*) {
    g_shared_mode0_v43 = {};
}


extern "C" void rocket_presentation_tinker_token_matrix(
    std::uint8_t* rdram, recomp_context* context) {
    if (rdram == nullptr || context == nullptr ||
        !g_shared_mode0_v43.active) {
        return;
    }

    const std::uint32_t object =
        static_cast<std::uint32_t>(context->r19);
    const std::uint32_t submodel =
        static_cast<std::uint32_t>(context->r16);
    const std::uint32_t matrix =
        static_cast<std::uint32_t>(context->r17);

    if (!ValidRange(object, kGameObjectSubmodelCountOffset + 4U) ||
        !ValidRange(submodel, kSubmodelBytes) ||
        !ValidRange(matrix, kMtxBytes)) {
        return;
    }

    // Ensure this is still the func_8001ECEC invocation whose arguments were
    // captured at entry.
    if (Physical(object) != g_shared_mode0_v43.model) return;

    const std::uint8_t mode = static_cast<std::uint8_t>(
        MEM_B(0, RdramAddress(submodel + 0x20U)));
    if (mode != 0U) {
        return; // v35 func_8001EA18 remains authoritative for nonzero modes.
    }

    const std::uint32_t physical_matrix = Physical(matrix);
    if (g_shared_mode0_v43.claimed_matrix != 0U) {
        // This is the critical v43 fix. Retail func_8001ECEC reuses s4 for
        // every later mode-0 Submodel. Never submit another semantic identity
        // for that same physical slot, otherwise FinalizeSpecificMatrixBindings
        // deliberately turns it into IgnoredBinding().
        return;
    }
    g_shared_mode0_v43.claimed_matrix = physical_matrix;

    const bool token_draw = g_tinker_token_draw_owner != 0U;
    const std::uint32_t owner = token_draw
        ? g_tinker_token_draw_owner
        : g_shared_mode0_v43.owner_hint;
    if (owner == 0U) return;

    // Identity belongs to the shared DIRECT DRAW MATRIX, not the current
    // Submodel. This removes the self-conflict that kept tokens rejected and
    // extends the same proven mode-0 family to other direct model draws such
    // as the remaining machine-part path.
    std::uint64_t base_key = Mix64(
        (token_draw ? 0x54494E4B45524D30ULL : 0x4D4F444530445241ULL) ^
        g_shared_mode0_v43.instance_key);

    std::scoped_lock lock(g_mutex);
    std::uint64_t key = base_key;
    if (key == 0U) key = 1U;

    SpecificMatrixSample sample{};
    sample.key = key;
    sample.address = matrix;
    sample.owner = owner;
    // func_800577E8 also builds flat/sheared shadow projections. Those keep
    // component interpolation; only an orthogonal basis uses rigid rotation.
    sample.rigid_decompose = !g_shared_mode0_v43.projected_shadow &&
        SupportsRigidInterpolation(rdram, matrix);
    if (g_shared_mode0_v43.player_wheel && SupportsWheelDecomposition(rdram, matrix)) {
        sample.rigid_decompose = true;
        sample.interpolate_shape = true;
    }
    sample.collectible_billboard = false;
    sample.submodel_mode = 0U;

    if (token_draw && g_tinker_token_draw_position_valid) {
        sample.position = g_tinker_token_draw_position;
        sample.position_valid = true;
    } else if (g_shared_mode0_v43.position_valid) {
        sample.position = g_shared_mode0_v43.position;
        sample.position_valid = true;
    } else {
        sample.position_valid =
            ReadMtxTranslation(rdram, matrix, sample.position);
    }

    g_specific_samples.push_back(sample);
    g_trace_specific_samples.fetch_add(1U, std::memory_order_relaxed);
    if (token_draw) {
        // Preserve v42's existing trace counter/verification behavior.
        g_trace_v42_token_matrices.fetch_add(1U, std::memory_order_relaxed);
    }
    g_trace_v45_mode0.fetch_add(1U, std::memory_order_relaxed);
}









extern "C" void rocket_presentation_render_entry(std::uint8_t* rdram,
                                                   recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return;
    RecordedEntry entry{};
    entry.gfx = static_cast<std::uint32_t>(context->r4);
    entry.mtx1 = static_cast<std::uint32_t>(context->r5);
    entry.mtx2 = static_cast<std::uint32_t>(context->r6);
    entry.callsite = EntryCallsite(context, 0x8008B24CU);
    if (entry.callsite == 0U) return;
    entry.alpha = static_cast<std::uint8_t>(MEM_W(0x14, context->r29) & 0xFF);
    const GfxSemanticRef gfx_ref = CanonicalGfxRef(rdram, entry.gfx);
    entry.key = EntryKey(
        entry.callsite, gfx_ref.key, entry.mtx2 != 0U, entry.alpha);

    Vec3 p1{};
    Vec3 p2{};
    const bool v1 = ReadMtxTranslation(rdram, entry.mtx1, p1);
    const bool v2 = ReadMtxTranslation(rdram, entry.mtx2, p2);
    entry.mtx1_position = p1;
    entry.mtx2_position = p2;
    entry.mtx1_position_valid = v1;
    entry.mtx2_position_valid = v2;
    if (v1 && v2) {
        entry.position = Add(p1, p2);
        entry.position_valid = Finite(entry.position);
    } else if (v1) {
        entry.position = p1;
        entry.position_valid = true;
    } else if (v2) {
        entry.position = p2;
        entry.position_valid = true;
    }
    entry.dynamic_gfx = gfx_ref.dynamic;

    std::scoped_lock lock(g_mutex);
    if (TraceEnabled() && (g_frame % 120U) == 0U && entry.callsite == 0x8001F084U &&
        g_shared_mode0_v43.active && g_shared_mode0_v43.player_wheel) {
        g_trace_wheel_matrix = Physical(entry.mtx2 ? entry.mtx2 : entry.mtx1);
        std::fprintf(stderr, "[rocket-player-wheel] model=%08x primary=%08x secondary=%08x samples=%zu\n",
            g_shared_mode0_v43.model, entry.mtx1, entry.mtx2,
            g_specific_samples.size() - g_shared_mode0_v43.specific_begin);
    }
    if (entry.callsite == 0x8001F084U && g_shared_mode0_v43.active &&
        ValidRange(entry.mtx2, kMtxBytes)) {
        // The render queue emits LOAD(mtx1), MUL(mtx2); RT64 associates the
        // resulting transform with mtx2's address. Carry the exact instance
        // binding to that address, after the guest has produced both matrices.
        const auto pair = (static_cast<std::uint64_t>(Physical(entry.mtx1)) << 32U) |
            Physical(entry.mtx2);
        auto& pairs = g_shared_mode0_v43.secondary_pairs;
        if (std::find(pairs.begin(), pairs.end(), pair) == pairs.end()) {
            std::optional<SpecificMatrixSample> primary;
            for (auto i = g_shared_mode0_v43.specific_begin; i < g_specific_samples.size(); ++i) {
                if (g_specific_samples[i].secondary_transform) continue;
                if (Physical(g_specific_samples[i].address) != Physical(entry.mtx1)) continue;
                if (primary.has_value()) { primary.reset(); break; }
                primary = g_specific_samples[i];
            }
            if (primary.has_value()) {
                SpecificMatrixSample sample = *primary;
                sample.key = Mix64(sample.key ^ 0x5345434F4E444152ULL);
                sample.address = entry.mtx2;
                sample.secondary_transform = true;
                sample.rigid_decompose = primary->rigid_decompose &&
                    (primary->interpolate_shape ? SupportsWheelPair(rdram, entry.mtx1, entry.mtx2) :
                        SupportsRigidPair(rdram, entry.mtx1, entry.mtx2));
                sample.interpolate_shape = primary->interpolate_shape && sample.rigid_decompose;
                g_specific_samples.push_back(sample);
                pairs.push_back(pair); // material repeats reuse the same transform
                g_trace_specific_samples.fetch_add(1U, std::memory_order_relaxed);
            }
        }
    }
    entry.ordinal = g_key_ordinals[entry.key]++;
    g_entries.push_back(entry);
    g_trace_entries.fetch_add(1U, std::memory_order_relaxed);
}

extern "C" void rocket_presentation_task_submitted(std::uint8_t* rdram,
                                                     recomp_context*) {
    if (rdram == nullptr) return;
    const std::uint32_t task = ReadCurrentGfxTask(rdram);
    if (task == 0U) return;
    const std::uint32_t display_list = ReadU32(rdram, task + kGfxTaskDlStartOffset);
    if (!ValidRange(display_list, 8U)) return;

    std::scoped_lock lock(g_mutex);
    SubmittedFrame frame{};
    frame.display_list = Physical(display_list);
    frame.task_address = Physical(task);
    const std::uint32_t context_buffer = ReadU32(
        rdram, task + kGfxTaskCtxDlStartOffset);
    frame.context_dl_start = ValidRange(context_buffer, 8U)
        ? Physical(context_buffer) : 0U;
    frame.context_size = ReadU32(rdram, task + kGfxTaskCtxSizeOffset);
    frame.background_ranges = g_background_ranges;
    frame.sequence = g_submission_sequence++;
    FinalizeTracksAndBindings(frame.matrices);
    FinalizeModelRangeBindings(frame.matrices);
    FinalizeDirectMatrixBindings(frame.matrices);
    FinalizeCameraMatrixBindings(frame.matrices);
    FinalizeSpecificMatrixBindings(frame.matrices);
    for(const auto& [address,handle]:g_mod_matrices) {
        const auto id=NormalizeIdentity(Mix64(0x53444B324143544FULL^handle));
        frame.matrices[address]={rocket::presentation::MatrixBinding{id,false,false,false,true,true}};
    }
    if (g_trace_wheel_matrix != 0) {
        const auto found = frame.matrices.find(g_trace_wheel_matrix);
        if (found != frame.matrices.end()) std::fprintf(stderr, "[rocket-wheel-binding] matrix=%08x id=%08x rigid=%d shape=%d\n",
            g_trace_wheel_matrix, found->second.binding.identity, int(found->second.binding.rigid_decompose),
            int(found->second.binding.interpolate_shape));
        for (const auto& sample : g_specific_samples) if (Physical(sample.address) == g_trace_wheel_matrix)
            std::fprintf(stderr, "[rocket-wheel-owner] key=%016llx mode=%u rigid=%d owner=%08x\n",
                static_cast<unsigned long long>(sample.key), sample.submodel_mode, int(sample.rigid_decompose), sample.owner);
    }
    frame.camera_token = g_recording_camera_token;

    if (g_entries.empty()) {
        ++g_empty_frames;
        if (g_empty_frames >= 2U) {
            g_specific_continuity.clear();
            g_direct_continuity.clear();
            g_object_lifetimes.clear();
            g_camera_continuity = {};
            g_recording_camera_token = 0U;
            g_tracks.clear();
            g_shared_matrix_tracks.clear();
        }
    } else {
        g_empty_frames = 0U;
    }

    if (g_submitted.size() >= kMaximumPendingTasks) {
        g_submitted.clear();
        // Never guess after queue ownership is lost. Current task is still
        // allowed to start a fresh FIFO; prior interpolation history is gone.
        g_specific_continuity.clear();
        g_direct_continuity.clear();
        g_object_lifetimes.clear();
        g_camera_continuity = {};
        g_recording_camera_token = 0U;
        g_tracks.clear();
        g_shared_matrix_tracks.clear();
        if (TraceEnabled()) {
            std::fprintf(stderr,
                "[rocket-presentation] sidecar queue overflow; history reset\n");
        }
    }
    g_submitted.push_back(std::move(frame));
    g_background_ranges.clear();
    g_background_capture_active = false;
    g_background_capture_begin = 0U;
    g_entries.clear();
    g_key_ordinals.clear();
    g_specific_samples.clear();
    g_submodel_matrix_capture = {};
    g_model_range_samples.clear();
    g_model_range_depth = 0U;
    g_model_range_overflow_depth = 0U;
    g_direct_samples.clear();
    for (DirectCapture& capture : g_direct_captures) capture = {};
    g_camera_samples.clear();
}
