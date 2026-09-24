#!/usr/bin/env python3
from pathlib import Path
import argparse
import ast
import datetime
import re
import runpy
import shutil

BASE_FRAME_BEGIN = r'''extern "C" void rocket_presentation_frame_begin(std::uint8_t*,
                                                  recomp_context*) {
    std::scoped_lock lock(g_mutex);
    ++g_frame;
    g_entries.clear();
    g_key_ordinals.clear();
    MaybeTraceSummary();
}'''

QUEUE_FUNCTION_MARKERS = (
    '[[nodiscard]] float RocketRenderDepth',
    '[[nodiscard]] bool RocketRenderOpaque',
    'void RocketRenderHeapSort',
    'void RocketBuildGlobalRenderOrder',
    'extern "C" void rocket_render_queue_begin',
    'extern "C" void rocket_render_queue_capture',
    'extern "C" void rocket_render_queue_finalize_capture',
    'extern "C" int rocket_render_queue_begin_batch',
    'extern "C" int rocket_render_queue_next',
    'extern "C" void rocket_render_queue_finish_batch',
    'extern "C" int rocket_render_queue_final_pass',
    'extern "C" void rocket_render_queue_prepare_empty_final',
    'extern "C" void rocket_render_queue_end',
    'extern "C" int rocket_render_queue_replay_active',
    'extern "C" void rocket_render_queue_replay_end',
)


def read_text(path: Path):
    raw = path.read_bytes()
    bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw.decode("utf-8-sig")
    nl = "\r\n" if "\r\n" in text else "\n"
    return text, nl, bom


def write_text(path: Path, text: str, nl: str, bom: bool):
    text = text.replace("\r\n", "\n")
    if nl == "\r\n":
        text = text.replace("\n", "\r\n")
    data = text.encode("utf-8")
    if bom:
        data = b"\xef\xbb\xbf" + data
    path.write_bytes(data)


def function_span(text: str, marker: str):
    start = text.find(marker)
    if start < 0:
        raise RuntimeError(f"Function marker not found: {marker}")
    brace = text.find("{", start)
    if brace < 0:
        raise RuntimeError(f"Opening brace not found: {marker}")
    depth = 0
    state = "code"
    i = brace
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""
        if state == "code":
            if c == "/" and n == "/":
                state = "line"; i += 2; continue
            if c == "/" and n == "*":
                state = "block"; i += 2; continue
            if c == '"':
                state = "string"
            elif c == "'":
                state = "char"
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return start, i + 1
        elif state == "line":
            if c == "\n":
                state = "code"
        elif state == "block":
            if c == "*" and n == "/":
                state = "code"; i += 2; continue
        elif state == "string":
            if c == "\\":
                i += 2; continue
            if c == '"':
                state = "code"
        elif state == "char":
            if c == "\\":
                i += 2; continue
            if c == "'":
                state = "code"
        i += 1
    raise RuntimeError(f"Unterminated function: {marker}")


def remove_all_functions(text: str, marker: str):
    count = 0
    while marker in text:
        start, end = function_span(text, marker)
        while end < len(text) and text[end] in "\r\n":
            end += 1
        text = text[:start] + text[end:]
        count += 1
    return text, count


def remove_struct(text: str, name: str):
    marker = f"struct {name} {{"
    count = 0
    while marker in text:
        start = text.find(marker)
        brace = text.find("{", start)
        depth = 0
        state = "code"
        i = brace
        while i < len(text):
            c = text[i]
            n = text[i + 1] if i + 1 < len(text) else ""
            if state == "code":
                if c == "/" and n == "/": state = "line"; i += 2; continue
                if c == "/" and n == "*": state = "block"; i += 2; continue
                if c == '"': state = "string"
                elif c == "'": state = "char"
                elif c == "{": depth += 1
                elif c == "}":
                    depth -= 1
                    if depth == 0:
                        end = i + 1
                        while end < len(text) and text[end] in " \t": end += 1
                        if end < len(text) and text[end] == ";": end += 1
                        while end < len(text) and text[end] in "\r\n": end += 1
                        text = text[:start] + text[end:]
                        count += 1
                        break
            elif state == "line":
                if c == "\n": state = "code"
            elif state == "block":
                if c == "*" and n == "/": state = "code"; i += 2; continue
            elif state == "string":
                if c == "\\": i += 2; continue
                if c == '"': state = "code"
            elif state == "char":
                if c == "\\": i += 2; continue
                if c == "'": state = "code"
            i += 1
        else:
            raise RuntimeError(f"Unterminated struct: {name}")
    return text, count


