#include "presentation_identity.hpp"

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
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// === ROCKET-R INTERPOLATION V35 SPECIFIC SUBMODEL MATRICES + RANGE-STABLE SKY ===
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
constexpr std::uint32_t kGameObjectSubmodelsOffset = 0x0F4U;
constexpr std::uint32_t kGameObjectSubmodelCountOffset = 0x0F8U;
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

struct SpecificMatrixSample {
    std::uint64_t key = 0U;
    std::uint32_t address = 0U;
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
    bool valid = false;
};

struct BindingRecord {
    rocket::presentation::MatrixBinding binding{};
};

using MatrixMap = std::unordered_map<std::uint32_t, BindingRecord>;

// ROCKET-R SKYBOX INTERPOLATION V33
struct BackgroundCommandRange {
    std::uint32_t begin = 0U;
    std::uint32_t end = 0U;
};

struct SubmittedFrame {
    std::uint32_t display_list = 0U;
    std::uint32_t task_address = 0U;
    std::uint32_t context_dl_start = 0U;
    std::uint32_t context_size = 0U;
    std::uint64_t sequence = 0U;
    std::vector<BackgroundCommandRange> background_ranges{};
    MatrixMap matrices{};
};

std::mutex g_mutex;
std::vector<RecordedEntry> g_entries;
std::vector<Track> g_tracks;
std::vector<SharedMatrixTrack> g_shared_matrix_tracks;
std::vector<SpecificMatrixSample> g_specific_samples;
std::vector<SpecificContinuity> g_specific_continuity;
std::vector<SubmittedFrame> g_submitted;
std::uint64_t g_frame = 0U;
std::uint64_t g_submission_sequence = 1U;
std::uint32_t g_next_token = 1U;
std::uint32_t g_empty_frames = 0U;
std::unordered_map<std::uint64_t, std::uint32_t> g_key_ordinals;
std::unordered_map<std::uint64_t, std::uint32_t> g_specific_key_ordinals;
std::vector<BackgroundCommandRange> g_background_ranges;
bool g_background_capture_active = false;
std::uint32_t g_background_capture_begin = 0U;
thread_local MatrixMap g_active_matrices;
thread_local SubmodelMatrixCapture g_submodel_matrix_capture{};
thread_local std::vector<BackgroundCommandRange> g_active_background_ranges;
thread_local std::uint32_t g_active_context_dl_start = 0U;
thread_local std::uint32_t g_active_context_size = 0U;
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
std::atomic<std::uint64_t> g_trace_unowned_matrix_snaps{0U};
std::atomic<std::uint64_t> g_trace_background_identities{0U};
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

