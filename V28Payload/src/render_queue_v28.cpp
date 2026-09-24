#include "recomp.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

// === ROCKET-R GRAPHICS V28 UNBOUNDED RENDER QUEUE BEGIN ===
//
// Rocket's retail renderer stores at most 256 RenderEntry records.  V28 keeps
// Rocket's original single-pass draw/state machine, but moves RenderEntry
// storage to otherwise-unused Expansion Pak RDRAM and supplies the renderer
// with a host-built final draw order.  There are no 256-entry replay batches.
//
// This file deliberately does NOT alter interpolation, simulation timing,
// culling, draw-distance math, or the GfxTask command/matrix arena.
namespace {

constexpr std::uint32_t kRdramMask = 0x007FFFFFU;
constexpr std::uint32_t kRdramStart = 0x80000000U;
constexpr std::uint32_t kRdramEnd = 0x807FFFFFU;

// Rocket itself hard-codes RAM_END=0x80400000.  This range is therefore outside
// the original 4 MiB game allocator while remaining ordinary 8 MiB N64 RDRAM
// for RT64/N64ModernRuntime.  32,768 entries is 768 KiB of guest storage and is
// intentionally far beyond any realistic Rocket scene.
constexpr std::uint32_t kQueueBase = 0x80480000U;
constexpr std::uint32_t kRetailQueueBase = 0x800ADB00U;
constexpr std::uint32_t kRetailQueueEndPointer = 0x800AF300U;
constexpr std::size_t kRetailQueueCapacity = 256U;
constexpr std::uint32_t kEntryBytes = 0x18U;
constexpr std::size_t kQueueCapacity = 32768U;
constexpr std::uint32_t kQueueBytes =
    static_cast<std::uint32_t>(kQueueCapacity * kEntryBytes);
static_assert(kQueueBase + kQueueBytes <= 0x80600000U);

// gGfxContext.dlHead / descending matrix cursor.  V28 only observes these; it
// never redirects them.  If high draw distance exposes a second independent
// arena limit, the telemetry makes that measurable instead of speculative.
constexpr std::uint32_t kDlHeadAddress = 0x800A5DB0U;
constexpr std::uint32_t kMatrixHeadAddress = 0x800A5DB4U;

struct EntryMeta {
    std::uint32_t guest_address = 0U;
    std::uint32_t depth_bits = 0U;
    std::uint32_t render_params = 0U;
};

struct QueueState {
    std::vector<EntryMeta> entries{};
    std::vector<std::size_t> final_order{};
    std::uint64_t frame = 0U;
    std::uint64_t attempts = 0U;
    std::uint64_t alpha_zero = 0U;
    std::uint64_t dropped = 0U;
    std::uint64_t emitted = 0U;
    std::uint64_t selection_errors = 0U;
    std::size_t retail_mirror_count = 0U;
    std::uint32_t min_dl_headroom = std::numeric_limits<std::uint32_t>::max();
    bool active = false;
    bool prepared = false;
};

thread_local QueueState g_queue{};
std::atomic<bool> g_logged_activation{false};
std::atomic<bool> g_logged_pressure{false};
std::atomic<bool> g_logged_capacity{false};
std::atomic<bool> g_logged_gfx_collision{false};

[[nodiscard]] gpr GuestAddress(std::uint32_t address) {
    return static_cast<gpr>(
        static_cast<std::int64_t>(static_cast<std::int32_t>(address)));
}

[[nodiscard]] bool ValidRange(std::uint32_t address, std::uint32_t bytes) {
    if (address < kRdramStart || address > kRdramEnd) return false;
    const std::uint32_t physical = address & kRdramMask;
    return bytes <= (kRdramMask + 1U) &&
           physical <= (kRdramMask + 1U - bytes);
}

[[nodiscard]] std::uint32_t ReadU32(std::uint32_t address) {
    return static_cast<std::uint32_t>(MEM_W(0, GuestAddress(address)));
}

void WriteU32(std::uint32_t address, std::uint32_t value) {
    MEM_W(0, GuestAddress(address)) = static_cast<gpr>(value);
}

void WriteU8(std::uint32_t address, std::uint8_t value) {
    MEM_B(0, GuestAddress(address)) = value;
}

[[nodiscard]] bool TraceEnabled() {
    const char* value = std::getenv("ROCKET_RENDER_TRACE");
    return value != nullptr && value[0] != '\0' &&
           !(value[0] == '0' && value[1] == '\0');
}

void AppendLog(const char* format, ...) {
    std::FILE* file = std::fopen("Rocket-R-renderer-v28.log", "ab");
    if (file == nullptr) return;
    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fflush(file);
    std::fclose(file);
}

[[nodiscard]] std::uint32_t DisplayListHeadroom() {
    const std::uint32_t dl_head = ReadU32(kDlHeadAddress);
    const std::uint32_t matrix_head = ReadU32(kMatrixHeadAddress);
    if (!ValidRange(dl_head, 1U) || !ValidRange(matrix_head, 1U) ||
        matrix_head < dl_head) {
        return 0U;
    }
    return matrix_head - dl_head;
}

void ObserveHeadroom() {
    g_queue.min_dl_headroom =
        std::min(g_queue.min_dl_headroom, DisplayListHeadroom());
}

[[nodiscard]] std::uint32_t FinalRenderParams(std::uint32_t raw,
                                               std::uint8_t alpha) {
    if (alpha >= 0xFFU) return raw;

    // Exact source-level equivalent of Rocket's faded-entry rewrite:
    // unk_make_RenderParams(2, 2, unk1, unk2 == 3 ? 5 : 4).
    const std::uint8_t unk1 = static_cast<std::uint8_t>((raw >> 16U) & 0xFFU);
    std::uint8_t unk2 = static_cast<std::uint8_t>((raw >> 8U) & 0xFFU);
    unk2 = (unk2 == 3U) ? 5U : 4U;
    const std::uint8_t render_mode = (unk2 == 5U) ? 7U : 6U;
    return (0x22U << 24U) |
           (static_cast<std::uint32_t>(unk1) << 16U) |
           (static_cast<std::uint32_t>(unk2) << 8U) |
           static_cast<std::uint32_t>(render_mode);
}

[[nodiscard]] std::uint8_t RenderClass(const EntryMeta& entry) {
    return static_cast<std::uint8_t>((entry.render_params >> 28U) & 0x0FU);
}

[[nodiscard]] float Depth(const EntryMeta& entry) {
    return std::bit_cast<float>(entry.depth_bits);
}

void HeapSortSegment(std::vector<std::size_t>& order,
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
                const float left = Depth(
                    g_queue.entries[order[begin + static_cast<std::size_t>(child)]]);
                const float right = Depth(
                    g_queue.entries[order[begin + static_cast<std::size_t>(child + 1)]]);
                if (left < right) ++child;
            }

