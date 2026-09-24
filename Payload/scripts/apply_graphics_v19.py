#!/usr/bin/env python3
from pathlib import Path
import argparse
import re
import ast
import shutil

CULLING_REPLACEMENT = r'''extern "C" void rocket_widescreen_frustum_begin(std::uint8_t* rdram,
                                                   recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return;

    // v19 visibility policy:
    //   - rocket_graphics_frustum_begin() has already scaled r7 using Rocket-R's
    //     Draw Distance slider.
    //   - Retail frustum_test uses r6 ONLY for its four side-plane comparisons.
    //   - Retail distance rejection and the authored final-10-unit alpha fade use
    //     r7 independently.
    //
    // Make the side-plane sphere effectively unbounded so a camera angle can no
    // longer reject an object that is still inside the selected draw distance.
    // The RSP/RT64 projection remains responsible for clipping what is actually
    // off-screen. This does NOT alter r7, the distance fade, or the far-clip
    // scaling owned by the Draw Distance setting.
    constexpr std::uint32_t kNoSideCullRadiusBits = 0x7F7FFFFFU; // FLT_MAX
    context->r6 = static_cast<gpr>(kNoSideCullRadiusBits);

    if (!g_logged_expansion.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
            "[culling] v19 distance-only visibility active: side-plane rejection disabled; "
            "Rocket-R Draw Distance remains on r7 and retail distance fade remains active.\n");
    }
}'''

QUEUE_GLOBAL_OLD_RE = re.compile(
    r'''std::vector<ExpandedRenderEntry> g_expanded_render_entries;\n'''
    r'''std::size_t g_expanded_render_cursor = kGuestRenderQueueCapacity;\n'''
    r'''std::size_t g_expanded_render_batch_end = kGuestRenderQueueCapacity;\n'''
    r'''std::atomic<bool> g_render_queue_replay_active\{false\};\n'''
    r'''std::atomic<bool> g_render_queue_replay_loading\{false\};\n'''
    r'''std::atomic<bool> g_logged_render_queue_expansion\{false\};''')

QUEUE_GLOBAL_NEW = '''std::vector<ExpandedRenderEntry> g_expanded_render_entries;\nstd::vector<std::size_t> g_expanded_render_order;\nstd::size_t g_expanded_render_cursor = 0U;\nstd::size_t g_expanded_render_batch_end = 0U;\nstd::size_t g_expanded_render_opaque_count = 0U;\nstd::atomic<bool> g_render_queue_replay_loading{false};\nstd::atomic<bool> g_render_queue_final_pass{false};\nstd::atomic<bool> g_logged_render_queue_expansion{false};'''

