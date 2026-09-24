#!/usr/bin/env python3
from pathlib import Path
import argparse, datetime, shutil, subprocess, sys, textwrap

HOST_MARKER_BEGIN = '// === ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION BEGIN ==='
HOST_MARKER_END = '// === ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION END ==='

HOST_BLOCK = r'''

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

[[nodiscard]] std::uint32_t RocketV21DisplayListHeadroomBytes() {
    const std::uint32_t dl_head = static_cast<std::uint32_t>(
        MEM_W(0, RocketV21GuestAddress(kRocketV21DlHeadAddress)));
    const std::uint32_t matrix_head = static_cast<std::uint32_t>(
        MEM_W(0, RocketV21GuestAddress(kRocketV21MatrixHeadAddress)));
    if (matrix_head < dl_head) return 0U;
    return matrix_head - dl_head;
}

void RocketV21ObserveDisplayListHeadroom() {
    auto& state = g_rocket_v21_queue;
    state.min_dl_headroom_bytes = std::min(
        state.min_dl_headroom_bytes, RocketV21DisplayListHeadroomBytes());
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
    RocketV21ObserveDisplayListHeadroom();
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
    if (state.expanded) RocketV21ObserveDisplayListHeadroom();
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
'''

SELF_CHECK_V21 = r'''    # v21: preserve v20.2 culling/distance policy while recovering the retail 256-entry queue overflow
    # inside the original func_8008B694 invocation. No capture/replay wrapper and no side-cull bypass.
    _v21_root = __import__('pathlib').Path(__file__).resolve().parents[1]
    _v21_culling = (_v21_root / 'src' / 'widescreen_culling.cpp').read_text(encoding='utf-8-sig')
    _v21_graphics = (_v21_root / 'src' / 'graphics_enhancements.cpp').read_text(encoding='utf-8-sig')
    _v21_presentation = (_v21_root / 'src' / 'presentation_identity.cpp').read_text(encoding='utf-8-sig')
    _v21_oneclick = (_v21_root / 'scripts' / 'OneClickBuild.ps1').read_text(encoding='utf-8-sig')
    _v21_patcher_path = _v21_root / 'scripts' / 'patch_render_queue_expansion_v21_generated.py'
    require(_v21_patcher_path.is_file(), 'v21 generated queue patcher missing')
    _v21_patcher = _v21_patcher_path.read_text(encoding='utf-8-sig')
    _v21_required = (
        'v16 viewport-locked FOV/aspect guard active' in _v21_culling and
        'rocket_popdiag_frustum_call()' in _v21_culling and
        'kDisableCpuSidePlanesBits = 0x7F7FFFFFU' not in _v21_culling and
        'static_cast<std::uint32_t>(context->r7)' in _v21_graphics and
        's.draw_distance_multiplier' in _v21_graphics and
        'ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION BEGIN' in _v21_presentation and
        'rocket_render_queue_prepare_first_batch' in _v21_presentation and
        'rocket_render_queue_prepare_next_batch' in _v21_presentation and
        'patch_popin_diagnostics_generated.py' in _v21_oneclick and
        'patch_render_queue_expansion_v21_generated.py' in _v21_oneclick and
        'ROCKET_QUEUE_V21_PROCESS_BATCH' in _v21_patcher
    )
    _v21_forbidden = any(token in (_v21_presentation + _v21_oneclick) for token in (
        'ExpandedRenderEntry', 'RocketBuildGlobalRenderOrder',
        'rocket_render_queue_begin_batch(', 'patch_render_queue_generated.py',
        'rocket_original_func_8008B694', 'rocket_capture_func_8008B694',
        'rocket_draw_func_8008B694', '[render-queue] GLOBAL',
    ))
    if not (_v21_required and not _v21_forbidden):
        raise SystemExit('SOURCE SELF-CHECK FAILED: v21 in-function render-queue expansion state missing')
'''