            const float parent_depth = Depth(
                g_queue.entries[order[begin + static_cast<std::size_t>(parent)]]);
            const float child_depth = Depth(
                g_queue.entries[order[begin + static_cast<std::size_t>(child)]]);
            if (!(parent_depth < child_depth)) break;

            std::swap(order[begin + static_cast<std::size_t>(parent)],
                      order[begin + static_cast<std::size_t>(child)]);
            parent = child;
            child += child + 1;
        }
    }
}

void BuildFinalOrder() {
    const std::size_t count = g_queue.entries.size();
    g_queue.final_order.resize(count);
    for (std::size_t i = 0; i < count; ++i) g_queue.final_order[i] = i;
    if (count == 0U) return;

    // Exact source-level reproduction of divide_opaque_and_transparent().
    std::size_t opaque_end = 0U;
    std::size_t transparent_start = count - 1U;
    while (true) {
        while (RenderClass(g_queue.entries[g_queue.final_order[opaque_end]]) == 1U &&
               opaque_end < transparent_start) {
            ++opaque_end;
        }
        while (RenderClass(g_queue.entries[g_queue.final_order[transparent_start]]) == 2U &&
               opaque_end < transparent_start) {
            --transparent_start;
        }
        if (opaque_end >= transparent_start) break;
        std::swap(g_queue.final_order[opaque_end],
                  g_queue.final_order[transparent_start]);
        ++opaque_end;
        --transparent_start;
    }
    if (RenderClass(g_queue.entries[g_queue.final_order[opaque_end]]) == 1U) {
        ++opaque_end;
    }

    // Exact source-level heap sort for opaque and transparent partitions.
    HeapSortSegment(g_queue.final_order, 0U, opaque_end);
    HeapSortSegment(g_queue.final_order, opaque_end, count - opaque_end);

    // Retail draws opaque forwards, then transparent backwards.  Convert the
    // sorted pointer list into the exact final draw order so the generated draw
    // loop can simply iterate one unbounded list.
    std::vector<std::size_t> draw_order;
    draw_order.reserve(count);
    draw_order.insert(draw_order.end(), g_queue.final_order.begin(),
                      g_queue.final_order.begin() +
                          static_cast<std::ptrdiff_t>(opaque_end));
    for (std::size_t i = count; i > opaque_end; --i) {
        draw_order.push_back(g_queue.final_order[i - 1U]);
    }
    g_queue.final_order.swap(draw_order);
}

} // namespace