QUEUE_API = r'''[[nodiscard]] float RocketRenderDepth(const ExpandedRenderEntry& entry) {
    return std::bit_cast<float>(static_cast<std::uint32_t>(entry.r7));
}

[[nodiscard]] bool RocketRenderOpaque(const ExpandedRenderEntry& entry) {
    const std::uint8_t alpha = static_cast<std::uint8_t>(entry.stack14 & 0xFFU);
    if (alpha < 0xFFU) {
        // Retail add_render_entry rewrites every partially-transparent entry to
        // unk0_4 == 2 before it reaches the sorter.
        return false;
    }

    // RenderParams is four bytes. On N64 big-endian o32 the first declared
    // 4-bit field (unk0_4) occupies the high nibble of the first byte, which is
    // bits 31..28 of the stack word. Retail considers only unk0_4 == 1 opaque.
    const std::uint32_t unk0_4 = (entry.stack10 >> 28U) & 0x0FU;
    return unk0_4 == 1U;
}

void RocketRenderHeapSort(std::vector<std::size_t>& order,
                          std::size_t start,
                          std::size_t length) {
    // Exact host equivalent of retail func_8008B4BC, generalized to host memory.
    if (length < 2U) return;

    std::size_t end = length;
    std::size_t var_t4 = end / 2U;
    --end;
    while (true) {
        if (var_t4 > 0U) {
            --var_t4;
        } else {
            std::swap(order[start], order[start + end]);
            --end;
            if (!(end > 0U)) break;
        }

        std::size_t var_t2 = var_t4;
        std::size_t var_a2 = (var_t2 * 2U) + 1U;
        while (end >= var_a2) {
            if (var_a2 < end) {
                const float left_depth = RocketRenderDepth(
                    g_expanded_render_entries[order[start + var_a2]]);
                const float right_depth = RocketRenderDepth(
                    g_expanded_render_entries[order[start + var_a2 + 1U]]);
                if (left_depth < right_depth) {
                    ++var_a2;
                }
            }

            const float parent_depth = RocketRenderDepth(
                g_expanded_render_entries[order[start + var_t2]]);
            const float child_depth = RocketRenderDepth(
                g_expanded_render_entries[order[start + var_a2]]);
            if (!(parent_depth < child_depth)) break;

            std::swap(order[start + var_t2], order[start + var_a2]);
            var_t2 = var_a2;
            var_a2 += var_a2 + 1U;
        }
    }
}

void RocketBuildGlobalRenderOrder() {
    const std::size_t total = g_expanded_render_entries.size();
    std::vector<std::size_t> sorted(total);
    for (std::size_t i = 0; i < total; ++i) sorted[i] = i;

    std::size_t opaque_count = 0U;
    if (total > 0U) {
        // Exact binary-class equivalent of retail divide_opaque_and_transparent.
        std::size_t opaque_end = 0U;
        std::size_t transparent_start = total - 1U;
        while (true) {
            while (opaque_end < transparent_start &&
                   RocketRenderOpaque(g_expanded_render_entries[sorted[opaque_end]])) {
                ++opaque_end;
            }
            while (opaque_end < transparent_start &&
                   !RocketRenderOpaque(g_expanded_render_entries[sorted[transparent_start]])) {
                --transparent_start;
            }
            if (opaque_end >= transparent_start) break;
            std::swap(sorted[opaque_end], sorted[transparent_start]);
            ++opaque_end;
            --transparent_start;
        }
        if (RocketRenderOpaque(g_expanded_render_entries[sorted[opaque_end]])) {
            ++opaque_end;
        }
        opaque_count = opaque_end;

        RocketRenderHeapSort(sorted, 0U, opaque_count);
        RocketRenderHeapSort(sorted, opaque_count, total - opaque_count);
    }

    // Retail draws opaque front-to-back, then transparent back-to-front.
    g_expanded_render_opaque_count = opaque_count;
    g_expanded_render_order.clear();
    g_expanded_render_order.reserve(total);
    for (std::size_t i = 0; i < opaque_count; ++i) {
        g_expanded_render_order.push_back(sorted[i]);
    }
    for (std::size_t i = total; i > opaque_count; --i) {
        g_expanded_render_order.push_back(sorted[i - 1U]);
    }
}

extern "C" void rocket_render_queue_begin(std::uint8_t*, recomp_context*) {
    std::scoped_lock lock(g_mutex);
    g_expanded_render_entries.clear();
    g_expanded_render_order.clear();
    if (g_expanded_render_entries.capacity() < 1024U) {
        g_expanded_render_entries.reserve(1024U);
    }
    if (g_expanded_render_order.capacity() < 1024U) {
        g_expanded_render_order.reserve(1024U);
    }
    g_expanded_render_cursor = 0U;
    g_expanded_render_batch_end = 0U;
    g_expanded_render_opaque_count = 0U;
    g_render_queue_replay_loading.store(false, std::memory_order_release);
    g_render_queue_final_pass.store(false, std::memory_order_release);
}

extern "C" void rocket_render_queue_capture(std::uint8_t* rdram,
                                                recomp_context* context) {
    if (rdram == nullptr || context == nullptr ||
        g_render_queue_replay_loading.load(std::memory_order_acquire)) {
        return;
    }
    const std::uint8_t alpha = static_cast<std::uint8_t>(
        MEM_W(0x14, context->r29) & 0xFF);
    if (alpha == 0U) return;

    ExpandedRenderEntry entry{};
    entry.r4 = context->r4;
    entry.r5 = context->r5;
    entry.r6 = context->r6;
    entry.r7 = context->r7;
    entry.stack10 = static_cast<std::uint32_t>(MEM_W(0x10, context->r29));
    entry.stack14 = static_cast<std::uint32_t>(MEM_W(0x14, context->r29));

    std::scoped_lock lock(g_mutex);
    g_expanded_render_entries.push_back(entry);
}

extern "C" void rocket_render_queue_finalize_capture(std::uint8_t*,
                                                         recomp_context*) {
    std::scoped_lock lock(g_mutex);
    RocketBuildGlobalRenderOrder();
    g_expanded_render_cursor = 0U;
    g_expanded_render_batch_end = 0U;
    g_render_queue_replay_loading.store(false, std::memory_order_release);
    g_render_queue_final_pass.store(false, std::memory_order_release);

    if (g_expanded_render_entries.size() > kGuestRenderQueueCapacity &&
        !g_logged_render_queue_expansion.exchange(true, std::memory_order_relaxed)) {
        const std::size_t total = g_expanded_render_entries.size();
        const std::size_t opaque_passes =
            (g_expanded_render_opaque_count + kGuestRenderQueueCapacity - 1U) /
            kGuestRenderQueueCapacity;
        const std::size_t transparent_count =
            total - g_expanded_render_opaque_count;
        const std::size_t transparent_passes =
            (transparent_count + kGuestRenderQueueCapacity - 1U) /
            kGuestRenderQueueCapacity;
        const std::size_t passes = opaque_passes + transparent_passes;
        std::fprintf(stderr,
            "[render-queue] GLOBAL: %zu submissions exceed Rocket's retail %zu-entry "
            "buffer; preserving one global opaque/transparent depth order across %zu "
            "retail-sized draw chunks.\n",
            total, kGuestRenderQueueCapacity, passes);
    }
}

extern "C" int rocket_render_queue_begin_batch(std::uint8_t* rdram,
                                                   recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return 0;
    std::scoped_lock lock(g_mutex);
    if (g_expanded_render_cursor >= g_expanded_render_order.size()) {
        g_render_queue_replay_loading.store(false, std::memory_order_release);
        g_render_queue_final_pass.store(false, std::memory_order_release);
        return 0;
    }

    const std::size_t class_end =
        g_expanded_render_cursor < g_expanded_render_opaque_count
            ? g_expanded_render_opaque_count
            : g_expanded_render_order.size();
    g_expanded_render_batch_end = std::min(
        g_expanded_render_cursor + kGuestRenderQueueCapacity, class_end);
    MEM_W(0, RdramAddress(kGuestRenderQueueEndPointerAddress)) =
        static_cast<std::int32_t>(kGuestRenderQueueBase);
    g_render_queue_replay_loading.store(true, std::memory_order_release);
    g_render_queue_final_pass.store(
        g_expanded_render_batch_end == g_expanded_render_order.size(),
        std::memory_order_release);
    return 1;
}

extern "C" int rocket_render_queue_next(std::uint8_t* rdram,
                                            recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return 0;
    std::scoped_lock lock(g_mutex);
    if (!g_render_queue_replay_loading.load(std::memory_order_acquire) ||
        g_expanded_render_cursor >= g_expanded_render_batch_end) {
        return 0;
    }

    const std::size_t source_index =
        g_expanded_render_order[g_expanded_render_cursor++];
    const ExpandedRenderEntry& entry =
        g_expanded_render_entries[source_index];
    context->r4 = entry.r4;
    context->r5 = entry.r5;
    context->r6 = entry.r6;
    context->r7 = entry.r7;
    MEM_W(0x10, context->r29) = static_cast<std::int32_t>(entry.stack10);
    MEM_W(0x14, context->r29) = static_cast<std::int32_t>(entry.stack14);
    return 1;
}

extern "C" void rocket_render_queue_finish_batch(std::uint8_t*,
                                                     recomp_context*) {
    g_render_queue_replay_loading.store(false, std::memory_order_release);
}

extern "C" int rocket_render_queue_final_pass(void) {
    return g_render_queue_final_pass.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" void rocket_render_queue_prepare_empty_final(std::uint8_t* rdram,
                                                            recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return;
    MEM_W(0, RdramAddress(kGuestRenderQueueEndPointerAddress)) =
        static_cast<std::int32_t>(kGuestRenderQueueBase);
    g_render_queue_replay_loading.store(false, std::memory_order_release);
    g_render_queue_final_pass.store(true, std::memory_order_release);
}

extern "C" void rocket_render_queue_end(void) {
    g_render_queue_replay_loading.store(false, std::memory_order_release);
    g_render_queue_final_pass.store(false, std::memory_order_release);
}'''

