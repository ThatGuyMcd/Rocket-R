#include "presentation_identity.hpp"

#include "recomp.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstdlib>
#include <limits>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t kRdramMask = 0x007FFFFFU;
constexpr std::uint32_t kRdramStart = 0x80000000U;
constexpr std::uint32_t kRdramEnd = 0x807FFFFFU;
constexpr std::uint32_t kCurGfxTaskAddress = 0x800A5DBCU;
constexpr std::uint32_t kGfxTaskCtxSizeOffset = 0x004U;
constexpr std::uint32_t kGfxTaskCtxDlStartOffset = 0x008U;
constexpr std::uint32_t kGfxTaskDlStartOffset = 0x014U;
// === ROCKET-R GRAPHICS V27 EXPANDED GFX ARENA BEGIN ===
// Rocket hard-codes RAM_END=0x80400000 (4 MiB). N64ModernRuntime exposes 8 MiB,
// so these two arenas live entirely in otherwise-unused Expansion Pak RAM.
constexpr std::uint32_t kRocketV27Task0Address = 0x800C1460U;
constexpr std::uint32_t kRocketV27Task1Address = 0x800C15E8U;
constexpr std::uint32_t kRocketV27Arena0Address = 0x80600000U;
constexpr std::uint32_t kRocketV27Arena1Address = 0x80680000U;
constexpr std::uint32_t kRocketV27ArenaBytes = 0x00080000U;
constexpr std::uint32_t kGfxContextAddress = 0x800A5DA8U;
constexpr std::uint32_t kGfxTaskCtxDlHeadOffset = 0x00CU;
constexpr std::uint32_t kGfxTaskCtxMtxHeadOffset = 0x010U;
// === ROCKET-R GRAPHICS V27 EXPANDED GFX ARENA END ===
constexpr std::uint32_t kMtxBytes = 0x40U;
constexpr std::uint64_t kMaximumTrackAge = 1U;
constexpr std::size_t kMaximumPendingTasks = 8U;
constexpr std::size_t kGuestRenderQueueCapacity = 256U;
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
    std::uint64_t owner_key = 0U;
    bool owner_valid = false;
    std::uint32_t track_token = 0U;
};

struct OwnerTrack {
    std::uint64_t key = 0U;
    std::uint32_t token = 0U;
    std::uint64_t last_frame = 0U;
    bool claimed = false;
};