def read_text(path: Path):
    raw = path.read_bytes()
    return raw.decode('utf-8-sig'), raw.startswith(b'\xef\xbb\xbf'), ('\r\n' if b'\r\n' in raw else '\n')


def write_text(path: Path, text: str, bom: bool, nl: str):
    text = text.replace('\r\n', '\n')
    if nl == '\r\n': text = text.replace('\n', '\r\n')
    data = text.encode('utf-8')
    if bom: data = b'\xef\xbb\xbf' + data
    path.write_bytes(data)


def patch_presentation(path: Path):
    text, bom, nl = read_text(path)
    if HOST_MARKER_BEGIN in text:
        if text.count(HOST_MARKER_BEGIN) != 1 or text.count(HOST_MARKER_END) != 1:
            raise RuntimeError('v21 presentation block duplicated/corrupt')
        changed = False
    else:
        text = text.rstrip() + HOST_BLOCK + '\n'
        changed = True

    if '#include <cstddef>' not in text:
        anchor = '#include <cstdint>\n'
        if anchor not in text:
            raise RuntimeError('presentation_identity.cpp include anchor missing')
        text = text.replace(anchor, anchor + '#include <cstddef>\n', 1)
        changed = True

    # Make v20 telemetry wording accurate once overflow is recovered by v21.
    replacements = {
        '=== Rocket-R v20.2 ORIGINAL-BASELINE pop-in diagnostic session ===':
            '=== Rocket-R v21 IN-FUNCTION QUEUE-EXPANSION diagnostic session ===',
        'mode=normal-v17.1-renderer side-plane-cpu-cull=V17.1-VIEWPORT-GUARD '
        'draw-distance=r7-preserved retail-distance-fade=preserved':
            'mode=normal-v17.1-renderer side-plane-cpu-cull=V17.1-VIEWPORT-GUARD '
            'draw-distance=r7-preserved retail-distance-fade=preserved queue-overflow=RECOVERED-V21',
        'retail-distance-fade=preserved\\n':
            'retail-distance-fade=preserved queue-overflow=RECOVERED-V21\\n',
        'rejected_at_256\\n': 'would_drop_at_256_recovered\\n',
        'max_guest_queue=%u rejected_at_256=%llu\\n':
            'max_guest_queue=%u would_drop_at_256_recovered=%llu\\n',
        'but Rocket\'s guest render list physically holds only %zu. At least %zu entries were '
        '"\n            "silently rejected by add_render_entry; camera-direction popping can result.\\n"':
            'and Rocket\'s retail staging list holds %zu. v21 recovers %zu overflow entries '
            '"\n            "inside the same renderer invocation.\\n"',
    }
    for old, new in replacements.items():
        if old in text:
            text = text.replace(old, new)
            changed = True

    if changed: write_text(path, text, bom, nl)


def patch_oneclick(path: Path):
    text, bom, nl = read_text(path)
    marker = "Invoke-Python @((Join-Path $Root 'scripts\\patch_render_queue_expansion_v21_generated.py'),'--root',$Root)"
    if marker not in text:
        anchor = "    Invoke-Python @((Join-Path $Root 'scripts\\patch_popin_diagnostics_generated.py'),'--root',$Root)"
        if anchor not in text:
            raise RuntimeError('Could not locate v20 generated telemetry hook in OneClickBuild.ps1')
        insertion = (anchor + "\n\n"
            "    # Graphics v21: recover entries beyond Rocket's retail 256-slot list inside\n"
            "    # the same func_8008B694 invocation. The normal renderer/tail remain authoritative.\n"
            + '    ' + marker.strip() + '\n')
        text = text.replace(anchor, insertion, 1)
        write_text(path, text, bom, nl)