SELF_CHECK_BLOCK = r'''# v19.2: distance-only object visibility + globally ordered expanded render queue.
_v19_culling = (root / 'src' / 'widescreen_culling.cpp').read_text(encoding='utf-8-sig')
_v19_graphics = (root / 'src' / 'graphics_enhancements.cpp').read_text(encoding='utf-8-sig')
_v19_presentation = (root / 'src' / 'presentation_identity.cpp').read_text(encoding='utf-8-sig')
_v19_oneclick = (root / 'scripts' / 'OneClickBuild.ps1').read_text(encoding='utf-8-sig')
_v19_patcher_path = root / 'scripts' / 'patch_render_queue_generated.py'
require(_v19_patcher_path.is_file(), 'v19 global render-queue generated patcher is missing')
_v19_patcher = _v19_patcher_path.read_text(encoding='utf-8-sig')
_v19_required = (
    'kNoSideCullRadiusBits = 0x7F7FFFFFU' in _v19_culling and
    'distance-only visibility active' in _v19_culling and
    'static_cast<std::uint32_t>(context->r7)' in _v19_graphics and
    'draw_distance_multiplier' in _v19_graphics and
    'RocketBuildGlobalRenderOrder' in _v19_presentation and
    'RocketRenderHeapSort' in _v19_presentation and
    'rocket_render_queue_finalize_capture' in _v19_presentation and
    'rocket_render_queue_final_pass' in _v19_presentation and
    'ROCKET-R GRAPHICS V19 GLOBAL RENDER QUEUE' in _v19_patcher and
    'rocket_capture_func_8008B694' in _v19_patcher and
    'rocket_draw_func_8008B694' in _v19_patcher and
    'patch_render_queue_generated.py' in _v19_oneclick
)
_v19_forbidden = any(token in _v19_presentation for token in (
    'g_render_queue_replay_active', 'rocket_render_queue_replay_end',
    'replaying safely in %zu retail-sized passes',
)) or 'kTargetFrustumGuard = 1.20F' in _v19_culling
require(_v19_required and not _v19_forbidden,
        'v19 distance-only visibility / global render-order policy missing')
'''