extern "C" void rocket_render_queue_v28_begin(std::uint8_t*, recomp_context*) {
    g_queue.entries.clear();
    g_queue.final_order.clear();
    g_queue.attempts = 0U;
    g_queue.alpha_zero = 0U;
    g_queue.dropped = 0U;
    g_queue.emitted = 0U;
    g_queue.selection_errors = 0U;
    g_queue.retail_mirror_count = 0U;
    g_queue.min_dl_headroom = std::numeric_limits<std::uint32_t>::max();
    g_queue.active = true;
    g_queue.prepared = false;
    ++g_queue.frame;
    if (g_queue.entries.capacity() < 1024U) g_queue.entries.reserve(1024U);
    if (g_queue.final_order.capacity() < 1024U) g_queue.final_order.reserve(1024U);
    WriteU32(kRetailQueueEndPointer, kRetailQueueBase);
    ObserveHeadroom();

    if (!g_logged_activation.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
            "[render-queue] v28 active: single-pass Expansion Pak RenderEntry list; "
            "capacity=%zu (retail=256).\n", kQueueCapacity);
        AppendLog(
            "=== Rocket-R v28 stable-v6 interpolation + unbounded render queue ===\n"
            "queue_base=%08X capacity=%zu entry_bytes=%u retail_limit=256 "
            "v21_batches=OFF v27_gfx_arena_redirect=OFF\n",
            kQueueBase, kQueueCapacity, kEntryBytes);
    }
}

extern "C" int rocket_render_queue_v28_add(std::uint8_t*, recomp_context* ctx) {
    if (!g_queue.active || ctx == nullptr) return 0;

    ++g_queue.attempts;
    const std::uint8_t alpha = static_cast<std::uint8_t>(
        static_cast<std::uint32_t>(MEM_W(0x14, ctx->r29)) & 0xFFU);
    if (alpha == 0U) {
        ++g_queue.alpha_zero;
        return 1;
    }

    if (g_queue.entries.size() >= kQueueCapacity) {
        ++g_queue.dropped;
        if (!g_logged_capacity.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr,
                "[render-queue] v28 CAPACITY ERROR: exceeded %zu entries.\n",
                kQueueCapacity);
        }
        return 1;
    }

    const std::size_t index = g_queue.entries.size();
    const std::uint32_t guest =
        kQueueBase + static_cast<std::uint32_t>(index * kEntryBytes);
    if (!ValidRange(guest, kEntryBytes)) {
        ++g_queue.dropped;
        return 1;
    }

    const std::uint32_t raw_params = static_cast<std::uint32_t>(
        MEM_W(0x10, ctx->r29));
    const std::uint32_t final_params = FinalRenderParams(raw_params, alpha);

    WriteU32(guest + 0x00U, static_cast<std::uint32_t>(ctx->r4));
    WriteU32(guest + 0x04U, static_cast<std::uint32_t>(ctx->r5));
    WriteU32(guest + 0x08U, static_cast<std::uint32_t>(ctx->r6));
    WriteU32(guest + 0x0CU, static_cast<std::uint32_t>(ctx->r7));
    WriteU32(guest + 0x10U, final_params);
    WriteU8(guest + 0x14U, alpha);

    // Preserve the legacy queue's observable state for any undecompiled scene
    // traversal code that happens to inspect it. The first 256 entries are an
    // exact retail mirror; later entries exist only in V28's expanded storage.
    if (g_queue.retail_mirror_count < kRetailQueueCapacity) {
        const std::uint32_t retail = kRetailQueueBase + static_cast<std::uint32_t>(
            g_queue.retail_mirror_count * kEntryBytes);
        WriteU32(retail + 0x00U, static_cast<std::uint32_t>(ctx->r4));
        WriteU32(retail + 0x04U, static_cast<std::uint32_t>(ctx->r5));
        WriteU32(retail + 0x08U, static_cast<std::uint32_t>(ctx->r6));
        WriteU32(retail + 0x0CU, static_cast<std::uint32_t>(ctx->r7));
        WriteU32(retail + 0x10U, final_params);
        WriteU8(retail + 0x14U, alpha);
        ++g_queue.retail_mirror_count;
        WriteU32(kRetailQueueEndPointer, kRetailQueueBase + static_cast<std::uint32_t>(
            g_queue.retail_mirror_count * kEntryBytes));
    }

    EntryMeta meta{};
    meta.guest_address = guest;
    meta.depth_bits = static_cast<std::uint32_t>(ctx->r7);
    meta.render_params = final_params;
    g_queue.entries.push_back(meta);
    return 1;
}

