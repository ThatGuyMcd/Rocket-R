// Compile the production bridge in this TU so fixtures can inspect the recorded
// sidecar before RT64 consumes it. No ROM, renderer or generated C is needed.
#include "../src/presentation_identity.cpp"
#include "hle/rt64_rigid_body.h"
#include "rt64_extended_gbi.h"
#include "common/rt64_rocket_sky.h"
#include "shared/rt64_rocket_sky_filter.h"

#include <stdexcept>
#include <string>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::uint32_t kModel = 0x8001ECECU;
constexpr std::uint32_t kEntry = 0x8008B24CU;
constexpr std::uint32_t kObject = 0x80100000U;
constexpr std::uint32_t kSubmodel = 0x80101000U;
constexpr std::uint32_t kHead = 0x80201000U;

struct Fixture {
    std::vector<std::uint8_t> memory = std::vector<std::uint8_t>(8U * 1024U * 1024U);
    std::uint8_t* rdram = memory.data();
    recomp_context ctx{};

    Fixture() {
        rocket_presentation_frame_begin(rdram, &ctx);
        Write(kGfxContextMtxHeadAddress, kHead);
        Write(kObject + kGameObjectSubmodelsOffset, kSubmodel);
        Write(kObject + kGameObjectSubmodelCountOffset, 1U);
        ctx.r4 = RdramAddress(kObject);
        ctx.r5 = RdramAddress(kSubmodel);
        ctx.r29 = RdramAddress(0x803FFFF0U);
        ctx.r31 = RdramAddress(0x80001234U); // deliberately unrelated/stale
    }
    void Write(std::uint32_t address, std::uint32_t value) {
        MEM_W(0, RdramAddress(address)) = value;
    }
    void Enter(std::uint32_t target, std::uint32_t callsite = 0x8001E9CCU) {
        rocket_presentation_callsite(&ctx, target, callsite);
        rocket_presentation_enter_call(&ctx, target);
    }
    void Submodel(std::uint32_t matrix = kHead - 64U) {
        IdentityMatrix(matrix);
        rocket_presentation_submodel_matrix_begin(rdram, &ctx);
        ctx.r2 = RdramAddress(matrix);
        rocket_presentation_submodel_matrix_end(rdram, &ctx);
    }
    void IdentityMatrix(std::uint32_t matrix) {
        for (std::uint32_t i = 0; i < 16U; ++i) {
            MEM_H(0, RdramAddress(matrix + i * 2U)) = (i % 5U == 0U) ? 1 : 0;
            MEM_H(0, RdramAddress(matrix + 32U + i * 2U)) = 0;
        }
    }
    void ModeZero(std::uint32_t matrix) {
        ctx.r19 = RdramAddress(kObject);
        ctx.r16 = RdramAddress(kSubmodel);
        ctx.r17 = RdramAddress(matrix);
        rocket_presentation_tinker_token_matrix(rdram, &ctx);
    }
    void RenderPair(std::uint32_t primary, std::uint32_t secondary) {
        ctx.r4 = RdramAddress(0x80108000U);
        ctx.r5 = RdramAddress(primary);
        ctx.r6 = RdramAddress(secondary);
        Enter(kEntry, 0x8001F084U);
        rocket_presentation_render_entry(rdram, &ctx);
    }
};

void CallsiteConsumption() {
    Fixture f;
    f.Enter(kModel);
    Check(EntryCallsite(&f.ctx, kModel) == 0x8001E9CCU, "explicit caller was not captured");
    rocket_presentation_enter_call(&f.ctx, kModel);
    Check(EntryCallsite(&f.ctx, kModel) == 0U, "callsite was reused");
    rocket_presentation_callsite(&f.ctx, kModel, 0x8001E9CCU);
    rocket_presentation_enter_call(&f.ctx, kEntry);
    Check(EntryCallsite(&f.ctx, kEntry) == 0U, "wrong target borrowed provenance");
    rocket_presentation_enter_call(&f.ctx, kModel);
    Check(EntryCallsite(&f.ctx, kModel) == 0U, "mismatched provenance was not consumed");
    recomp_context other{};
    rocket_presentation_callsite(&other, kModel, 0x8001E9CCU);
    rocket_presentation_enter_call(&f.ctx, kModel);
    Check(EntryCallsite(&f.ctx, kModel) == 0U, "another context borrowed provenance");
}

void ModelEntrySurvivesRestoredRegisters() {
    Fixture f;
    f.Enter(kModel);
    rocket_presentation_model_range_begin(f.rdram, &f.ctx);
    f.Submodel();
    f.Write(kGfxContextMtxHeadAddress, kHead - 128U);
    f.ctx.r19 = 0; // return epilogue has restored the caller's s3
    f.ctx.r4 = 0;  // and a0 is caller-saved
    rocket_presentation_model_range_end(f.rdram, &f.ctx);
    Check(g_model_range_samples.size() == 2U, "restored registers lost the captured model");
    Check(g_model_range_samples[0].identity != 0U, "captured model has no identity");
}