void MatchSharedMatrixSamples(std::vector<SharedMatrixSample>& samples) {
    for (SharedMatrixTrack& track : g_shared_matrix_tracks) track.claimed = false;
    if (samples.empty()) return;

    std::vector<SharedCandidate> candidates;
    for (std::size_t si = 0; si < samples.size(); ++si) {
        const SharedMatrixSample& sample = samples[si];
        for (std::size_t ti = 0; ti < g_shared_matrix_tracks.size(); ++ti) {
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

    for (const SharedMatrixSample& sample : samples) {
        const auto found = std::find_if(
            g_shared_matrix_tracks.begin(), g_shared_matrix_tracks.end(),
            [&](const SharedMatrixTrack& track) {
                return track.token == sample.track_token;
            });
        if (found == g_shared_matrix_tracks.end()) continue;
        found->position = sample.position;
        found->position_valid = sample.position_valid;
        found->last_frame = g_frame;
    }

    g_trace_shared_samples.fetch_add(samples.size(), std::memory_order_relaxed);
}

// ROCKET-R INTERPOLATION V35: high-confidence Submodel matrices are matched
// separately from the stable v5/v6 RenderEntry heuristic. This cannot make an
// existing generic match less conservative: it only overrides a physical slot
// when Rocket's own func_8001EA18 proved the exact GameObject/Submodel owner.
void FinalizeSpecificMatrixBindings(MatrixMap& out) {
    std::erase_if(g_specific_continuity, [](const SpecificContinuity& item) {
        return g_frame > item.last_frame + kMaximumTrackAge;
    });
    for (SpecificContinuity& item : g_specific_continuity) item.claimed = false;

    std::unordered_map<std::uint32_t, std::uint32_t> specific_claims;
    specific_claims.reserve(g_specific_samples.size());

    for (SpecificMatrixSample& sample : g_specific_samples) {
        SpecificContinuity* match = nullptr;
        bool ambiguous = false;
        for (SpecificContinuity& item : g_specific_continuity) {
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
        }

        if (sample.track_token == 0U || sample.address == 0U ||
            !ValidRange(sample.address, kMtxBytes)) continue;
        const std::uint32_t physical = Physical(sample.address);
        rocket::presentation::MatrixBinding binding{};
        binding.identity = MatrixIdentity(sample.track_token, 0x40U);
        binding.interpolate_vertices = false;
        binding.interpolate_texcoords = false;
        binding.interpolate_tiles = false;

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
        }
    }
}

void FinalizeTracksAndBindings(MatrixMap& out) {
    ExpireTracks();
    for (Track& track : g_tracks) track.claimed = false;

    std::vector<Candidate> candidates;
    candidates.reserve(g_entries.size() * 4U);
    for (std::size_t ei = 0; ei < g_entries.size(); ++ei) {
        const RecordedEntry& entry = g_entries[ei];
        for (std::size_t ti = 0; ti < g_tracks.size(); ++ti) {
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
    for (const RecordedEntry& entry : g_entries) {
        const auto found = std::find_if(g_tracks.begin(), g_tracks.end(),
            [&](const Track& track) { return track.token == entry.track_token; });
        if (found == g_tracks.end()) continue;
        Track& track = *found;
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
        g_tracks.clear();
        g_shared_matrix_tracks.clear();
        return;
    }

    g_active_matrices = std::move(frame.matrices);
    g_active_background_ranges = std::move(frame.background_ranges);
    g_active_context_dl_start = frame.context_dl_start;
    g_active_context_size = frame.context_size;
    g_submitted.erase(g_submitted.begin());
    g_trace_task_matches.fetch_add(1U, std::memory_order_relaxed);
}

rocket::presentation::TaskIdentityScope::~TaskIdentityScope() {
    g_active_matrices.clear();
    g_active_background_ranges.clear();
    g_active_context_dl_start = 0U;
    g_active_context_size = 0U;
    g_active_task_fail_closed = false;
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
    out_binding->reserved = 0U;
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
    g_specific_samples.clear();
    g_specific_key_ordinals.clear();
    g_submodel_matrix_capture = {};
    g_background_ranges.clear();
    g_background_capture_active = false;
    g_background_capture_begin = 0U;
    ++g_frame;
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
    std::uint64_t base_key = Mix64(0x5355424D41545258ULL ^
        static_cast<std::uint64_t>(Physical(object)));
    base_key = Mix64(base_key ^ (static_cast<std::uint64_t>(index) << 32U));
    base_key = Mix64(base_key ^
        (static_cast<std::uint64_t>(object_class) << 1U));
    base_key = Mix64(base_key ^
        (static_cast<std::uint64_t>(submodel_gfx) << 17U));

    std::scoped_lock lock(g_mutex);
    const std::uint32_t occurrence = g_specific_key_ordinals[base_key]++;
    std::uint64_t key = Mix64(base_key ^
        (static_cast<std::uint64_t>(occurrence) << 48U));
    if (key == 0U) key = 1U;
    g_submodel_matrix_capture = {key, true};
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
    sample.position_valid = ReadMtxTranslation(rdram, matrix, sample.position);

    std::scoped_lock lock(g_mutex);
    g_specific_samples.push_back(sample);
    g_trace_specific_samples.fetch_add(1U, std::memory_order_relaxed);
}

extern "C" void rocket_presentation_render_entry(std::uint8_t* rdram,
                                                   recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return;
    RecordedEntry entry{};
    entry.gfx = static_cast<std::uint32_t>(context->r4);
    entry.mtx1 = static_cast<std::uint32_t>(context->r5);
    entry.mtx2 = static_cast<std::uint32_t>(context->r6);
    entry.callsite = static_cast<std::uint32_t>(context->r31);
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
    FinalizeSpecificMatrixBindings(frame.matrices);

    if (g_entries.empty()) {
        ++g_empty_frames;
        if (g_empty_frames >= 2U) {
            g_specific_continuity.clear();
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
    g_specific_key_ordinals.clear();
    g_submodel_matrix_capture = {};
}