extern "C" int rocket_render_queue_v28_prepare(std::uint8_t*, recomp_context*) {
    if (!g_queue.active) return 0;
    BuildFinalOrder();
    g_queue.prepared = true;
    ObserveHeadroom();

    if (g_queue.entries.size() > 256U &&
        !g_logged_pressure.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
            "[render-queue] v28 removed retail ceiling: this frame contains %zu "
            "visible entries (%zu beyond 256).\n",
            g_queue.entries.size(), g_queue.entries.size() - 256U);
    }

    return static_cast<int>(g_queue.final_order.size());
}

extern "C" std::uint32_t rocket_render_queue_v28_entry_address(int index) {
    if (!g_queue.active || !g_queue.prepared || index < 0 ||
        static_cast<std::size_t>(index) >= g_queue.final_order.size()) {
        ++g_queue.selection_errors;
        return 0U;
    }

    ObserveHeadroom();
    const std::size_t source_index =
        g_queue.final_order[static_cast<std::size_t>(index)];
    if (source_index >= g_queue.entries.size()) {
        ++g_queue.selection_errors;
        return 0U;
    }
    ++g_queue.emitted;
    return g_queue.entries[source_index].guest_address;
}

extern "C" void rocket_render_queue_v28_end(std::uint8_t*, recomp_context*) {
    if (!g_queue.active) return;
    ObserveHeadroom();
    const std::uint32_t headroom =
        g_queue.min_dl_headroom == std::numeric_limits<std::uint32_t>::max()
            ? 0U
            : g_queue.min_dl_headroom;

    const bool interesting = TraceEnabled() || g_queue.entries.size() >= 220U ||
        g_queue.dropped != 0U || g_queue.selection_errors != 0U ||
        headroom < 4096U;
    if (interesting) {
        AppendLog(
            "frame=%llu attempts=%llu alpha_zero=%llu captured=%zu ordered=%zu "
            "emitted=%llu dropped=%llu selection_errors=%llu "
            "min_dl_headroom_bytes=%u\n",
            static_cast<unsigned long long>(g_queue.frame),
            static_cast<unsigned long long>(g_queue.attempts),
            static_cast<unsigned long long>(g_queue.alpha_zero),
            g_queue.entries.size(), g_queue.final_order.size(),
            static_cast<unsigned long long>(g_queue.emitted),
            static_cast<unsigned long long>(g_queue.dropped),
            static_cast<unsigned long long>(g_queue.selection_errors),
            headroom);
    }

    if (headroom == 0U &&
        !g_logged_gfx_collision.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
            "[render-queue] v28 WARNING: Gfx command/matrix arena headroom reached "
            "zero. See Rocket-R-renderer-v28.log.\n");
    }

    g_queue.active = false;
    g_queue.prepared = false;
}
// === ROCKET-R GRAPHICS V28 UNBOUNDED RENDER QUEUE END ===