void ModelRequiresProofFromThisInvocation() {
    Fixture f;
    f.Submodel(); // an earlier invocation cannot prove the new range
    f.Enter(kModel);
    rocket_presentation_model_range_begin(f.rdram, &f.ctx);
    f.Write(kGfxContextMtxHeadAddress, kHead - 64U);
    rocket_presentation_model_range_end(f.rdram, &f.ctx);
    Check(g_model_range_samples.empty(), "stale submodel proof was accepted");
    rocket_presentation_frame_begin(f.rdram, &f.ctx);
    f.Write(kGfxContextMtxHeadAddress, kHead);
    f.Enter(kModel);
    rocket_presentation_model_range_begin(f.rdram, &f.ctx);
    f.Submodel(kHead + 64U); // exact owner, but outside this allocation range
    f.Write(kGfxContextMtxHeadAddress, kHead - 64U);
    rocket_presentation_model_range_end(f.rdram, &f.ctx);
    Check(g_model_range_samples.empty(), "out-of-range submodel proof was accepted");
}

void UnprovenCallersSnap() {
    Fixture f;
    rocket_presentation_enter_call(&f.ctx, kModel);
    rocket_presentation_model_range_begin(f.rdram, &f.ctx);
    f.Submodel();
    f.Write(kGfxContextMtxHeadAddress, kHead - 64U);
    rocket_presentation_model_range_end(f.rdram, &f.ctx);
    Check(g_model_range_samples.empty(), "model range used an unproven caller");
    rocket_presentation_shared_mode0_begin(f.rdram, &f.ctx);
    Check(!g_shared_mode0_v43.active, "shared matrix used an unproven caller");
    rocket_presentation_enter_call(&f.ctx, kEntry);
    rocket_presentation_render_entry(f.rdram, &f.ctx);
    Check(g_entries.empty(), "render entry used stale r31");
    rocket_presentation_direct_begin(f.rdram, &f.ctx, 1U);
    Check(!g_direct_captures[1].valid, "direct range used an unproven caller");
}

void ExplicitCallersSeparateEntries() {
    Fixture f;
    f.ctx.r6 = 0;
    f.Enter(kEntry, 0x8001F084U);
    rocket_presentation_render_entry(f.rdram, &f.ctx);
    f.Enter(kEntry, 0x8002CA4CU);
    rocket_presentation_render_entry(f.rdram, &f.ctx);
    Check(g_entries.size() == 2U && g_entries[0].key != g_entries[1].key,
          "different callers with the same stale r31 collapsed");
}

void SharedModeZeroDeduplicates() {
    Fixture f;
    f.IdentityMatrix(kHead - 64U);
    f.Enter(kModel);
    rocket_presentation_shared_mode0_begin(f.rdram, &f.ctx);
    f.ctx.r19 = RdramAddress(kObject);
    f.ctx.r16 = RdramAddress(kSubmodel);
    f.ctx.r17 = RdramAddress(kHead - 64U);
    rocket_presentation_tinker_token_matrix(f.rdram, &f.ctx);
    rocket_presentation_tinker_token_matrix(f.rdram, &f.ctx);
    Check(g_specific_samples.size() == 1U, "shared mode-0 matrix got duplicate identities");
    Check(g_specific_samples[0].rigid_decompose, "shared mode-0 lost rigid rotation");
    rocket_presentation_shared_mode0_end(f.rdram, &f.ctx);
    rocket_presentation_tinker_token_matrix(f.rdram, &f.ctx);
    Check(g_specific_samples.size() == 1U, "ended model scope accepted a matrix");
}

void ConflictsRemainSnapped() {
    Fixture f;
    g_model_range_samples = {{kHead - 64U, 11U}, {kHead - 64U, 22U}, {kHead - 64U, 11U}};
    MatrixMap bindings;
    FinalizeModelRangeBindings(bindings);
    Check(bindings.at(Physical(kHead - 64U)).binding.identity == 0U,
          "A/B/A ownership conflict revived interpolation");
}

void SharedModelInstancesSurviveCulling() {
    Fixture f;
    auto* rdram = f.rdram;
    constexpr std::uint32_t tokenA = 0x80103000U, tokenB = 0x80104000U;
    MEM_B(0, RdramAddress(kSubmodel + 0x20U)) = 1;
    auto draw = [&](std::uint32_t owner, std::uint32_t matrix) {
        f.ctx.r16 = RdramAddress(owner);
        rocket_presentation_tinker_token_draw_begin(f.rdram, &f.ctx);
        f.ctx.r7 = RdramAddress(owner + 0x30U);
        f.Enter(kModel, 0x8006BE94U);
        rocket_presentation_shared_mode0_begin(f.rdram, &f.ctx);
        f.IdentityMatrix(matrix);
        f.Submodel(matrix);
        rocket_presentation_shared_mode0_end(f.rdram, &f.ctx);
        rocket_presentation_tinker_token_draw_end(f.rdram, &f.ctx);
    };
    draw(tokenA, kHead - 64U);
    draw(tokenB, kHead - 128U);
    MatrixMap previous;
    FinalizeSpecificMatrixBindings(previous);
    const auto bIdentity = previous.at(Physical(kHead - 128U)).binding.identity;
    rocket_presentation_frame_begin(f.rdram, &f.ctx);
    draw(tokenB, kHead - 192U); // A has been collected or culled; B moved in draw order.
    MatrixMap current;
    FinalizeSpecificMatrixBindings(current);
    Check(current.at(Physical(kHead - 192U)).binding.identity == bIdentity,
          "remaining token borrowed another instance's identity after culling");
    Check(current.at(Physical(kHead - 192U)).binding.rigid_decompose,
          "camera-relative submodel still uses deforming matrix blending");
}