def load_reference(payload: Path):
    ref = payload / "reference"
    v16 = runpy.run_path(str(ref / "apply_graphics_v16.py"))
    v17 = runpy.run_path(str(ref / "apply_graphics_v17.py"))
    v171 = runpy.run_path(str(ref / "apply_graphics_v17_1.py"))
    return v16, v17, v171


def patch_graphics(path: Path, v171):
    text, nl, bom = read_text(path)
    start, end = function_span(text, 'extern "C" void rocket_graphics_frustum_begin')
    text = text[:start] + v171["REPLACEMENT"] + text[end:]
    write_text(path, text, nl, bom)


def patch_culling(path: Path, v16):
    text, nl, bom = read_text(path)
    if "#include <limits>" not in text:
        anchor = "#include <cstdint>\n"
        if anchor in text:
            text = text.replace(anchor, anchor + "#include <limits>\n", 1)
    start, end = function_span(text, 'extern "C" void rocket_widescreen_frustum_begin')
    text = text[:start] + v16["CULLING_REPLACEMENT"] + text[end:]
    write_text(path, text, nl, bom)


def patch_presentation(path: Path, v17):
    text, nl, bom = read_text(path)
    removed = 0

    text, count = remove_struct(text, "ExpandedRenderEntry")
    removed += count
    for marker in QUEUE_FUNCTION_MARKERS:
        text, count = remove_all_functions(text, marker)
        removed += count

    # Strip every v18/v19 host-expanded queue global/constant. We reconstruct
    # the v17 diagnostic-only state from the original v17 patch function below.
    patterns = (
        r'^constexpr std::size_t kGuestRenderQueueCapacity = 256U;\n?',
        r'^constexpr std::uint32_t kGuestRenderQueueBase = .*?;\n?',
        r'^constexpr std::uint32_t kGuestRenderQueueEndPointerAddress = .*?;\n?',
        r'^std::vector<ExpandedRenderEntry> g_expanded_render_entries;\n?',
        r'^std::vector<std::size_t> g_expanded_render_order;\n?',
        r'^std::size_t g_expanded_render_cursor = .*?;\n?',
        r'^std::size_t g_expanded_render_batch_end = .*?;\n?',
        r'^std::size_t g_expanded_render_opaque_count = .*?;\n?',
        r'^std::atomic<bool> g_render_queue_[A-Za-z0-9_]+\{false\};\n?',
        r'^std::atomic<bool> g_logged_render_queue_expansion\{false\};\n?',
        r'^std::atomic<bool> g_logged_render_queue_pressure\{false\};\n?',
        r'^std::atomic<bool> g_logged_render_queue_saturation\{false\};\n?',
    )
    for pattern in patterns:
        text, count = re.subn(pattern, "", text, flags=re.M)
        removed += count

    # Remove the synthetic replay guard from the real presentation identity hook.
    text = text.replace(
        "    if (g_render_queue_replay_loading.load(std::memory_order_acquire)) return;\n",
        "",
    )

    # Restore the pre-v17 frame function first, then let the exact original v17
    # installer add its diagnostic-only pressure/saturation logic.
    start, end = function_span(text, 'extern "C" void rocket_presentation_frame_begin')
    text = text[:start] + BASE_FRAME_BEGIN + text[end:]
    text = v17["install_queue_diagnostic"](text)

    forbidden = (
        "ExpandedRenderEntry",
        "RocketBuildGlobalRenderOrder",
        "RocketRenderHeapSort",
        "rocket_render_queue_begin(",
        "rocket_render_queue_capture(",
        "rocket_render_queue_finalize_capture(",
        "rocket_render_queue_begin_batch(",
        "rocket_render_queue_next(",
        "rocket_render_queue_final_pass(",
        "rocket_render_queue_prepare_empty_final(",
        "rocket_render_queue_end(",
        "rocket_render_queue_replay_active(",
        "rocket_render_queue_replay_end(",
        "g_expanded_render_",
        "g_render_queue_replay_",
        "g_render_queue_final_pass",
        "[render-queue] GLOBAL",
        "[render-queue] EXPANDED",
    )
    leftovers = [token for token in forbidden if token in text]
    if leftovers:
        raise RuntimeError("v18/v19 queue tokens remain: " + ", ".join(leftovers))
    for token in (
        "[render-queue] PRESSURE",
        "[render-queue] SATURATION",
        "visible_attempts - kGuestRenderQueueCapacity",
    ):
        if token not in text:
            raise RuntimeError("v17 diagnostic token missing: " + token)

    write_text(path, text, nl, bom)
    return removed


