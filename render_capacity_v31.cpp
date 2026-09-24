#include "recomp.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr std::uint32_t kRetailQueueBase = 0x800ADB00U;
constexpr std::uint32_t kRetailQueueEndPointer = 0x800AF300U;
constexpr std::uint32_t kRenderEntryBytes = 0x18U;
constexpr std::uint32_t kRetailCapacity = 256U;

// IMPORTANT: Rocket itself is a 4 MiB game (RAM_END == 0x80400000).  Model-side
// code therefore continues to use ONLY the original retail queue below that
// boundary.  Extended memory is used solely after scene traversal has finished,
// when Rocket's model/post-processing code can no longer observe RenderEntry
// addresses.
constexpr std::uint32_t kFinalStageBase = 0x80400000U;
constexpr std::uint32_t kFinalStageCapacity = 65536U;

struct RawRenderEntry {
    std::array<std::uint32_t, 6> words{};
};

thread_local std::vector<RawRenderEntry> g_finalized_entries;
thread_local std::uint32_t g_model_scope_depth = 0U;
thread_local std::uint32_t g_max_retail_entries = 0U;
thread_local std::uint32_t g_flush_count = 0U;
thread_local std::uint32_t g_last_reported_total = 0U;
thread_local bool g_bad_retail_pointer_reported = false;

[[nodiscard]] gpr GuestAddress(std::uint32_t address) {
    return static_cast<gpr>(
        static_cast<std::int64_t>(static_cast<std::int32_t>(address)));
}

[[nodiscard]] std::uint32_t ReadWord(std::uint8_t* rdram, std::uint32_t address) {
    return static_cast<std::uint32_t>(MEM_W(0, GuestAddress(address)));
}

void WriteWord(std::uint8_t* rdram, std::uint32_t address, std::uint32_t value) {
    MEM_W(0, GuestAddress(address)) = static_cast<std::int32_t>(value);
}

[[nodiscard]] std::uint32_t RetailCount(std::uint8_t* rdram) {
    const std::uint32_t end = ReadWord(rdram, kRetailQueueEndPointer);
    if (end == 0U || end == kRetailQueueBase) {
        return 0U;
    }

    constexpr std::uint32_t retail_end =
        kRetailQueueBase + kRetailCapacity * kRenderEntryBytes;
    if (end < kRetailQueueBase || end > retail_end ||
        ((end - kRetailQueueBase) % kRenderEntryBytes) != 0U) {
        if (!g_bad_retail_pointer_reported) {
            std::fprintf(stderr,
                "[render-capacity-v31] unexpected retail queue pointer 0x%08X; "
                "leaving it untouched\n",
                end);
            g_bad_retail_pointer_reported = true;
        }
        return 0U;
    }

    return (end - kRetailQueueBase) / kRenderEntryBytes;
}

void CaptureFinalizedRetailEntries(std::uint8_t* rdram) {
    const std::uint32_t count = RetailCount(rdram);
    if (count == 0U) {
        return;
    }

    if (count > g_max_retail_entries) {
        g_max_retail_entries = count;
    }

    g_finalized_entries.reserve(g_finalized_entries.size() + count);
    for (std::uint32_t entry_index = 0; entry_index < count; ++entry_index) {
        RawRenderEntry entry{};
        const std::uint32_t entry_address =
            kRetailQueueBase + entry_index * kRenderEntryBytes;
        for (std::uint32_t word_index = 0; word_index < 6U; ++word_index) {
            entry.words[word_index] =
                ReadWord(rdram, entry_address + word_index * 4U);
        }
        g_finalized_entries.push_back(entry);
    }

    // Reuse Rocket's exact retail 256-entry working area.  This reset only
    // occurs at a safe boundary: before an outer model starts, after its
    // post-processing has finished, or outside model scope immediately before
    // the retail list would otherwise overflow.
    WriteWord(rdram, kRetailQueueEndPointer, kRetailQueueBase);
    ++g_flush_count;
}