void CheckRigidRotation(bool firstMatch) {
    constexpr float pi = 3.14159265358979323846F;
    const auto axis = hlslpp::normalize(hlslpp::float3(1.0F, 2.0F, 3.0F));
    const auto previous = hlslpp::float4x4::identity();
    for (float angle : {0.0F, 0.001F, -pi * 0.9F, pi * 0.9F, pi * 0.999F, pi * 1.5F}) {
        const auto current = hlslpp::float4x4(hlslpp::quaternion::rotation_axis(axis, angle));
        RT64::RigidBody body;
        body.updateLinear(previous, current, G_EX_COMPONENT_INTERPOLATE);
        body.updateAngular(previous, current, G_EX_COMPONENT_INTERPOLATE,
            G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP);
        // GameFrame::matchTransform has no previous decomposition on first match.
        if (!firstMatch) body.updateDecomposition(previous, true);
        body.updateDecomposition(current, true);
        const float shortestAngle = angle > pi ? angle - 2.0F * pi : angle;
        for (float weight : {0.0F, 0.25F, 0.5F, 0.75F, 1.0F}) {
            const auto actual = body.lerp(weight, previous, current, true);
            const auto expected = hlslpp::float4x4(hlslpp::quaternion::rotation_axis(axis, shortestAngle * weight));
            for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t column = 0; column < 3; ++column) {
                    Check(std::abs(float(actual[row][column]) - float(expected[row][column])) < 0.002F,
                          firstMatch ? "first matched rotation stretches or jumps" :
                          "rigid rotation does not follow constant angular progress");
                }
            }
        }
    }
}

void RigidRotationKeepsItsAngularProgress() { CheckRigidRotation(false); }
void FirstMatchedRotationStaysRigid() { CheckRigidRotation(true); }

void PersistentOriginsIdentifyEverySubmodelMode() {
    for (std::uint8_t mode = 0U; mode <= 5U; ++mode) {
        Fixture f;
        auto* rdram = f.rdram;
        MEM_B(0, RdramAddress(kSubmodel + 0x20U)) = mode;
        auto draw = [&](std::uint32_t origin, std::uint32_t matrix) {
            f.ctx.r7 = RdramAddress(origin);
            f.Enter(kModel);
            rocket_presentation_shared_mode0_begin(rdram, &f.ctx);
            if (mode == 0U) {
                f.IdentityMatrix(matrix);
                f.ctx.r19 = RdramAddress(kObject);
                f.ctx.r16 = RdramAddress(kSubmodel);
                f.ctx.r17 = RdramAddress(matrix);
                rocket_presentation_tinker_token_matrix(rdram, &f.ctx);
            } else {
                f.Submodel(matrix);
            }
            rocket_presentation_shared_mode0_end(rdram, &f.ctx);
        };
        constexpr auto originA = 0x80103000U, originB = 0x80104000U;
        draw(originA, kHead - 64U);
        draw(originB, kHead - 128U);
        MatrixMap previous;
        FinalizeSpecificMatrixBindings(previous);
        const auto a = previous.at(Physical(kHead - 64U)).binding.identity;
        const auto b = previous.at(Physical(kHead - 128U)).binding.identity;
        Check(a != 0U && b != 0U && a != b, "shared model origins collapsed");
        rocket_presentation_frame_begin(rdram, &f.ctx);
        draw(originB, kHead - 192U);
        draw(originA, kHead - 256U);
        MatrixMap reordered;
        FinalizeSpecificMatrixBindings(reordered);
        Check(reordered.at(Physical(kHead - 192U)).binding.identity == b &&
              reordered.at(Physical(kHead - 256U)).binding.identity == a,
              "draw order changed a persistent model instance's identity");
        Check(reordered.at(Physical(kHead - 192U)).binding.rigid_decompose,
              "a submodel mode still deforms during interpolation");
        rocket_presentation_frame_begin(rdram, &f.ctx);
        draw(originB, kHead - 320U);
        MatrixMap culled;
        FinalizeSpecificMatrixBindings(culled);
        Check(culled.at(Physical(kHead - 320U)).binding.identity == b,
              "culled model changed its neighbour's identity");
    }
}

void AmbiguousInstancesSnapAndBreakContinuity() {
    Fixture f;
    f.Submodel();
    MatrixMap previous;
    FinalizeSpecificMatrixBindings(previous);
    const auto oldIdentity = previous.at(Physical(kHead - 64U)).binding.identity;
    rocket_presentation_frame_begin(f.rdram, &f.ctx);
    f.Submodel(kHead - 128U);
    f.Submodel(kHead - 192U); // same model with no persistent draw-instance proof
    MatrixMap ambiguous;
    FinalizeSpecificMatrixBindings(ambiguous);
    Check(ambiguous.at(Physical(kHead - 128U)).binding.identity == 0U &&
          ambiguous.at(Physical(kHead - 192U)).binding.identity == 0U,
          "ambiguous instances were assigned identities using draw order");
    rocket_presentation_frame_begin(f.rdram, &f.ctx);
    f.Submodel(kHead - 256U);
    MatrixMap next;
    FinalizeSpecificMatrixBindings(next);
    Check(next.at(Physical(kHead - 256U)).binding.identity != oldIdentity,
          "ambiguous lifetime reconnected to an older instance");
}

