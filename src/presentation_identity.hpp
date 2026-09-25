#pragma once

#include "recomp.h"

#include <cstdint>

#include <cstddef>

namespace rocket::presentation {

struct MatrixBinding {
    std::uint32_t identity = 0U;
    bool interpolate_vertices = false;
    bool interpolate_texcoords = false;
    bool interpolate_tiles = false;
};

struct CoverageStats {
    std::uint64_t entries = 0U;
    std::uint64_t matches = 0U;
    std::uint64_t new_tracks = 0U;
    std::uint64_t conflicts = 0U;
    std::uint64_t ambiguous = 0U;
    std::uint64_t shared_samples = 0U;
    std::uint64_t shared_matches = 0U;
    std::uint64_t shared_rejects = 0U;
    std::uint64_t task_matches = 0U;
    std::uint64_t task_misses = 0U;
    std::uint64_t sidecar_mismatches = 0U;
    std::uint64_t semantic_bindings = 0U;
    std::uint64_t snapped_bindings = 0U;
    std::uint64_t dynamic_vertex_bindings = 0U;
};

CoverageStats coverage_stats();

// Mirrors DKR-R's task-owned semantic sidecar: guest hooks record identities
// while Rocket still owns its unsorted render entries, task submission freezes
// them, and the RT64 decode thread activates only the sidecar belonging to the
// immutable RDRAM snapshot it is parsing.
class TaskIdentityScope {
public:
    TaskIdentityScope(std::uint8_t* rdram_snapshot,
                      std::uint32_t display_list_address);
    ~TaskIdentityScope();
    TaskIdentityScope(const TaskIdentityScope&) = delete;
    TaskIdentityScope& operator=(const TaskIdentityScope&) = delete;
};

bool matrix_binding(std::uint32_t physical_matrix_address,
                    MatrixBinding& binding);

} // namespace rocket::presentation

extern "C" void rocket_presentation_frame_begin(std::uint8_t* rdram,
                                                  recomp_context* context);
extern "C" void rocket_presentation_render_entry(std::uint8_t* rdram,
                                                   recomp_context* context);
extern "C" void rocket_presentation_task_submitted(std::uint8_t* rdram,
                                                     recomp_context* context);

// ROCKET-R INTERPOLATION V35: exact GameObject/Submodel matrix ownership hooks.
extern "C" void rocket_presentation_submodel_matrix_begin(
    std::uint8_t* rdram, recomp_context* context);
extern "C" void rocket_presentation_submodel_matrix_end(
    std::uint8_t* rdram, recomp_context* context);

// ROCKET-R SKYBOX INTERPOLATION V33
// The guest thread records the exact pre-world/background command range; the
// RT64 decode thread uses this task-owned range to identify only the nested
// sky/background display lists that need a semantic presentation group.
extern "C" void rocket_presentation_background_begin(std::uint8_t* rdram,
                                                       recomp_context* context);
extern "C" void rocket_presentation_background_end(std::uint8_t* rdram,
                                                     recomp_context* context);
extern "C" bool rocket_presentation_background_display_list(
    std::uint32_t physical_command_address,
    std::uint32_t physical_target_address,
    std::uint32_t* out_identity);

// C ABI consumed by the pinned RT64 dependency. Physical matrix addresses are
// lookup keys only. During a Rocket task the sidecar is authoritative for all
// model matrices: identity 0 is an explicit G_EX_ID_IGNORE fail-closed binding
// (including unrecognised matrices), while any other valid identity is the
// stable semantic owner token.
struct RocketPresentationMatrixBinding {
    std::uint32_t identity;
    std::uint8_t interpolate_vertices;
    std::uint8_t interpolate_texcoords;
    std::uint8_t interpolate_tiles;
    std::uint8_t reserved;
};
extern "C" bool rocket_presentation_matrix_binding(
    std::uint32_t physical_matrix_address,
    RocketPresentationMatrixBinding* out_binding);