void StageFinalizedEntries(std::uint8_t* rdram) {
    const std::size_t count = g_finalized_entries.size();
    if (count > kFinalStageCapacity) {
        std::fprintf(stderr,
            "[render-capacity-v31] FATAL: %zu RenderEntries exceed the no-drop "
            "staging capacity of %u. Nothing will be silently discarded.\n",
            count, kFinalStageCapacity);
        std::abort();
    }

    for (std::size_t entry_index = 0; entry_index < count; ++entry_index) {
        const std::uint32_t entry_address =
            kFinalStageBase +
            static_cast<std::uint32_t>(entry_index) * kRenderEntryBytes;
        const RawRenderEntry& entry = g_finalized_entries[entry_index];
        for (std::uint32_t word_index = 0; word_index < 6U; ++word_index) {
            WriteWord(rdram, entry_address + word_index * 4U,
                      entry.words[word_index]);
        }
    }
}

} // namespace

extern "C" void rocket_render_capacity_v31_frame_begin(
    std::uint8_t* rdram, recomp_context* context) {
    (void)rdram;
    (void)context;
    g_finalized_entries.clear();
    g_model_scope_depth = 0U;
    g_max_retail_entries = 0U;
    g_flush_count = 0U;
    g_bad_retail_pointer_reported = false;
}

extern "C" void rocket_render_capacity_v31_model_begin(
    std::uint8_t* rdram, recomp_context* context) {
    (void)context;

    // Only the OUTERMOST model boundary may recycle the retail working list.
    // Nested/attached models must remain in the same range until the outer
    // model's own post-processing is complete.
    if (g_model_scope_depth == 0U) {
        CaptureFinalizedRetailEntries(rdram);
    }
    ++g_model_scope_depth;
}

extern "C" void rocket_render_capacity_v31_model_end(
    std::uint8_t* rdram, recomp_context* context) {
    (void)context;

    if (g_model_scope_depth == 0U) {
        std::fprintf(stderr,
            "[render-capacity-v31] model scope underflow; refusing to recycle "
            "the retail queue at an unsafe boundary\n");
        return;
    }

    --g_model_scope_depth;
    if (g_model_scope_depth == 0U) {
        // func_80020134 and any other per-model queue post-processing have
        // already completed before this hook runs, so these bytes are final.
        CaptureFinalizedRetailEntries(rdram);
    }
}

extern "C" void rocket_render_capacity_v31_add_guard(
    std::uint8_t* rdram, recomp_context* context) {
    (void)context;

    // Outside a protected model scope there is no outstanding range pointer to
    // the retail list.  Recycle only when the NEXT add would hit the retail cap.
    // Inside model scope we never recycle underneath Rocket's post-processor.
    const std::uint32_t count = RetailCount(rdram);
    if (g_model_scope_depth == 0U && count >= 255U) {
        CaptureFinalizedRetailEntries(rdram);
        return;
    }

    // Never silently reproduce Rocket's 256-entry drop inside one protected
    // model.  Real Rocket models are far below this; if a pathological model
    // ever reaches the retail ceiling, fail loudly instead of causing pop-out.
    if (g_model_scope_depth != 0U && count >= 255U) {
        std::fprintf(stderr,
            "[render-capacity-v31] FATAL: one protected model reached the "
            "255-entry retail working limit. Refusing to silently drop the "
            "next RenderEntry.\n");
        std::abort();
    }
}

extern "C" void rocket_render_capacity_v31_prepare(
    std::uint8_t* rdram, recomp_context* context) {
    if (rdram == nullptr || context == nullptr) {
        return;
    }

    if (g_model_scope_depth != 0U) {
        std::fprintf(stderr,
            "[render-capacity-v31] FATAL: scene traversal ended with model scope "
            "depth %u. Refusing to silently lose RenderEntries.\n",
            g_model_scope_depth);
        std::abort();
    }

    // Pick up terrain/effects/other entries emitted after the last model.
    CaptureFinalizedRetailEntries(rdram);
    StageFinalizedEntries(rdram);

    const std::uint32_t total =
        static_cast<std::uint32_t>(g_finalized_entries.size());
    context->r2 = static_cast<gpr>(total);

    if (total > 256U && total != g_last_reported_total) {
        std::fprintf(stderr,
            "[render-capacity-v31] NO-DROP scene: finalized=%u staged=%u "
            "retail_working_max=%u flushes=%u\n",
            total, total, g_max_retail_entries, g_flush_count);
        g_last_reported_total = total;
    }
}