void NonRigidMatricesKeepTheirIdentity() {
    Fixture f;
    auto* rdram = f.rdram;
    const auto matrix = kHead - 64U;
    f.IdentityMatrix(matrix);
    Check(SupportsRigidInterpolation(rdram, matrix), "identity matrix rejected");
    MEM_H(0, RdramAddress(matrix)) = -2; // mirrored non-uniform scale is valid
    Check(SupportsRigidInterpolation(rdram, matrix), "mirrored rigid scale rejected");
    MEM_H(0, RdramAddress(matrix + 2U)) = 1; // shear: row axes no longer orthogonal
    Check(!SupportsRigidInterpolation(rdram, matrix), "sheared matrix treated as rigid");
    f.IdentityMatrix(matrix);
    MEM_H(0, RdramAddress(matrix)) = 0; // zero-length axis
    rocket_presentation_submodel_matrix_begin(rdram, &f.ctx);
    f.ctx.r2 = RdramAddress(matrix);
    rocket_presentation_submodel_matrix_end(rdram, &f.ctx);
    MatrixMap bindings;
    rocket::presentation::MatrixBinding generic{};
    generic.identity = 123U;
    bindings.emplace(Physical(matrix), BindingRecord{generic});
    FinalizeSpecificMatrixBindings(bindings);
    Check(bindings.at(Physical(matrix)).binding.identity != 0U &&
          !bindings.at(Physical(matrix)).binding.rigid_decompose,
          "a non-rigid matrix lost interpolation instead of using linear blending");
}

void SingularProjectionInterpolates() {
    auto previous = hlslpp::float4x4::identity();
    previous[2][2] = 0.0F; // The game projects shadows onto a plane.
    previous[3][0] = 10.0F;
    auto current = previous;
    current[0][0] = 2.0F;
    current[1][1] = 3.0F;
    current[3][0] = 20.0F;
    RT64::RigidBody body;
    body.updateLinear(previous, current, G_EX_COMPONENT_INTERPOLATE);
    body.updateAngular(previous, current, G_EX_COMPONENT_INTERPOLATE,
        G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP);
    body.updateDecomposition(previous, true);
    body.updateDecomposition(current, true);
    const auto actual = body.lerp(0.5F, previous, current, true);
    Check(float(actual[3][0]) == 15.0F && float(actual[0][0]) == 1.5F &&
          float(actual[1][1]) == 2.0F && float(actual[2][2]) == 0.0F,
          "singular shadow projection snapped instead of interpolating");
}

void SharedShadowAssetsKeepSeparateOwners() {
    Fixture f;
    auto* rdram = f.rdram;
    auto draw = [&](std::uint32_t owner, std::uint32_t matrix) {
        f.ctx.r4 = RdramAddress(kObject);
        f.ctx.r7 = RdramAddress(0x803FFFA0U); // the reused shadow result on the caller's stack
        f.ctx.r17 = RdramAddress(owner + 0x6CU); // s1 in func_8007C2BC
        f.Enter(kModel, 0x8007C3CCU);
        rocket_presentation_shared_mode0_begin(rdram, &f.ctx);
        f.IdentityMatrix(matrix);
        MEM_H(0, RdramAddress(matrix + 20U)) = 0; // zero third basis row, as in func_8007BF84
        f.ModeZero(matrix);
        rocket_presentation_shared_mode0_end(rdram, &f.ctx);
    };
    constexpr auto a = 0x80103000U, b = 0x80104000U;
    draw(a, kHead - 64U);
    draw(b, kHead - 128U);
    MatrixMap previous;
    FinalizeSpecificMatrixBindings(previous);
    const auto aID = previous.at(Physical(kHead - 64U)).binding.identity;
    const auto bID = previous.at(Physical(kHead - 128U)).binding.identity;
    Check(aID != 0U && bID != 0U && aID != bID, "shared shadow assets were snapped or conflated");
    Check(!previous.at(Physical(kHead - 64U)).binding.rigid_decompose,
          "a projected shadow was treated as a rigid object");
    rocket_presentation_frame_begin(rdram, &f.ctx);
    draw(b, kHead - 192U);
    draw(a, kHead - 256U);
    MatrixMap current;
    FinalizeSpecificMatrixBindings(current);
    Check(current.at(Physical(kHead - 192U)).binding.identity == bID &&
          current.at(Physical(kHead - 256U)).binding.identity == aID,
          "shadow ownership changed with render order");
}