def patch_oneclick(path: Path):
    text, nl, bom = read_text(path)
    lines = text.replace("\r\n", "\n").splitlines(True)
    out = []
    for line in lines:
        if "patch_render_queue_generated.py" in line:
            continue
        if "Graphics v18.1: N64Recomp regenerates these files every build" in line:
            continue
        if "safe multi-pass render-queue wrapper immediately after CPU generation" in line:
            continue
        out.append(line)
    write_text(path, "".join(out), nl, bom)


def names_in(stmt):
    return {node.id for node in ast.walk(stmt) if isinstance(node, ast.Name)}


def patch_self_check(path: Path):
    text, nl, bom = read_text(path)
    try:
        tree = ast.parse(text)
    except SyntaxError as exc:
        raise RuntimeError(f"self_check.py invalid before recovery: {exc}")
    main = next(
        (node for node in tree.body if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name == "main"),
        None,
    )
    if main is None:
        raise RuntimeError("self_check.py main() missing")

    lines = text.splitlines(keepends=True)
    ranges = []
    prefixes = ("_v16", "_v17", "_v18", "_v19")
    for stmt in main.body:
        seg = ast.get_source_segment(text, stmt) or ""
        if any(name.startswith(prefixes) for name in names_in(stmt)) or "FIXED34/v18" in seg or "Graphics v19" in seg:
            ranges.append((stmt.lineno - 1, stmt.end_lineno))
    for first, last in sorted(ranges, reverse=True):
        del lines[first:last]

    text = "".join(lines)
    tree = ast.parse(text)
    main = next(node for node in tree.body if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name == "main")
    lines = text.splitlines(keepends=True)

    insert_line = None
    for stmt in main.body:
        seg = ast.get_source_segment(text, stmt) or ""
        if "scan_checked_source(root)" in seg:
            insert_line = stmt.lineno - 1
            break
    if insert_line is None:
        for stmt in main.body:
            if isinstance(stmt, ast.Return):
                insert_line = stmt.lineno - 1
                break
    if insert_line is None:
        insert_line = main.end_lineno

    indent = "    "
    if main.body:
        indent = re.match(r"[ \t]*", lines[main.body[0].lineno - 1]).group(0)

    raw_block = """# v19.5 recovery: proven v16/v17.1 renderer baseline; no v18/v19 queue replay.
_v171_root = __import__('pathlib').Path(__file__).resolve().parents[1]
_v171_culling = (_v171_root / 'src' / 'widescreen_culling.cpp').read_text(encoding='utf-8-sig')
_v171_graphics = (_v171_root / 'src' / 'graphics_enhancements.cpp').read_text(encoding='utf-8-sig')
_v171_presentation = (_v171_root / 'src' / 'presentation_identity.cpp').read_text(encoding='utf-8-sig')
_v171_oneclick = (_v171_root / 'scripts' / 'OneClickBuild.ps1').read_text(encoding='utf-8-sig')
_v171_required = (
    'v16 viewport-locked FOV/aspect guard active' in _v171_culling and
    'position_address = static_cast<std::uint32_t>(context->r5)' in _v171_culling and
    'context->r6 = static_cast<gpr>' in _v171_culling and
    'static_cast<std::uint32_t>(context->r7)' in _v171_graphics and
    's.draw_distance_multiplier' in _v171_graphics and
    '[render-queue] SATURATION' in _v171_presentation
)
_v171_forbidden = any(token in (_v171_culling + _v171_presentation + _v171_oneclick) for token in (
    'kNoSideCullRadiusBits = 0x7F7FFFFFU', 'ExpandedRenderEntry',
    'RocketBuildGlobalRenderOrder', 'rocket_render_queue_finalize_capture',
    'patch_render_queue_generated.py', '[render-queue] GLOBAL', '[render-queue] EXPANDED',
)) or 'maximum_detail' in _v171_graphics or 'MEM_W(0x14' in _v171_graphics
if not (_v171_required and not _v171_forbidden):
    raise SystemExit('SOURCE SELF-CHECK FAILED: v19.5 recovery v16/v17.1 renderer baseline missing')
"""
    block = "".join(indent + line if line.strip() else line for line in raw_block.splitlines(True))
    lines[insert_line:insert_line] = [block]
    text = "".join(lines)
    ast.parse(text)
    write_text(path, text, nl, bom)