def read_text(path: Path):
    raw = path.read_bytes()
    bom = raw.startswith(b'\xef\xbb\xbf')
    text = raw.decode('utf-8-sig')
    nl = '\r\n' if '\r\n' in text else '\n'
    return text.replace('\r\n', '\n'), nl, bom


def write_text(path: Path, text: str, nl: str, bom: bool):
    text = text.replace('\r\n', '\n')
    if nl == '\r\n':
        text = text.replace('\n', '\r\n')
    data = text.encode('utf-8')
    if bom:
        data = b'\xef\xbb\xbf' + data
    path.write_bytes(data)


def function_span(text: str, marker: str):
    start = text.find(marker)
    if start < 0:
        raise RuntimeError('Missing function: ' + marker)
    brace = text.find('{', start)
    if brace < 0:
        raise RuntimeError('Missing opening brace: ' + marker)
    depth = 0
    state = 'code'
    i = brace
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ''
        if state == 'code':
            if c == '/' and n == '/': state = 'line'; i += 2; continue
            if c == '/' and n == '*': state = 'block'; i += 2; continue
            if c == '"': state = 'string'
            elif c == "'": state = 'char'
            elif c == '{': depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0: return start, i + 1
        elif state == 'line':
            if c == '\n': state = 'code'
        elif state == 'block':
            if c == '*' and n == '/': state = 'code'; i += 2; continue
        elif state == 'string':
            if c == '\\': i += 2; continue
            if c == '"': state = 'code'
        elif state == 'char':
            if c == '\\': i += 2; continue
            if c == "'": state = 'code'
        i += 1
    raise RuntimeError('Unterminated function: ' + marker)


def patch_culling(path: Path):
    text, nl, bom = read_text(path)
    marker = 'extern "C" void rocket_widescreen_frustum_begin'
    start, end = function_span(text, marker)
    text = text[:start] + CULLING_REPLACEMENT + text[end:]
    write_text(path, text, nl, bom)