struct PendingModelOwner {
    std::uint64_t key = 0U;
    std::uint32_t gfx_physical = 0U;
    bool valid = false;
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

struct BindingRecord {
    rocket::presentation::MatrixBinding binding{};
};

using MatrixMap = std::unordered_map<std::uint32_t, BindingRecord>;

struct SubmittedFrame {
    std::uint32_t display_list = 0U;
    std::uint32_t task_address = 0U;
    std::uint32_t context_dl_start = 0U;
    std::uint32_t context_size = 0U;
    std::uint64_t sequence = 0U;
    MatrixMap matrices{};
};

std::mutex g_mutex;
std::vector<RecordedEntry> g_entries;
std::vector<OwnerTrack> g_owner_tracks;
std::vector<Track> g_tracks;
std::vector<SharedMatrixTrack> g_shared_matrix_tracks;
std::vector<SubmittedFrame> g_submitted;
std::uint64_t g_frame = 0U;
std::uint64_t g_submission_sequence = 1U;
std::uint32_t g_next_token = 1U;
std::uint32_t g_empty_frames = 0U;
std::unordered_map<std::uint64_t, std::uint32_t> g_key_ordinals;
thread_local MatrixMap g_active_matrices;
thread_local PendingModelOwner g_pending_model_owner{};
// While an RT64 Rocket task is being decoded, the semantic sidecar owns the
// matching policy for every model matrix. Unknown matrices must therefore snap
// instead of escaping back into RT64's anonymous automatic matcher.
thread_local bool g_active_task_fail_closed = false;

std::atomic<bool> g_logged_render_queue_pressure{false};
std::atomic<bool> g_logged_render_queue_saturation{false};
std::atomic<std::uint64_t> g_trace_entries{0U};
std::atomic<std::uint64_t> g_trace_owned_entries{0U};
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
    if (!ValidRange(buffer, 8U) || bytes == 0U || bytes > kRocketV27ArenaBytes) return result;
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

[[nodiscard]] std::uint32_t NextPresentationToken() {
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
    std::erase_if(g_owner_tracks, [](const OwnerTrack& track) {
        return g_frame > track.last_frame + kMaximumTrackAge;
    });
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
        AddSharedOccurrence(accumulators, entry.mtx1, 1U,
                            entry.owner_valid ? entry.owner_key : entry.key,
                            entry.mtx1_position, entry.mtx1_position_valid,
                            entry.dynamic_gfx);
        AddSharedOccurrence(accumulators, entry.mtx2, 2U,
                            entry.owner_valid ? entry.owner_key : entry.key,
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
            const std::uint32_t token = NextPresentationToken();
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

void FinalizeTracksAndBindings(MatrixMap& out) {
    ExpireTracks();
    for (OwnerTrack& track : g_owner_tracks) track.claimed = false;
    for (Track& track : g_tracks) track.claimed = false;

    // v26 durable owner path: model/submodel entries captured directly from
    // func_8001ECEC never enter the positional/ordinal matcher. If the exact
    // GameObject/submodel was present in the immediately previous authored
    // frame it retains its token; otherwise it starts a fresh presentation
    // lifetime and snaps to the current authored endpoint.
    std::vector<bool> entry_claimed(g_entries.size(), false);
    for (std::size_t ei = 0; ei < g_entries.size(); ++ei) {
        RecordedEntry& entry = g_entries[ei];
        if (!entry.owner_valid || entry.owner_key == 0U) continue;
        auto found = std::find_if(g_owner_tracks.begin(), g_owner_tracks.end(),
            [&](const OwnerTrack& track) {
                return track.key == entry.owner_key && !track.claimed &&
                       track.last_frame + 1U == g_frame;
            });
        if (found == g_owner_tracks.end()) {
            OwnerTrack track{};
            track.key = entry.owner_key;
            track.token = NextPresentationToken();
            track.last_frame = g_frame;
            track.claimed = true;
            entry.track_token = track.token;
            g_owner_tracks.push_back(track);
        } else {
            entry.track_token = found->token;
            found->last_frame = g_frame;
            found->claimed = true;
        }
        entry_claimed[ei] = true;
        g_trace_owned_entries.fetch_add(1U, std::memory_order_relaxed);
    }

    std::vector<Candidate> candidates;
    candidates.reserve(g_entries.size() * 4U);
    for (std::size_t ei = 0; ei < g_entries.size(); ++ei) {
        const RecordedEntry& entry = g_entries[ei];
        if (entry.owner_valid) continue;
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
            const std::uint32_t token = NextPresentationToken();
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
        "[rocket-presentation] frame=%llu entries=%llu owned=%llu matched=%llu new=%llu conflicts=%llu ambiguous=%llu shared=%llu shared-match=%llu shared-new=%llu shared-reject=%llu task-match=%llu task-miss=%llu sidecar-mismatch=%llu tracks=%zu shared-tracks=%zu pending=%zu\n",
        static_cast<unsigned long long>(g_frame),
        static_cast<unsigned long long>(g_trace_entries.exchange(0U)),
        static_cast<unsigned long long>(g_trace_owned_entries.exchange(0U)),
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
            context_size = ReadU32(rdram_snapshot, task + kGfxTaskCtxSizeOffset);
        }
    }

    std::scoped_lock lock(g_mutex);
    if (g_submitted.empty()) {
        g_trace_task_misses.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    // DKR-style durable task ownership: the immutable OSTask display-list root
    // is authoritative. Mutable task/context parity can already have flipped by
    // decode time, so it is diagnostic only and must never invalidate a valid
    // sidecar or clear good object identity history.
    const auto matching = std::find_if(
        g_submitted.begin(), g_submitted.end(),
        [physical](const SubmittedFrame& candidate) {
            return candidate.display_list == physical;
        });
    if (matching == g_submitted.end()) {
        g_trace_task_misses.fetch_add(1U, std::memory_order_relaxed);
        g_trace_sidecar_mismatches.fetch_add(1U, std::memory_order_relaxed);
        if (TraceEnabled()) {
            const SubmittedFrame& expected = g_submitted.front();
            std::fprintf(stderr,
                "[rocket-presentation] sidecar mismatch: expected dl=%06X task=%06X ctx=%06X/%u got dl=%06X task=%06X ctx=%06X/%u; interpolation disabled for this task\n",
                expected.display_list, expected.task_address,
                expected.context_dl_start, expected.context_size, physical,
                task_address, context_dl_start, context_size);
        }
        return;
    }

    SubmittedFrame frame = std::move(*matching);
    g_submitted.erase(g_submitted.begin(), std::next(matching));
    g_active_matrices = std::move(frame.matrices);
    g_trace_task_matches.fetch_add(1U, std::memory_order_relaxed);
}

rocket::presentation::TaskIdentityScope::~TaskIdentityScope() {
    g_active_matrices.clear();
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


// === ROCKET-R GRAPHICS V27 EXPANDED GFX ARENA RUNTIME BEGIN ===
extern "C" void rocket_graphics_arena_v27(std::uint8_t* rdram, recomp_context*) {
    if (rdram == nullptr) return;
    const std::uint32_t task = ReadU32(rdram, kCurGfxTaskAddress);
    std::uint32_t arena = 0U;
    if (task == kRocketV27Task0Address) arena = kRocketV27Arena0Address;
    else if (task == kRocketV27Task1Address) arena = kRocketV27Arena1Address;
    else return;

    const std::uint32_t arena_end = arena + kRocketV27ArenaBytes;
    if (!ValidRange(arena, kRocketV27ArenaBytes) ||
        !ValidRange(task, kGfxTaskCtxMtxHeadOffset + 4U) ||
        !ValidRange(kGfxContextAddress, 0x10U)) return;

    const auto write_u32 = [&](std::uint32_t address, std::uint32_t value) {
        MEM_W(0, RdramAddress(address)) = value;
    };

    // Persist the expanded template for this alternating task.
    write_u32(task + kGfxTaskCtxSizeOffset, kRocketV27ArenaBytes);
    write_u32(task + kGfxTaskCtxDlStartOffset, arena);
    write_u32(task + kGfxTaskCtxDlHeadOffset, arena);
    write_u32(task + kGfxTaskCtxMtxHeadOffset, arena_end);

    // update_gfx_context already copied the retail context this frame, so redirect
    // the active global context before func_80046D58 emits any frame commands.
    write_u32(kGfxContextAddress + 0x00U, kRocketV27ArenaBytes);
    write_u32(kGfxContextAddress + 0x04U, arena);
    write_u32(kGfxContextAddress + 0x08U, arena);
    write_u32(kGfxContextAddress + 0x0CU, arena_end);

    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr, "[graphics] v27 GfxTask arena expansion active: 512 KiB x2.\n");
    }
}
// === ROCKET-R GRAPHICS V27 EXPANDED GFX ARENA RUNTIME END ===

// v20: direct pop-in telemetry. This does not reorder, replay, expand or replace
// Rocket's renderer. It only observes add_render_entry before the retail 256
// capacity gate and writes a dedicated diagnostic file.
std::atomic<std::uint64_t> g_popdiag_frustum_calls{0U};
std::atomic<std::uint64_t> g_popdiag_visible_attempts{0U};
std::atomic<std::uint64_t> g_popdiag_rejected_at_capacity{0U};
std::atomic<std::uint32_t> g_popdiag_max_guest_entries{0U};
std::atomic<std::uint64_t> g_popdiag_frame_serial{0U};

static void RocketPopDiagAppendToPath(const char* path, const char* line) {
    if (path == nullptr || line == nullptr) return;
    std::FILE* file = std::fopen(path, "ab");
    if (file == nullptr) return;
    std::fputs(line, file);
    std::fflush(file);
    std::fclose(file);
}

static void RocketPopDiagWrite(const char* format, ...) {
    char line[1024]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    // Always write a copy in the process working directory.
    RocketPopDiagAppendToPath("Rocket-R-popin-diagnostics.log", line);

    // Also write a predictable TEMP copy so the log is easy to find on Windows.
    const char* temp = std::getenv("TEMP");
    if (temp == nullptr || *temp == '\0') {
        temp = std::getenv("TMPDIR");
    }
    if (temp != nullptr && *temp != '\0') {
        char path[1024]{};
#ifdef _WIN32
        std::snprintf(path, sizeof(path), "%s\\Rocket-R-popin-diagnostics.log", temp);
#else
        std::snprintf(path, sizeof(path), "%s/Rocket-R-popin-diagnostics.log", temp);
#endif
        RocketPopDiagAppendToPath(path, line);
    }
}

static void RocketPopDiagAtomicMax(std::atomic<std::uint32_t>& target,
                                   std::uint32_t value) {
    std::uint32_t current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(current, value,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
    }
}

extern "C" void rocket_popdiag_frustum_call(void) {
    g_popdiag_frustum_calls.fetch_add(1U, std::memory_order_relaxed);
}

extern "C" void rocket_popdiag_add_render_entry_attempt(std::uint8_t* rdram,
                                                            recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return;
    const std::uint8_t alpha = static_cast<std::uint8_t>(
        MEM_W(0x14, context->r29) & 0xFF);
    if (alpha == 0U) return;

    g_popdiag_visible_attempts.fetch_add(1U, std::memory_order_relaxed);

    constexpr std::uint32_t kQueueBase = 0x800ADB00U;
    constexpr std::uint32_t kQueueEndPointerAddress = 0x800AF300U;
    constexpr std::uint32_t kEntryBytes = 24U;
    constexpr std::uint32_t kCapacity = 256U;
    const gpr end_pointer_address = static_cast<gpr>(
        static_cast<std::int32_t>(kQueueEndPointerAddress));
    const std::uint32_t end_pointer = static_cast<std::uint32_t>(
        MEM_W(0, end_pointer_address));

    if (end_pointer >= kQueueBase) {
        const std::uint32_t delta = end_pointer - kQueueBase;
        if ((delta % kEntryBytes) == 0U) {
            const std::uint32_t entries = delta / kEntryBytes;
            RocketPopDiagAtomicMax(g_popdiag_max_guest_entries,
                                   std::min(entries, kCapacity));
            if (entries >= kCapacity) {
                g_popdiag_rejected_at_capacity.fetch_add(
                    1U, std::memory_order_relaxed);
            }
        }
    }
}

static void RocketPopDiagFlushPreviousFrame(void) {
    const std::uint64_t frame =
        g_popdiag_frame_serial.fetch_add(1U, std::memory_order_relaxed) + 1U;
    const std::uint64_t frustum =
        g_popdiag_frustum_calls.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t attempts =
        g_popdiag_visible_attempts.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t rejected =
        g_popdiag_rejected_at_capacity.exchange(0U, std::memory_order_relaxed);
    const std::uint32_t max_entries =
        g_popdiag_max_guest_entries.exchange(0U, std::memory_order_relaxed);

    if (frame == 1U) {
        RocketPopDiagWrite(
            "=== Rocket-R v27 EXPANDED-GFX-ARENA + DURABLE-OWNERSHIP diagnostic session ===\n"
            "mode=normal-v17.1-renderer side-plane-cpu-cull=V17.1-VIEWPORT-GUARD authored-submodel-mask=RETAIL pre-render-object-gate=RETAIL presentation-identity=DURABLE-OWNER-V26 gfx-arena=512Kx2-V27 "
            "draw-distance=r7-preserved retail-distance-fade=preserved queue-overflow=RECOVERED-V21\n"
            "fields: frame frustum_calls visible_add_attempts max_guest_queue "
            "would_drop_at_256_recovered\n");
    }

    // One compact line per authored frame. This remains small enough for a
    // short reproduction and makes camera-angle transitions easy to correlate.
    RocketPopDiagWrite(
        "frame=%llu frustum_calls=%llu visible_add_attempts=%llu "
        "max_guest_queue=%u would_drop_at_256_recovered=%llu\n",
        static_cast<unsigned long long>(frame),
        static_cast<unsigned long long>(frustum),
        static_cast<unsigned long long>(attempts),
        max_entries,
        static_cast<unsigned long long>(rejected));
}

extern "C" void rocket_presentation_frame_begin(std::uint8_t*,
                                                  recomp_context*) {
    g_pending_model_owner = {};
    std::scoped_lock lock(g_mutex);
    RocketPopDiagFlushPreviousFrame();
    const std::size_t visible_attempts = static_cast<std::size_t>(
        std::count_if(g_entries.begin(), g_entries.end(),
                      [](const RecordedEntry& entry) { return entry.alpha != 0U; }));
    if (visible_attempts >= 240U &&
        !g_logged_render_queue_pressure.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
            "[render-queue] PRESSURE: previous authored frame attempted %zu/%zu visible entries. "
            "Dense widescreen views are close to Rocket's original render-list ceiling.\n",
            visible_attempts, kGuestRenderQueueCapacity);
    }
    if (visible_attempts > kGuestRenderQueueCapacity &&
        !g_logged_render_queue_saturation.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
            "[render-queue] SATURATION: previous authored frame attempted %zu visible entries, "
            "and Rocket's retail staging list holds %zu. v21 recovers %zu overflow entries "
            "inside the same renderer invocation.\n",
            visible_attempts, kGuestRenderQueueCapacity,
            visible_attempts - kGuestRenderQueueCapacity);
    }
    ++g_frame;
    g_entries.clear();
    g_key_ordinals.clear();
    MaybeTraceSummary();
}