void SecondaryRotationReceivesTheInstanceBinding() {
    Fixture f;
    auto draw = [&](std::uint32_t origin, std::uint32_t primary, std::uint32_t secondary) {
        f.ctx.r4 = RdramAddress(kObject);
        f.ctx.r7 = RdramAddress(origin);
        f.Enter(kModel, 0x8004D170U); // verified rotating vehicle-attachment draw
        rocket_presentation_shared_mode0_begin(f.rdram, &f.ctx);
        f.IdentityMatrix(primary);
        f.IdentityMatrix(secondary);
        f.ModeZero(primary);
        f.RenderPair(primary, secondary);
        f.RenderPair(primary, secondary); // several materials reuse the exact pair
        rocket_presentation_shared_mode0_end(f.rdram, &f.ctx);
    };
    draw(0x80103000U, kHead - 64U, kHead - 128U);
    draw(0x80104000U, kHead - 192U, kHead - 256U);
    MatrixMap previous;
    FinalizeTracksAndBindings(previous);
    FinalizeSpecificMatrixBindings(previous);
    const auto first = previous.at(Physical(kHead - 128U)).binding;
    const auto second = previous.at(Physical(kHead - 256U)).binding;
    Check(first.identity != 0U && second.identity != 0U && first.identity != second.identity,
          "secondary transforms do not have distinct draw-instance owners");
    Check(first.rigid_decompose && second.rigid_decompose,
          "the final multiplied rotation matrix bypassed rigid interpolation");
    rocket_presentation_frame_begin(f.rdram, &f.ctx);
    draw(0x80104000U, kHead - 320U, kHead - 384U);
    MatrixMap current;
    FinalizeTracksAndBindings(current);
    FinalizeSpecificMatrixBindings(current);
    Check(current.at(Physical(kHead - 384U)).binding.identity == second.identity,
          "secondary transform changed ownership after culling another instance");
}

void ShadowCallersKeepLinearInterpolation() {
    for (auto caller : {0x8007C298U, 0x8007C3CCU, 0x8007C504U, 0x8007C654U, 0x8007CA04U}) {
        for (auto mode : {0U, 1U}) {
            Fixture f;
            auto* rdram = f.rdram;
            constexpr auto owner = 0x80103000U;
            MEM_B(0, RdramAddress(kSubmodel + 0x20U)) = mode;
            f.ctx.r7 = RdramAddress(caller == 0x8007C298U ? owner + 0x6CU : 0x803FFFA0U);
            f.ctx.r17 = f.ctx.r18 = RdramAddress(owner + 0x6CU);
            f.ctx.r19 = RdramAddress(owner);
            f.Enter(kModel, caller);
            rocket_presentation_shared_mode0_begin(rdram, &f.ctx);
            f.IdentityMatrix(kHead - 64U);
            if (mode == 0U) f.ModeZero(kHead - 64U);
            else f.Submodel(kHead - 64U);
            MatrixMap bindings;
            FinalizeSpecificMatrixBindings(bindings);
            const auto binding = bindings.at(Physical(kHead - 64U)).binding;
            Check(binding.identity != 0U && !binding.rigid_decompose,
                "shadow caller lost linear interpolation when its basis temporarily became orthogonal");
        }
    }
}

void SecondaryBindingsRequireCurrentPrimaryProof() {
    Fixture f;
    f.IdentityMatrix(kHead - 64U);
    f.IdentityMatrix(kHead - 128U);
    f.Enter(kModel, 0x8004D170U);
    rocket_presentation_shared_mode0_begin(f.rdram, &f.ctx);
    f.ModeZero(kHead - 64U);
    rocket_presentation_shared_mode0_end(f.rdram, &f.ctx);
    f.ctx.r4 = RdramAddress(kObject);
    f.Enter(kModel, 0x8004D170U);
    rocket_presentation_shared_mode0_begin(f.rdram, &f.ctx);
    const auto samples = g_specific_samples.size();
    f.RenderPair(kHead - 64U, kHead - 128U);
    Check(g_specific_samples.size() == samples, "secondary borrowed primary proof from another invocation");
    f.ModeZero(kHead - 192U);
    f.ctx.r4 = RdramAddress(0x80108000U);
    f.ctx.r5 = RdramAddress(kHead - 192U);
    f.ctx.r6 = RdramAddress(kHead - 128U);
    f.Enter(kEntry, 0x80025730U); // not the common model renderer's JAL
    rocket_presentation_render_entry(f.rdram, &f.ctx);
    Check(g_specific_samples.size() == samples + 1U, "unproven render caller acquired a secondary binding");
}

void SharedSecondaryConflictsStaySnapped() {
    Fixture f;
    auto draw = [&](std::uint32_t origin, std::uint32_t primary) {
        f.ctx.r4 = RdramAddress(kObject);
        f.ctx.r7 = RdramAddress(origin);
        f.Enter(kModel, 0x8004D170U);
        rocket_presentation_shared_mode0_begin(f.rdram, &f.ctx);
        f.IdentityMatrix(primary);
        f.IdentityMatrix(kHead - 256U);
        f.ModeZero(primary);
        f.RenderPair(primary, kHead - 256U);
        rocket_presentation_shared_mode0_end(f.rdram, &f.ctx);
    };
    draw(0x80103000U, kHead - 64U);
    draw(0x80104000U, kHead - 128U);
    draw(0x80105000U, kHead - 192U);
    MatrixMap bindings;
    FinalizeSpecificMatrixBindings(bindings);
    Check(bindings.at(Physical(kHead - 256U)).binding.identity == 0U,
          "a secondary shared by different transforms revived an ambiguous binding");
}