def patch_presentation(path: Path):
    text, nl, bom = read_text(path)

    # Upgrade the v18 global state without disturbing interpolation identity data.
    m = QUEUE_GLOBAL_OLD_RE.search(text)
    if m:
        text = text[:m.start()] + QUEUE_GLOBAL_NEW + text[m.end():]
    elif 'std::vector<std::size_t> g_expanded_render_order;' not in text:
        raise RuntimeError('Known v18 render-queue global state not found')

    queue_start = text.find('extern "C" void rocket_render_queue_begin(')
    if queue_start < 0:
        raise RuntimeError('v18/v19 queue API start not found')

    # v19 helper functions live immediately before rocket_render_queue_begin().
    # Earlier v19 installers incorrectly replaced only from queue_start, which
    # preserved the old helpers and inserted another copy on every rerun.
    # If any v19 helper copy already exists, replace from the FIRST helper so
    # partial/rerun states are canonicalized to one complete QUEUE_API block.
    helper_start = text.find('[[nodiscard]] float RocketRenderDepth(')
    start = helper_start if 0 <= helper_start < queue_start else queue_start

    # v18 ends at rocket_render_queue_replay_end; v19 ends at rocket_render_queue_end.
    if 'extern "C" void rocket_render_queue_replay_end(' in text[queue_start:]:
        _, end = function_span(text, 'extern "C" void rocket_render_queue_replay_end')
    elif 'extern "C" void rocket_render_queue_end(' in text[queue_start:]:
        _, end = function_span(text, 'extern "C" void rocket_render_queue_end')
    else:
        raise RuntimeError('render-queue API end function not found')
    text = text[:start] + QUEUE_API + text[end:]

    # Presentation identity must ignore synthetic add_render_entry loads, but
    # no longer needs the old replay-active state.
    if 'g_render_queue_replay_loading.load(std::memory_order_acquire)' not in text:
        raise RuntimeError('presentation replay-loading guard missing')

    write_text(path, text, nl, bom)


def patch_oneclick(path: Path):
    text, nl, bom = read_text(path)
    text = text.replace(
        '# Graphics v18.1: N64Recomp regenerates these files every build, so apply the\n'
        '    # safe multi-pass render-queue wrapper immediately after CPU generation.',
        '# Graphics v19: N64Recomp regenerates these files every build, so apply the\n'
        '    # global-order expanded render-queue wrapper immediately after CPU generation.')
    text = text.replace(
        'safe multi-pass render-queue wrapper immediately after CPU generation.',
        'global-order expanded render-queue wrapper immediately after CPU generation.')
    write_text(path, text, nl, bom)