def patch_self_check(path: Path):
    text, bom, nl = read_text(path)
    if '# v21: preserve v20.2 culling/distance policy' in text:
        return
    start = text.find('    # v19.2: distance-only object visibility + globally ordered expanded render queue.')
    if start < 0:
        raise RuntimeError('Could not locate graphics self-check block start')
    end_marker = "    if not (_v202_required and not _v202_forbidden):\n        raise SystemExit('SOURCE SELF-CHECK FAILED: v20.2 baseline-culling telemetry state missing')\n"
    end = text.find(end_marker, start)
    if end < 0:
        raise RuntimeError('Could not locate v20.2 graphics self-check block end')
    end += len(end_marker)
    text = text[:start] + SELF_CHECK_V21 + text[end:]
    write_text(path, text, bom, nl)


def backup(root: Path, files):
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    dest = root / 'build' / 'repair-backups' / f'graphics-v21-{stamp}'
    for rel in files:
        src = root / rel
        if src.is_file():
            out = dest / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, out)
    print(f'[OK] Backup: {dest}')
    return dest


def run(cmd, cwd: Path):
    print('+ ' + ' '.join(str(x) for x in cmd))
    cp = subprocess.run(cmd, cwd=str(cwd))
    if cp.returncode != 0:
        raise RuntimeError(f'Command failed with exit {cp.returncode}: {cmd}')


def verify_source(root: Path):
    p = (root/'src/presentation_identity.cpp').read_text(encoding='utf-8-sig')
    o = (root/'scripts/OneClickBuild.ps1').read_text(encoding='utf-8-sig')
    s = (root/'scripts/self_check.py').read_text(encoding='utf-8-sig')
    required = [HOST_MARKER_BEGIN, 'rocket_render_queue_capture_entry',
                'rocket_render_queue_prepare_first_batch', 'rocket_render_queue_prepare_next_batch']
    for tok in required:
        if p.count(tok) < 1: raise RuntimeError(f'v21 source marker missing: {tok}')
    if "patch_render_queue_expansion_v21_generated.py" not in o:
        raise RuntimeError('OneClickBuild v21 generated hook missing')
    if 'v21 in-function render-queue expansion state missing' not in s:
        raise RuntimeError('self_check v21 policy missing')
    forbidden = ['ExpandedRenderEntry', 'RocketBuildGlobalRenderOrder', 'rocket_original_func_8008B694']
    bad = [t for t in forbidden if t in p]
    if bad: raise RuntimeError('Retired v18/v19 source still active: ' + ', '.join(bad))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    ap.add_argument('--verify-only', action='store_true')
    args = ap.parse_args()
    root = Path(args.root).resolve()
    payload = Path(__file__).resolve().parent

    required = [root/'src/presentation_identity.cpp', root/'scripts/OneClickBuild.ps1',
                root/'scripts/self_check.py', root/'scripts/patch_popin_diagnostics_generated.py']
    for p in required:
        if not p.is_file(): raise RuntimeError(f'Required current v20.2 file missing: {p}')

    target_patcher = root/'scripts/patch_render_queue_expansion_v21_generated.py'
    if not args.verify_only:
        files = ['src/presentation_identity.cpp','scripts/OneClickBuild.ps1','scripts/self_check.py',
                 'scripts/patch_render_queue_expansion_v21_generated.py',
                 'runtime-recomp/RecompiledFuncs/funcs_21.c','runtime-recomp/RecompiledFuncs/funcs_27.c']
        backup(root, files)
        shutil.copy2(payload/'patch_render_queue_expansion_v21_generated.py', target_patcher)
        patch_presentation(root/'src/presentation_identity.cpp')
        patch_oneclick(root/'scripts/OneClickBuild.ps1')
        patch_self_check(root/'scripts/self_check.py')

    verify_source(root)
    run([sys.executable, str(target_patcher), '--root', str(root)] + (['--verify'] if args.verify_only else []), root)
    run([sys.executable, str(root/'scripts/self_check.py')], root)
    print('[OK] Rocket-R Graphics v21 source/generated verification PASS.')
    print('[OK] Retail <=256-entry frames stay on the untouched renderer path; overflow frames are recovered in-function.')

if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        print(f'Graphics v21 migration failed: {exc}', file=sys.stderr)
        raise