void ShearedCompositeKeepsLinearInterpolation() {
    Fixture f;
    auto* rdram = f.rdram;
    constexpr auto primary = kHead - 64U, secondary = kHead - 128U;
    f.IdentityMatrix(primary);
    f.IdentityMatrix(secondary);
    MEM_H(0, RdramAddress(primary)) = 2; // nonuniform X scale
    auto element = [&](std::uint32_t index, std::int32_t fixed) {
        MEM_H(0, RdramAddress(secondary + index * 2U)) = fixed >> 16;
        MEM_H(0, RdramAddress(secondary + 32U + index * 2U)) = fixed & 0xFFFF;
    };
    element(0, 46341); element(1, 46341); // 45-degree rotation
    element(4, -46341); element(5, 46341);
    Check(SupportsRigidInterpolation(rdram, primary) && SupportsRigidInterpolation(rdram, secondary),
          "fixture did not contain separately orthogonal matrices");
    f.Enter(kModel, 0x8004D170U);
    rocket_presentation_shared_mode0_begin(rdram, &f.ctx);
    f.ModeZero(primary);
    f.RenderPair(primary, secondary);
    MatrixMap bindings;
    FinalizeSpecificMatrixBindings(bindings);
    const auto binding = bindings.at(Physical(secondary)).binding;
    Check(binding.identity != 0U && !binding.rigid_decompose,
          "a sheared composite was snapped or incorrectly treated as rigid");
}
}

namespace {
void SkyIdentitySurvivesArenaChanges() {
    g_active_task_fail_closed = true;
    g_active_camera_token = 123U;
    g_active_context_dl_start = 0x1000U;
    g_active_context_size = 0x1000U;
    g_active_background_ranges = {{0x1100U, 0x1200U}};
    std::uint32_t first = 0, second = 0, third = 0;
    Check(rocket_presentation_background_display_list(0x1110U, 0x1500U, &first), "sky range was not accepted");
    g_active_context_dl_start = 0x3000U;
    g_active_background_ranges = {{0x3400U, 0x3500U}};
    Check(rocket_presentation_background_display_list(0x3410U, 0x3A00U, &second) && first == second,
          "alternating arena or earlier allocations changed the sky identity");
    Check(rocket_presentation_background_display_list(0x3420U, 0x3A00U, &third) && second != third,
          "separate sky layers shared an identity");
    Check(!rocket_presentation_background_display_list(0x3500U, 0x3A00U, &third), "world/HUD commands entered sky scope");
    ++g_active_camera_token;
    Check(rocket_presentation_background_display_list(0x3410U, 0x3A00U, &third) && second != third,
          "camera discontinuity retained sky history");
    g_active_task_fail_closed = false;
    Check(!rocket_presentation_background_display_list(0x3410U, 0x3A00U, &third), "sky identity escaped its task");
    g_active_background_ranges.clear();
    g_active_context_dl_start = g_active_context_size = g_active_camera_token = 0;
}

void SkyConnectivityMustMatch() {
    using RT64::rocketSkyTopology;
    const auto a = rocketSkyTopology({20,21,22,20,22,23,100,101,102},20,4);
    const auto b = rocketSkyTopology({80,81,82,80,82,83,1,2,3},80,4);
    Check(a.valid && b.valid && a.hash == b.hash && a.triangles == b.triangles,
          "equivalent sky topology depends on frame allocation");
    const auto reordered = rocketSkyTopology({20,22,21,20,23,22},20,4);
    Check(a.hash != reordered.hash, "reordered sky vertices were treated as corresponding");
    Check(!rocketSkyTopology({20,21,100},20,4).valid, "cross-transform triangle accepted for sky interpolation");
    Check(!rocketSkyTopology({},20,4).valid && !rocketSkyTopology({20,21},20,4).valid,
          "missing or truncated sky topology was accepted");
}

void SkyTextureWrapUsesShortestDistance() {
    using RT64::rocketSkyWrappedDelta;
    Check(rocketSkyWrappedDelta(1,127,128) == 2, "forward sky tile wrap reversed direction");
    Check(rocketSkyWrappedDelta(127,1,128) == -2, "reverse sky tile wrap reversed direction");
    Check(rocketSkyWrappedDelta(0,256,128) == 0, "equivalent wrapping coordinates moved");
    Check(rocketSkyWrappedDelta(1,127,0) == -126, "clamped texture acquired wrapping");
    Check(rocketSkyWrappedDelta(4,1,128) == 3, "ordinary scrolling changed speed");
}
}