def patch_self_check(path: Path):
    text, nl, bom = read_text(path)

    def parse(src, label):
        try:
            return ast.parse(src)
        except SyntaxError as exc:
            raise RuntimeError(f'{label}: self_check.py is not valid Python: {exc}')

    def names_in(node):
        return {n.id for n in ast.walk(node) if isinstance(n, ast.Name)}

    def is_graphics_stmt(stmt):
        return any(name.startswith(('_v16', '_v17', '_v18', '_v19')) for name in names_in(stmt))

    def is_scan_stmt(stmt):
        for n in ast.walk(stmt):
            if isinstance(n, ast.Call) and isinstance(n.func, ast.Name) and n.func.id == 'scan_checked_source':
                return True
        return False

    def is_pass_print(stmt):
        if not isinstance(stmt, ast.Expr) or not isinstance(stmt.value, ast.Call):
            return False
        call = stmt.value
        if not isinstance(call.func, ast.Name) or call.func.id != 'print':
            return False
        segment = ast.get_source_segment(text, stmt) or ''
        return 'self-check PASS' in segment or 'integrity check complete' in segment

    def is_deps_print(stmt):
        if not isinstance(stmt, ast.Expr) or not isinstance(stmt.value, ast.Call):
            return False
        call = stmt.value
        if not isinstance(call.func, ast.Name) or call.func.id != 'print':
            return False
        segment = ast.get_source_segment(text, stmt) or ''
        return 'Pinned dependencies:' in segment and 'ROM-free source: PASS' in segment

    tree = parse(text, 'pre-migration')
    main_fn = next((n for n in tree.body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and n.name == 'main'), None)
    if main_fn is None:
        raise RuntimeError('Could not locate def main() structurally in self_check.py')

    lines = text.splitlines(keepends=True)
    removals = []

    # Remove active legacy/current graphics statements if they are already inside main().
    main_graphics = [stmt for stmt in main_fn.body if is_graphics_stmt(stmt)]
    if main_graphics:
        first, last = main_graphics[0], main_graphics[-1]
        covered = [stmt for stmt in main_fn.body if first.lineno <= stmt.lineno <= last.end_lineno]
        if any(not is_graphics_stmt(stmt) for stmt in covered):
            raise RuntimeError('Graphics assertions inside main() are not contiguous; refusing ambiguous rewrite')
        a, b = first.lineno - 1, last.end_lineno
        if a > 0 and lines[a - 1].lstrip().startswith(('# v16:', '# v17:', '# v18.', '# v19')):
            a -= 1
        removals.append((a, b))

    # v18.1's historical migration could accidentally end main() and emit its
    # replacement assertions at module scope. Detect and remove only the
    # contiguous module-level graphics statement region after def main().
    module_graphics = [stmt for stmt in tree.body if stmt.lineno > main_fn.end_lineno and is_graphics_stmt(stmt)]
    if module_graphics:
        first, last = module_graphics[0], module_graphics[-1]
        between = [stmt for stmt in tree.body if first.lineno <= stmt.lineno <= last.end_lineno]
        if any(not is_graphics_stmt(stmt) for stmt in between):
            raise RuntimeError('Orphaned module-level graphics assertions are not contiguous; refusing ambiguous rewrite')
        a, b = first.lineno - 1, last.end_lineno
        while a > 0 and lines[a - 1].lstrip().startswith(('# v16:', '# v17:', '# v18.', '# v19')):
            a -= 1
        removals.append((a, b))

    for a, b in sorted(removals, reverse=True):
        del lines[a:b]
    text = ''.join(lines)
    tree = parse(text, 'after legacy graphics removal')
    main_fn = next((n for n in tree.body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and n.name == 'main'), None)
    if main_fn is None:
        raise RuntimeError('def main() disappeared while removing legacy graphics assertions')

    # Insert the v19.2 block immediately before the normal source scan if the
    # tail survived, otherwise append it to main() and reconstruct that tail.
    lines = text.splitlines(keepends=True)
    scan_stmt = next((stmt for stmt in main_fn.body if is_scan_stmt(stmt)), None)
    return_stmt = next((stmt for stmt in reversed(main_fn.body) if isinstance(stmt, ast.Return)), None)
    pass_stmt = next((stmt for stmt in main_fn.body if is_pass_print(stmt)), None)
    deps_stmt = next((stmt for stmt in main_fn.body if is_deps_print(stmt)), None)

    if scan_stmt is not None:
        insert_line = scan_stmt.lineno - 1
        indent = re.match(r'[ \t]*', lines[insert_line]).group(0)
    elif return_stmt is not None:
        insert_line = return_stmt.lineno - 1
        indent = re.match(r'[ \t]*', lines[insert_line]).group(0)
    else:
        insert_line = main_fn.end_lineno
        # Derive main-body indentation from its first statement; FIXED34 uses 4 spaces.
        if main_fn.body:
            indent = re.match(r'[ \t]*', lines[main_fn.body[0].lineno - 1]).group(0)
        else:
            indent = '    '

    block = '\n'.join((indent + line if line else '') for line in SELF_CHECK_BLOCK.rstrip().split('\n')) + '\n'
    lines[insert_line:insert_line] = [block]
    text = ''.join(lines)
    tree = parse(text, 'after v19.2 insertion')
    main_fn = next(n for n in tree.body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and n.name == 'main')

    # Recompute after insertion and restore the standard tail only when the old
    # v18.1 truncation actually removed it.
    lines = text.splitlines(keepends=True)
    has_scan = any(is_scan_stmt(stmt) for stmt in main_fn.body)
    has_return = any(isinstance(stmt, ast.Return) for stmt in main_fn.body)
    has_pass = any(is_pass_print(stmt) for stmt in main_fn.body)
    has_deps = any(is_deps_print(stmt) for stmt in main_fn.body)

    tail = ''
    body_indent = re.match(r'[ \t]*', lines[main_fn.body[0].lineno - 1]).group(0) if main_fn.body else '    '
    if not has_scan:
        tail += body_indent + 'scan_checked_source(root)\n'
    if not has_pass:
        tail += body_indent + 'print(f"Rocket-R source self-check PASS ({version}).")\n'
    if not has_deps:
        tail += body_indent + 'print(f"Pinned dependencies: {len(deps)}; patch integrity: PASS; ROM-free source: PASS.")\n'
    if not has_return:
        tail += body_indent + 'return 0\n'
    if tail:
        insert_after = main_fn.end_lineno
        lines[insert_after:insert_after] = [tail]
        text = ''.join(lines)
        tree = parse(text, 'after self-check tail restoration')
        main_fn = next(n for n in tree.body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and n.name == 'main')

    # Restore the normal module runner if the v18.1 truncation deleted it.
    has_runner = False
    for stmt in tree.body:
        if isinstance(stmt, ast.If):
            seg = ast.get_source_segment(text, stmt.test) or ''
            if '__name__' in seg and '__main__' in seg:
                has_runner = True
                break
    if not has_runner:
        if not text.endswith('\n'):
            text += '\n'
        text += (
            '\nif __name__ == "__main__":\n'
            '    try:\n'
            '        raise SystemExit(main())\n'
            '    except Exception as exc:\n'
            '        print(f"SOURCE SELF-CHECK FAILED: {exc}", file=sys.stderr)\n'
            '        raise SystemExit(1)\n'
        )

    # Final structural verification. No wording-based PASS anchor is used.
    out_tree = parse(text, 'v19.2 post-check')
    out_main = next((n for n in out_tree.body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and n.name == 'main'), None)
    if out_main is None:
        raise RuntimeError('v19.2 post-check lost def main()')
    active_names = names_in(out_main)
    if '_v19_culling' not in active_names or '_v19_required' not in active_names:
        raise RuntimeError('v19.2 graphics self-check block is not active inside main()')
    stale = sorted(name for name in active_names if name.startswith(('_v16', '_v17', '_v181', '_v182', '_v183')))
    if stale:
        raise RuntimeError('active legacy graphics self-check variables remain after v19.2 migration: ' + ', '.join(stale[:8]))
    module_stale = sorted({name for stmt in out_tree.body if stmt is not out_main for name in names_in(stmt)
                           if name.startswith(('_v16', '_v17', '_v181', '_v182', '_v183'))})
    if module_stale:
        raise RuntimeError('orphaned module-level legacy graphics variables remain: ' + ', '.join(module_stale[:8]))
    if not any(is_scan_stmt(stmt) for stmt in out_main.body):
        raise RuntimeError('scan_checked_source(root) was not restored inside main()')
    if not any(isinstance(stmt, ast.Return) for stmt in out_main.body):
        raise RuntimeError('return 0 was not restored inside main()')
    runner_ok = any(isinstance(stmt, ast.If) and '__name__' in (ast.get_source_segment(text, stmt.test) or '') and
                    '__main__' in (ast.get_source_segment(text, stmt.test) or '') for stmt in out_tree.body)
    if not runner_ok:
        raise RuntimeError('__main__ runner was not restored')

    write_text(path, text, nl, bom)