def verify(root: Path):
    culling = (root / "src/widescreen_culling.cpp").read_text(encoding="utf-8-sig")
    graphics = (root / "src/graphics_enhancements.cpp").read_text(encoding="utf-8-sig")
    presentation = (root / "src/presentation_identity.cpp").read_text(encoding="utf-8-sig")
    oneclick = (root / "scripts/OneClickBuild.ps1").read_text(encoding="utf-8-sig")
    selfcheck = (root / "scripts/self_check.py").read_text(encoding="utf-8-sig")

    for token in (
        "v16 viewport-locked FOV/aspect guard active",
        "position_address = static_cast<std::uint32_t>(context->r5)",
        "context->r6 = static_cast<gpr>",
    ):
        if token not in culling:
            raise RuntimeError("v16 culling token missing: " + token)
    start, end = function_span(graphics, 'extern "C" void rocket_graphics_frustum_begin')
    gfx_body = graphics[start:end]
    for token in (
        "static_cast<std::uint32_t>(context->r7)",
        "s.draw_distance_multiplier",
        "context->r7 = static_cast<gpr>",
    ):
        if token not in gfx_body:
            raise RuntimeError("v17.1 Draw Distance token missing: " + token)
    if "maximum_detail" in gfx_body or "MEM_W(0x14" in gfx_body:
        raise RuntimeError("retired Draw Distance ABI/settings token remains")
    for token in ("[render-queue] PRESSURE", "[render-queue] SATURATION"):
        if token not in presentation:
            raise RuntimeError("v17 diagnostic token missing: " + token)
    for token in (
        "kNoSideCullRadiusBits = 0x7F7FFFFFU",
        "ExpandedRenderEntry",
        "RocketBuildGlobalRenderOrder",
        "rocket_render_queue_finalize_capture",
        "[render-queue] GLOBAL",
        "[render-queue] EXPANDED",
    ):
        if token in culling + presentation:
            raise RuntimeError("retired v18/v19 token remains: " + token)
    if "patch_render_queue_generated.py" in oneclick:
        raise RuntimeError("OneClick still invokes retired generated queue patcher")
    if "# v19.5 recovery:" not in selfcheck:
        raise RuntimeError("recovery self-check block missing")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--payload", required=True)
    args = ap.parse_args()
    root = Path(args.root).resolve()
    payload = Path(args.payload).resolve()
    v16, v17, v171 = load_reference(payload)

    required = [
        root / "src/graphics_enhancements.cpp",
        root / "src/widescreen_culling.cpp",
        root / "src/presentation_identity.cpp",
        root / "scripts/OneClickBuild.ps1",
        root / "scripts/self_check.py",
    ]
    for path in required:
        if not path.is_file():
            raise RuntimeError("Required recovery file missing: " + str(path))

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = root / "build/repair-backups" / ("graphics-v19.5-recovery-" + stamp)
    backup.mkdir(parents=True, exist_ok=True)
    for path in required:
        dest = backup / path.relative_to(root)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, dest)
    for extra in (
        root / "scripts/patch_render_queue_generated.py",
        root / "runtime-recomp/RecompiledFuncs/funcs_21.c",
        root / "runtime-recomp/RecompiledFuncs/funcs_27.c",
    ):
        if extra.is_file():
            dest = backup / extra.relative_to(root)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(extra, dest)
    print("[OK] Backup: " + str(backup))

    patch_graphics(required[0], v171)
    patch_culling(required[1], v16)
    removed = patch_presentation(required[2], v17)
    patch_oneclick(required[3])
    patch_self_check(required[4])

    patcher = root / "scripts/patch_render_queue_generated.py"
    if patcher.exists():
        patcher.unlink()

    # Current generated CPU source is v19-wrapped. Remove it so a stale incremental
    # build cannot accidentally launch the broken renderer. OneClick/N64Recomp will
    # regenerate clean retail functions after the persistent hook has been removed.
    for generated in (
        root / "runtime-recomp/RecompiledFuncs/funcs_21.c",
        root / "runtime-recomp/RecompiledFuncs/funcs_27.c",
    ):
        if generated.exists():
            generated.unlink()

    verify(root)
    print("[OK] Restored exact historical v17.1 r7 Draw Distance hook.")
    print("[OK] Restored exact historical v16 viewport/FOV culling hook.")
    print(f"[OK] Removed {removed} v18/v19 queue helper/global blocks and reconstructed the exact v17 diagnostic-only queue policy.")
    print("[OK] Removed the persistent v18/v19 generated-renderer hook from OneClickBuild.")
    print("[OK] Removed stale v19 funcs_21.c/funcs_27.c so they cannot be reused accidentally.")
    print("[OK] runtime_ui.cpp was deliberately left untouched; Comic Sans overlay fixes are preserved.")
    print("[OK] v19.5 recovery structural verification PASS.")
    print("[NEXT] Run ONE-CLICK-BUILD.cmd. Full CPU regeneration is REQUIRED before launching Rocket-R.")


if __name__ == "__main__":
    main()