void SkyRowsRemainInsideTexture() {
    using rocket::presentation::sky_rows;
    for (int row = 0; row <= 271; ++row) {
        for (float delta : {-20.0F, -0.25F, 0.0F, 0.25F, 20.0F}) {
            const auto r = sky_rows(row, float(row) + .25F, float(row) + delta, 512, 256, true);
            Check(r.load >= 0 && r.load + 256 <= 512, "sky texture load exceeded the source image");
            Check(std::min(r.previous, r.current) - r.load >= 0 &&
                  std::max(r.previous, r.current) - r.load + 211 <= 255,
                  "sky presentation sampled outside its loaded texture");
        }
    }
    const auto cut = sky_rows(120,120.25F,10,512,256,false);
    Check(cut.previous == cut.current, "sky interpolated through a camera cut");
    const auto fast = sky_rows(200,200.5F,10,512,256,true);
    Check(fast.previous == fast.current, "fast sky movement sampled unrelated texture memory");
    const auto full = sky_rows(200,200.5F,10,512,512,true);
    Check(full.load == 0 && full.previous == 10, "full sky image needlessly changed texture or lost history");
}

void SkyRectangleScopeIsExact() {
    g_active_task_fail_closed = true;
    g_active_background_ranges = {{0x1100,0x1200,true,.25F,-1.75F}};
    float current=0, previous=0;
    Check(rocket_presentation_sky_rectangle(0x1180,&current,&previous) && current == .25F && previous == -1.75F,
          "sky row endpoints were lost at the task boundary");
    Check(!rocket_presentation_sky_rectangle(0x1200,&current,&previous), "world or HUD rectangle was marked as sky");
    g_active_task_fail_closed = false;
    Check(!rocket_presentation_sky_rectangle(0x1180,&current,&previous), "sky rows escaped their task");
    g_active_background_ranges.clear();
}

void SkyHorizonMatchesWorldProjection() {
    constexpr float pi = 3.14159265358979323846F;
    const float authored = pi / 4;
    for (float degrees : {25.0F, 45.0F, 85.0F, 120.0F}) {
        const float rendered = degrees * pi / 180;
        for (float pitch : {-0.5F, -0.1F, 0.0F, 0.1F, 0.5F}) {
            // Project a point at infinity on the world's horizontal plane.
            const float screen = 107.5F * std::sin(pitch) / (std::cos(pitch) * std::tan(rendered / 2));
            const float sky = rocket::presentation::sky_pitch_displacement(pitch * 240 / authored, authored, rendered);
            Check(std::abs(screen - sky) < .001F, "sky horizon moves at a different rate to world geometry");
        }
    }
}

void SkyLoadUsesAdjustedFov() {
    Fixture f;
    auto* rdram = f.rdram;
    const auto saved = rocket::graphics::settings();
    auto settings = saved;
    settings.fov_offset_degrees = 40;
    rocket::graphics::set_settings(settings);
    constexpr auto texture = 0x80109000U, camera = 0x8010A000U;
    constexpr float authored = .785398163F;
    MEM_H(0, RdramAddress(texture + 2)) = 512;
    f.Write(texture + 0x10, 0x80110000U);
    f.Write(camera + 0xA0, std::bit_cast<std::uint32_t>(authored));
    f.ctx.r19 = RdramAddress(texture);
    f.ctx.r16 = RdramAddress(camera);
    f.ctx.r20 = 256;
    f.ctx.f0.fl = 0.2F * 240 / authored;
    rocket_presentation_sky_rows(rdram, &f.ctx);
    rocket::graphics::set_settings(saved);
    Check(g_sky_capture.valid && g_sky_capture.rows.current > 111 && g_sky_capture.rows.current < 113,
          "sky load still uses the original FOV");
    Check(g_sky_capture.rows.current - g_sky_capture.rows.load + 211 < 256,
          "FOV correction left the loaded sky texture");
    g_sky_capture.valid = false;
    MEM_H(0, RdramAddress(texture + 2)) = 240;
    rocket_presentation_sky_rows(rdram, &f.ctx);
    Check(!g_sky_capture.valid, "sky hook accepted an invalid source range");
}

void PlayerWheelShapeRequiresExactOwner() {
    constexpr auto player = 0x80103000U, matrix = kHead - 64U;
    for (auto caller : {0x800589ECU,0x80058878U}) for (bool exact : {false, true}) {
        Fixture f;
        auto* rdram = f.rdram;
        f.Write(player + 0x268U, exact ? kObject : kObject + 0x100U);
        f.ctx.r19 = RdramAddress(player);
        f.Enter(kModel, caller);
        rocket_presentation_shared_mode0_begin(rdram, &f.ctx);
        f.IdentityMatrix(matrix);
        MEM_H(0, RdramAddress(matrix + 2U)) = 1; // spring shear
        Check(!SupportsRigidInterpolation(rdram, matrix), "wheel fixture is not sheared");
        f.ModeZero(matrix);
        f.IdentityMatrix(matrix-64U);
        f.RenderPair(matrix,matrix-64U);
        MatrixMap bindings;
        FinalizeSpecificMatrixBindings(bindings);
        const auto binding = bindings.at(Physical(matrix)).binding;
        const bool wheel = exact && caller == 0x800589ECU;
        Check(binding.identity != 0 && binding.rigid_decompose == wheel && binding.interpolate_shape == wheel,
              "wheel shape interpolation accepted an unproven owner or rejected the player's wheel");
        const auto rolling = bindings.at(Physical(matrix-64U)).binding;
        Check(rolling.rigid_decompose == wheel && rolling.interpolate_shape == wheel,
              "wheel's secondary rolling transform lost affine interpolation");
        MEM_H(0, RdramAddress(matrix + 20U)) = 0;
        Check(!SupportsWheelDecomposition(rdram, matrix), "singular wheel basis was accepted for decomposition");
    }
}