def patch_old_verifiers(root: Path):
    # Old v18 verifiers are one-time installer artifacts. Leave them present but
    # make it explicit that v19 supersedes their renderer checks if users run them.
    for name in ('verify_graphics_v18_1.py', 'verify_graphics_v18_2.py', 'verify_graphics_v18_3.py'):
        path = root / 'scripts' / name
        if not path.is_file():
            continue
        text, nl, bom = read_text(path)
        marker = '# Superseded by Rocket-R Graphics v19 renderer policy.'
        if marker not in text:
            text = marker + '\n' + text
            write_text(path, text, nl, bom)


def verify(root: Path):
    c = (root/'src/widescreen_culling.cpp').read_text(encoding='utf-8-sig')
    g = (root/'src/graphics_enhancements.cpp').read_text(encoding='utf-8-sig')
    p = (root/'src/presentation_identity.cpp').read_text(encoding='utf-8-sig')
    q = (root/'scripts/patch_render_queue_generated.py').read_text(encoding='utf-8-sig')
    o = (root/'scripts/OneClickBuild.ps1').read_text(encoding='utf-8-sig')
    sc = (root/'scripts/self_check.py').read_text(encoding='utf-8-sig')

    for token in ('kNoSideCullRadiusBits = 0x7F7FFFFFU', 'distance-only visibility active'):
        if token not in c: raise RuntimeError('v19 culling token missing: '+token)
    for token in ('static_cast<std::uint32_t>(context->r7)', 'draw_distance_multiplier'):
        if token not in g: raise RuntimeError('Draw Distance r7 policy missing: '+token)
    for token in ('RocketBuildGlobalRenderOrder','RocketRenderHeapSort','rocket_render_queue_finalize_capture',
                  'rocket_render_queue_final_pass','g_expanded_render_order'):
        if token not in p: raise RuntimeError('v19 global queue token missing: '+token)
    for signature in (
        '[[nodiscard]] float RocketRenderDepth(const ExpandedRenderEntry& entry)',
        '[[nodiscard]] bool RocketRenderOpaque(const ExpandedRenderEntry& entry)',
        'void RocketRenderHeapSort(std::vector<std::size_t>& order,',
        'void RocketBuildGlobalRenderOrder()',
    ):
        if p.count(signature) != 1:
            raise RuntimeError('v19 global queue helper must exist exactly once: '+signature)
    for forbidden in ('g_render_queue_replay_active','rocket_render_queue_replay_end',
                      'replaying safely in %zu retail-sized passes'):
        if forbidden in p: raise RuntimeError('retired v18 multi-pass token remains: '+forbidden)
    for token in ('ROCKET-R GRAPHICS V19 GLOBAL RENDER QUEUE','rocket_capture_func_8008B694',
                  'rocket_draw_func_8008B694'):
        if token not in q: raise RuntimeError('v19 generated patcher token missing: '+token)
    if 'patch_render_queue_generated.py' not in o:
        raise RuntimeError('OneClickBuild generated queue hook missing')
    try:
        sc_tree = ast.parse(sc)
    except SyntaxError as exc:
        raise RuntimeError(f'self_check.py is invalid Python after v19.2 migration: {exc}')
    sc_main = next((n for n in sc_tree.body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and n.name == 'main'), None)
    if sc_main is None:
        raise RuntimeError('self_check.py no longer contains def main()')
    sc_names = {n.id for n in ast.walk(sc_main) if isinstance(n, ast.Name)}
    if '_v19_culling' not in sc_names or '_v19_required' not in sc_names:
        raise RuntimeError('v19.2 graphics self-check block is not active inside main()')
    stale_names = sorted(name for name in sc_names if name.startswith(('_v16', '_v17', '_v181', '_v182', '_v183')))
    if stale_names:
        raise RuntimeError('active legacy graphics self-check variables remain: ' + ', '.join(stale_names[:8]))
    sc_segment = ast.get_source_segment(sc, sc_main) or ''
    if 'scan_checked_source(root)' not in sc_segment or 'return 0' not in sc_segment:
        raise RuntimeError('normal self-check tail was not restored')
    runner_ok = any(isinstance(stmt, ast.If) and '__name__' in (ast.get_source_segment(sc, stmt.test) or '') and
                    '__main__' in (ast.get_source_segment(sc, stmt.test) or '') for stmt in sc_tree.body)
    if not runner_ok:
        raise RuntimeError('normal __main__ self-check runner is missing')