extern "C" void rocket_presentation_model_entry_owner(std::uint8_t* rdram,
                                                       recomp_context* context) {
    g_pending_model_owner = {};
    if (rdram == nullptr || context == nullptr) return;
    const std::uint32_t object = static_cast<std::uint32_t>(context->r19);
    const std::uint32_t submodel = static_cast<std::uint32_t>(context->r16);
    if (!ValidRange(object, 0xFCU) || !ValidRange(submodel, 0x28U)) return;
    const std::uint32_t submodels = ReadU32(rdram, object + 0xF4U);
    const std::uint32_t count = ReadU32(rdram, object + 0xF8U);
    if (count == 0U || count > 512U || !ValidRange(submodels, count * 0x28U) ||
        submodel < submodels) return;
    const std::uint32_t delta = submodel - submodels;
    if ((delta % 0x28U) != 0U) return;
    const std::uint32_t index = delta / 0x28U;
    if (index >= count) return;

    const std::uint32_t gfx = static_cast<std::uint32_t>(context->r4);
    const GfxSemanticRef gfx_ref = CanonicalGfxRef(rdram, gfx);
    const std::uint32_t object_class = ReadU32(rdram, object);
    std::uint64_t key = Mix64(static_cast<std::uint64_t>(Physical(object)) |
                              (static_cast<std::uint64_t>(index) << 32U));
    key = Mix64(key ^ (static_cast<std::uint64_t>(Physical(object_class)) << 1U));
    key = Mix64(key ^ (static_cast<std::uint64_t>(gfx_ref.key) << 17U));
    if (key == 0U) key = 1U;
    g_pending_model_owner = {key, Physical(gfx), true};
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
    const PendingModelOwner pending_owner = std::exchange(
        g_pending_model_owner, PendingModelOwner{});
    if (pending_owner.valid &&
        pending_owner.gfx_physical == Physical(entry.gfx)) {
        entry.owner_key = pending_owner.key;
        entry.owner_valid = true;
    }
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
    frame.sequence = g_submission_sequence++;
    FinalizeTracksAndBindings(frame.matrices);

    if (g_entries.empty()) {
        ++g_empty_frames;
        if (g_empty_frames >= 2U) {
            g_owner_tracks.clear();
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
        g_owner_tracks.clear();
        g_tracks.clear();
        g_shared_matrix_tracks.clear();
        if (TraceEnabled()) {
            std::fprintf(stderr,
                "[rocket-presentation] sidecar queue overflow; history reset\n");
        }
    }
    g_submitted.push_back(std::move(frame));
    g_entries.clear();
    g_key_ordinals.clear();
}

// === ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION BEGIN ===
// Recover submissions beyond Rocket's retail 256-entry queue without replacing
// func_8008B694. Scene traversal occurs once; the original renderer remains in
// control and draws globally preordered staging batches before its normal tail.
namespace {

constexpr std::uint32_t kRocketV21QueueBase = 0x800ADB00U;
constexpr std::uint32_t kRocketV21QueueEndPointer = 0x800AF300U;
constexpr std::uint32_t kRocketV21EntryBytes = 24U;
constexpr std::size_t kRocketV21RetailBatchCapacity = 256U;
// gGfxContext = 0x800A5DA8 in the pinned Rocket US build. dlHead and the
// descending matrix cursor are +0x08/+0x0C respectively. This is telemetry
// only: v21 never changes either pointer or truncates a recovered batch.
constexpr std::uint32_t kRocketV21DlHeadAddress = 0x800A5DB0U;
constexpr std::uint32_t kRocketV21MatrixHeadAddress = 0x800A5DB4U;

struct RocketV21QueueEntry {
    std::uint32_t gfx = 0U;
    std::uint32_t mtx1 = 0U;
    std::uint32_t mtx2 = 0U;
    std::uint32_t depth_bits = 0U;
    std::uint32_t render_params = 0U;
    std::uint8_t alpha = 0U;
};

struct RocketV21QueueState {
    std::vector<RocketV21QueueEntry> captured{};
    std::vector<std::size_t> final_order{};
    std::size_t cursor = 0U;
    std::size_t batches_staged = 0U;
    std::uint32_t min_dl_headroom_bytes = std::numeric_limits<std::uint32_t>::max();
    bool capture_active = false;
    bool expanded = false;
    bool continuation = false;
};

thread_local RocketV21QueueState g_rocket_v21_queue{};
std::atomic<bool> g_rocket_v21_logged_activation{false};
std::atomic<bool> g_rocket_v21_logged_invalid_class{false};

[[nodiscard]] gpr RocketV21GuestAddress(std::uint32_t address) {
    return static_cast<gpr>(
        static_cast<std::int64_t>(static_cast<std::int32_t>(address)));
}

[[nodiscard]] std::uint32_t RocketV21DisplayListHeadroomBytes(std::uint8_t* rdram) {
    const std::uint32_t dl_head = static_cast<std::uint32_t>(
        MEM_W(0, RocketV21GuestAddress(kRocketV21DlHeadAddress)));
    const std::uint32_t matrix_head = static_cast<std::uint32_t>(
        MEM_W(0, RocketV21GuestAddress(kRocketV21MatrixHeadAddress)));
    if (matrix_head < dl_head) return 0U;
    return matrix_head - dl_head;
}

void RocketV21ObserveDisplayListHeadroom(std::uint8_t* rdram) {
    auto& state = g_rocket_v21_queue;
    state.min_dl_headroom_bytes = std::min(
        state.min_dl_headroom_bytes, RocketV21DisplayListHeadroomBytes(rdram));
}

[[nodiscard]] std::uint8_t RocketV21RenderClass(const RocketV21QueueEntry& entry) {
    return static_cast<std::uint8_t>((entry.render_params >> 28U) & 0x0FU);
}

[[nodiscard]] float RocketV21Depth(const RocketV21QueueEntry& entry) {
    return std::bit_cast<float>(entry.depth_bits);
}

[[nodiscard]] std::uint32_t RocketV21FinalRenderParams(std::uint32_t raw,
                                                        std::uint8_t alpha) {
    if (alpha >= 0xFFU) return raw;

    // add_render_entry rewrites every faded entry to
    // unk_make_RenderParams(2, 2, unk1, unk2==3 ? 5 : 4).
    // RenderParams is four guest bytes: [unk0/cycle][unk1][unk2][renderMode].
    const std::uint8_t unk1 = static_cast<std::uint8_t>((raw >> 16U) & 0xFFU);
    std::uint8_t unk2 = static_cast<std::uint8_t>((raw >> 8U) & 0xFFU);
    unk2 = (unk2 == 3U) ? 5U : 4U;
    const std::uint8_t render_mode = (unk2 == 5U) ? 7U : 6U;
    return (0x22U << 24U) |
           (static_cast<std::uint32_t>(unk1) << 16U) |
           (static_cast<std::uint32_t>(unk2) << 8U) |
           static_cast<std::uint32_t>(render_mode);
}

void RocketV21HeapSortSegment(std::vector<std::size_t>& order,
                              std::size_t begin,
                              std::size_t length) {
    if (length < 2U) return;

    std::ptrdiff_t end = static_cast<std::ptrdiff_t>(length);
    std::ptrdiff_t root = end / 2;
    --end;

    while (true) {
        if (root > 0) {
            --root;
        } else {
            std::swap(order[begin], order[begin + static_cast<std::size_t>(end)]);
            --end;
            if (!(end > 0)) break;
        }

        std::ptrdiff_t parent = root;
        std::ptrdiff_t child = (parent * 2) + 1;
        while (end >= child) {
            if (child < end) {
                const float left = RocketV21Depth(
                    g_rocket_v21_queue.captured[order[begin + static_cast<std::size_t>(child)]]);
                const float right = RocketV21Depth(
                    g_rocket_v21_queue.captured[order[begin + static_cast<std::size_t>(child + 1)]]);
                if (left < right) ++child;
            }

            const float parent_depth = RocketV21Depth(
                g_rocket_v21_queue.captured[order[begin + static_cast<std::size_t>(parent)]]);
            const float child_depth = RocketV21Depth(
                g_rocket_v21_queue.captured[order[begin + static_cast<std::size_t>(child)]]);
            if (!(parent_depth < child_depth)) break;

            std::swap(order[begin + static_cast<std::size_t>(parent)],
                      order[begin + static_cast<std::size_t>(child)]);
            parent = child;
            child += child + 1;
        }
    }
}

[[nodiscard]] bool RocketV21BuildGlobalOrder() {
    auto& state = g_rocket_v21_queue;
    const std::size_t count = state.captured.size();
    state.final_order.resize(count);
    for (std::size_t i = 0; i < count; ++i) state.final_order[i] = i;
    if (count == 0U) return true;

    for (const RocketV21QueueEntry& entry : state.captured) {
        const std::uint8_t cls = RocketV21RenderClass(entry);
        if (cls != 1U && cls != 2U) {
            if (!g_rocket_v21_logged_invalid_class.exchange(true, std::memory_order_relaxed)) {
                std::fprintf(stderr,
                    "[render-queue] v21 expansion declined: encountered RenderParams class %u; "
                    "retail 256-entry behaviour retained for safety.\n",
                    static_cast<unsigned>(cls));
            }
            return false;
        }
    }

    // Exact source-level reproduction of divide_opaque_and_transparent().
    std::size_t opaque_end = 0U;
    std::size_t transparent_start = count - 1U;
    while (true) {
        while (RocketV21RenderClass(state.captured[state.final_order[opaque_end]]) == 1U &&
               opaque_end < transparent_start) {
            ++opaque_end;
        }
        while (RocketV21RenderClass(state.captured[state.final_order[transparent_start]]) == 2U &&
               opaque_end < transparent_start) {
            --transparent_start;
        }
        if (opaque_end >= transparent_start) break;
        std::swap(state.final_order[opaque_end], state.final_order[transparent_start]);
        ++opaque_end;
        --transparent_start;
    }
    if (RocketV21RenderClass(state.captured[state.final_order[opaque_end]]) == 1U) {
        ++opaque_end;
    }

    // Exact source-level heap sort for each retail class.
    RocketV21HeapSortSegment(state.final_order, 0U, opaque_end);
    RocketV21HeapSortSegment(state.final_order, opaque_end, count - opaque_end);

    // func_8008B694 draws opaque forward and transparent backward. Convert that
    // to one explicit final draw order so arbitrary 256-entry staging boundaries
    // can never perturb global transparency order.
    std::vector<std::size_t> draw_order;
    draw_order.reserve(count);
    draw_order.insert(draw_order.end(), state.final_order.begin(),
                      state.final_order.begin() + static_cast<std::ptrdiff_t>(opaque_end));
    for (std::size_t i = count; i > opaque_end; --i) {
        draw_order.push_back(state.final_order[i - 1U]);
    }
    state.final_order.swap(draw_order);
    return true;
}

void RocketV21WriteEntry(std::uint8_t* rdram,
                         std::size_t slot,
                         const RocketV21QueueEntry& entry) {
    (void)rdram;
    const std::uint32_t guest = kRocketV21QueueBase +
        static_cast<std::uint32_t>(slot * kRocketV21EntryBytes);
    const gpr address = RocketV21GuestAddress(guest);
    MEM_W(0x00, address) = entry.gfx;
    MEM_W(0x04, address) = entry.mtx1;
    MEM_W(0x08, address) = entry.mtx2;
    MEM_W(0x0C, address) = entry.depth_bits;
    MEM_W(0x10, address) = entry.render_params;
    MEM_B(0x14, address) = entry.alpha;
}

[[nodiscard]] bool RocketV21StageNextBatch(std::uint8_t* rdram) {
    auto& state = g_rocket_v21_queue;
    if (!state.expanded || state.cursor >= state.final_order.size()) return false;

    const std::size_t remaining = state.final_order.size() - state.cursor;
    const std::size_t batch_count = std::min(kRocketV21RetailBatchCapacity, remaining);
    for (std::size_t i = 0; i < batch_count; ++i) {
        RocketV21WriteEntry(rdram, i,
            state.captured[state.final_order[state.cursor + i]]);
    }
    state.cursor += batch_count;
    ++state.batches_staged;
    RocketV21ObserveDisplayListHeadroom(rdram);
    MEM_W(0, RocketV21GuestAddress(kRocketV21QueueEndPointer)) =
        kRocketV21QueueBase + static_cast<std::uint32_t>(batch_count * kRocketV21EntryBytes);
    return true;
}

} // namespace

extern "C" void rocket_render_queue_capture_begin(std::uint8_t*, recomp_context*) {
    auto& state = g_rocket_v21_queue;
    state.captured.clear();
    state.final_order.clear();
    state.cursor = 0U;
    state.batches_staged = 0U;
    state.min_dl_headroom_bytes = std::numeric_limits<std::uint32_t>::max();
    state.capture_active = true;
    state.expanded = false;
    state.continuation = false;
    if (state.captured.capacity() < 512U) state.captured.reserve(512U);
}

extern "C" void rocket_render_queue_capture_entry(std::uint8_t* rdram,
                                                     recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return;
    auto& state = g_rocket_v21_queue;
    if (!state.capture_active) return;

    const std::uint8_t alpha = static_cast<std::uint8_t>(
        MEM_W(0x14, context->r29) & 0xFFU);
    if (alpha == 0U) return;

    RocketV21QueueEntry entry{};
    entry.gfx = static_cast<std::uint32_t>(context->r4);
    entry.mtx1 = static_cast<std::uint32_t>(context->r5);
    entry.mtx2 = static_cast<std::uint32_t>(context->r6);
    entry.depth_bits = static_cast<std::uint32_t>(context->r7);
    const std::uint32_t raw_params = static_cast<std::uint32_t>(
        MEM_W(0x10, context->r29));
    entry.render_params = RocketV21FinalRenderParams(raw_params, alpha);
    entry.alpha = alpha;
    state.captured.push_back(entry);
}

extern "C" void rocket_render_queue_prepare_first_batch(std::uint8_t* rdram,
                                                           recomp_context*) {
    auto& state = g_rocket_v21_queue;
    state.capture_active = false;
    state.cursor = 0U;
    state.continuation = false;

    // Zero-overhead retail path: <=256 submissions are left completely alone.
    if (state.captured.size() <= kRocketV21RetailBatchCapacity) {
        state.expanded = false;
        return;
    }
    if (!RocketV21BuildGlobalOrder()) {
        state.expanded = false;
        return;
    }

    state.expanded = true;
    if (!RocketV21StageNextBatch(rdram)) {
        state.expanded = false;
        return;
    }

    if (!g_rocket_v21_logged_activation.exchange(true, std::memory_order_relaxed)) {
        const std::size_t recovered = state.captured.size() - kRocketV21RetailBatchCapacity;
        std::fprintf(stderr,
            "[render-queue] v21 in-function expansion active: captured %zu entries; "
            "%zu submissions beyond Rocket's retail 256-entry ceiling recovered.\n",
            state.captured.size(), recovered);
        RocketPopDiagWrite(
            "[queue-v21] in-function expansion active captured=%zu recovered_beyond_256=%zu\n",
            state.captured.size(), recovered);
    }
}

extern "C" int rocket_render_queue_batch_preordered(void) {
    return g_rocket_v21_queue.expanded ? 1 : 0;
}

extern "C" int rocket_render_queue_is_continuation(void) {
    return (g_rocket_v21_queue.expanded && g_rocket_v21_queue.continuation) ? 1 : 0;
}

extern "C" int rocket_render_queue_prepare_next_batch(std::uint8_t* rdram,
                                                         recomp_context*) {
    auto& state = g_rocket_v21_queue;
    if (state.expanded) RocketV21ObserveDisplayListHeadroom(rdram);
    if (!state.expanded || state.cursor >= state.final_order.size()) {
        if (state.expanded) {
            const std::uint32_t headroom =
                (state.min_dl_headroom_bytes == std::numeric_limits<std::uint32_t>::max())
                    ? 0U : state.min_dl_headroom_bytes;
            RocketPopDiagWrite(
                "[queue-v21-frame] captured=%zu recovered_beyond_256=%zu batches=%zu "
                "min_dl_headroom_bytes=%u min_dl_headroom_gfx=%u\n",
                state.captured.size(),
                state.captured.size() - kRocketV21RetailBatchCapacity,
                state.batches_staged,
                headroom, headroom / 8U);
        }
        state.continuation = false;
        return 0;
    }
    state.continuation = true;
    return RocketV21StageNextBatch(rdram) ? 1 : 0;
}
// === ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION END ===