void WheelShapeAndRotationInterpolateTogether() {
    auto shape = hlslpp::float4x4::identity();
    shape[0][0] = 2;
    shape[1][0] = .25F;
    const auto rotation = hlslpp::float4x4(hlslpp::quaternion::rotation_axis(hlslpp::float3(0,0,1), 1.2F));
    const auto current = hlslpp::mul(shape, rotation);
    RT64::RigidBody body;
    body.updateLinear(shape, current, G_EX_COMPONENT_INTERPOLATE);
    body.updateAngular(shape, current, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE);
    body.updateDecomposition(shape, true);
    body.updateDecomposition(current, true);
    for (float weight : {0.0F, .25F, .5F, .75F, 1.0F}) {
        const auto expected = hlslpp::mul(shape, hlslpp::float4x4(hlslpp::quaternion::rotation_axis(hlslpp::float3(0,0,1), 1.2F * weight)));
        const auto actual = body.lerp(weight, shape, current, true);
        for (unsigned row=0; row<4; ++row) for (unsigned column=0; column<4; ++column)
            Check(std::abs(float(expected[row][column]) - float(actual[row][column])) < .002F,
                  "spring-mounted wheel flattens or jumps while rotating");
    }
}

void SkyFilterRejectsMovingDither() {
    constexpr int bayer[4][4] = {{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
    auto sample = [&](float x, float y) {
        const int ix = int(std::floor(x)), iy = int(std::floor(y));
        const float fx=x-ix, fy=y-iy;
        auto pixel = [&](int a, int b) { return float(bayer[b & 3][a & 3]); };
        return (pixel(ix,iy)*(1-fx)+pixel(ix+1,iy)*fx)*(1-fy) +
            (pixel(ix,iy+1)*(1-fx)+pixel(ix+1,iy+1)*fx)*fy;
    };
    for (float x : {0.0F,.25F,.5F,.75F}) for (int frame=0; frame<80; ++frame) {
        const float y = float(frame) * .137F;
        float sum = 0;
        for (unsigned i=0; i<16; ++i)
            sum += sample(x+RT64::rocketSkyFilterAxis(i&3), y+RT64::rocketSkyFilterAxis(i>>2));
        Check(std::abs(sum/16 - 7.5F) < .0001F, "sky dither pulses as its sampling phase changes");
    }
}

void SkyGradientCoversEveryColumn() {
    // An eight-column pattern is deliberately different in every column.
    const float values[] = {1,8,3,11,0,6,2,9};
    float filtered = 0, expected = 0;
    for (float value : values) expected += value / 8;
    for (unsigned pair=0; pair<4; ++pair) {
        const float x = RT64::rocketSkyGradientColumn(pair);
        const auto column = unsigned(std::floor(x));
        Check(column+1 < 8, "sky filter samples beyond the gradient width");
        const float fraction = x-column;
        filtered += (values[column]*(1-fraction) + values[column+1]*fraction)/4;
    }
    Check(std::abs(filtered-expected) < .0001F, "full-strength sky filter leaves columns of dither unaveraged");
}

int main() {
    const auto tests = {CallsiteConsumption, ModelEntrySurvivesRestoredRegisters,
        ModelRequiresProofFromThisInvocation, UnprovenCallersSnap,
        ExplicitCallersSeparateEntries, SharedModeZeroDeduplicates,
        ConflictsRemainSnapped, RigidRotationKeepsItsAngularProgress,
        SharedModelInstancesSurviveCulling, FirstMatchedRotationStaysRigid,
        PersistentOriginsIdentifyEverySubmodelMode, AmbiguousInstancesSnapAndBreakContinuity,
        NonRigidMatricesKeepTheirIdentity, SingularProjectionInterpolates,
        SharedShadowAssetsKeepSeparateOwners, SecondaryRotationReceivesTheInstanceBinding,
        ShadowCallersKeepLinearInterpolation, SecondaryBindingsRequireCurrentPrimaryProof,
        SharedSecondaryConflictsStaySnapped, ShearedCompositeKeepsLinearInterpolation,
        SkyIdentitySurvivesArenaChanges, SkyConnectivityMustMatch, SkyTextureWrapUsesShortestDistance,
        SkyRowsRemainInsideTexture, SkyRectangleScopeIsExact, SkyHorizonMatchesWorldProjection,
        SkyLoadUsesAdjustedFov, PlayerWheelShapeRequiresExactOwner, WheelShapeAndRotationInterpolateTogether,
        SkyFilterRejectsMovingDither, SkyGradientCoversEveryColumn};
    unsigned failures = 0U;
    for (auto test : tests) {
        try { test(); }
        catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL: %s\n", error.what());
            ++failures;
        }
    }
    std::printf("%zu presentation regression scenarios; %u failures\n", tests.size(), failures);
    return failures == 0U ? 0 : 1;
}