def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    ap.add_argument('--payload')
    args = ap.parse_args()
    root = Path(args.root).resolve()
    payload = Path(args.payload).resolve() if args.payload else None

    required = [
        root/'src/widescreen_culling.cpp',
        root/'src/graphics_enhancements.cpp',
        root/'src/presentation_identity.cpp',
        root/'scripts/OneClickBuild.ps1',
        root/'scripts/self_check.py',
    ]
    for path in required:
        if not path.is_file(): raise RuntimeError('Missing required file: '+str(path))

    # Refuse to silently install over a source that has lost the proven r7 slider path.
    graphics = required[1].read_text(encoding='utf-8-sig')
    if 'static_cast<std::uint32_t>(context->r7)' not in graphics or 'draw_distance_multiplier' not in graphics:
        raise RuntimeError('Current Draw Distance r7 slider path is missing; refusing renderer-only migration')

    patch_culling(required[0])
    patch_presentation(required[2])
    patch_oneclick(required[3])
    patch_self_check(required[4])
    if payload is not None:
        src = payload/'patch_render_queue_generated.py'
        if not src.is_file(): raise RuntimeError('v19 generated-code patcher missing from payload')
        shutil.copy2(src, root/'scripts/patch_render_queue_generated.py')

    verify(root)
    print('[OK] v19.2: Draw Distance slider preserved on frustum_test r7; retail distance fade remains active.')
    print('[OK] v19.2: Disabled only the four CPU side-plane rejection tests by supplying FLT_MAX on r6.')
    print('[OK] v19.2: Replaced v18 independent overflow passes with one host-global retail-equivalent render order.')
    print('[OK] v19.2: Guest RenderEntry storage remains 256 slots; ordered output is streamed through safe <=256-entry chunks.')

if __name__ == '__main__':
    main()
