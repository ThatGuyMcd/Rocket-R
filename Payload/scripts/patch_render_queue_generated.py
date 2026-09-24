#!/usr/bin/env python3
from pathlib import Path
import argparse
import re

MARKER = "ROCKET-R GRAPHICS V19 GLOBAL RENDER QUEUE"
V18_MARKER = "ROCKET-R GRAPHICS V18.1 SAFE MULTI-PASS RENDER QUEUE"

DECLS = r'''/* ROCKET-R GRAPHICS V19 GLOBAL RENDER QUEUE */
extern void rocket_render_queue_begin(uint8_t* rdram, recomp_context* ctx);
extern void rocket_render_queue_capture(uint8_t* rdram, recomp_context* ctx);
extern void rocket_render_queue_finalize_capture(uint8_t* rdram, recomp_context* ctx);
extern int rocket_render_queue_begin_batch(uint8_t* rdram, recomp_context* ctx);
extern int rocket_render_queue_next(uint8_t* rdram, recomp_context* ctx);
extern void rocket_render_queue_finish_batch(uint8_t* rdram, recomp_context* ctx);
extern int rocket_render_queue_final_pass(void);
extern void rocket_render_queue_prepare_empty_final(uint8_t* rdram, recomp_context* ctx);
extern void rocket_render_queue_end(void);
'''

WRAPPER = r'''
/* ROCKET-R GRAPHICS V19 GLOBAL RENDER QUEUE
 *
 * Capture the entire scene traversal before drawing anything. Host code then
 * performs Rocket's opaque/transparent partition and exact depth heapsort over
 * ALL submissions, not separately per 256-entry overflow chunk. The globally
 * ordered entries are streamed back through the original add_render_entry()
 * conversion in <=256-entry chunks and drawn by the original renderer body.
 *
 * This keeps the retail guest BSS buffer intact while removing its visibility
 * ceiling and preserving one frame-wide render order.
 */
RECOMP_FUNC void func_8008B694(uint8_t* rdram, recomp_context* ctx) {
    const recomp_context rocket_v19_entry_ctx = *ctx;
    const gpr rocket_v19_saved_r4 = ctx->r4;
    const gpr rocket_v19_saved_r5 = ctx->r5;
    const gpr rocket_v19_saved_sp = ctx->r29;
    const uint32_t rocket_v19_saved_stack10 = MEM_W(0X10, rocket_v19_saved_sp);
    const uint32_t rocket_v19_saved_stack14 = MEM_W(0X14, rocket_v19_saved_sp);
    uint8_t rocket_v19_saved_tail_flags = 0;
    if (rocket_v19_saved_r5 != 0) {
        rocket_v19_saved_tail_flags = MEM_B(0XBC, rocket_v19_saved_r5);
    }

    rocket_render_queue_begin(rdram, ctx);

    /* The capture copy intentionally returns immediately after scene traversal.
     * It runs on a disposable context because that early host return bypasses
     * the guest epilogue; only its guest-memory/display-list side effects are
     * wanted here. */
    recomp_context rocket_v19_capture_ctx = rocket_v19_entry_ctx;
    rocket_capture_func_8008B694(rdram, &rocket_v19_capture_ctx);
    rocket_render_queue_finalize_capture(rdram, ctx);

    int rocket_v19_had_batch = 0;
    recomp_context rocket_v19_final_ctx = rocket_v19_entry_ctx;
    while (rocket_render_queue_begin_batch(rdram, ctx)) {
        rocket_v19_had_batch = 1;
        recomp_context rocket_v19_add_ctx = rocket_v19_entry_ctx;
        rocket_v19_add_ctx.r29 = rocket_v19_saved_sp;
        while (rocket_render_queue_next(rdram, &rocket_v19_add_ctx)) {
            add_render_entry(rdram, &rocket_v19_add_ctx);
            rocket_v19_add_ctx = rocket_v19_entry_ctx;
            rocket_v19_add_ctx.r29 = rocket_v19_saved_sp;
        }
        rocket_render_queue_finish_batch(rdram, ctx);

        const int rocket_v19_final_pass = rocket_render_queue_final_pass();
        if (rocket_v19_saved_r5 != 0) {
            MEM_B(0XBC, rocket_v19_saved_r5) =
                rocket_v19_final_pass ? rocket_v19_saved_tail_flags : 0;
        }

        recomp_context rocket_v19_draw_ctx = rocket_v19_entry_ctx;
        rocket_v19_draw_ctx.r4 = rocket_v19_saved_r4;
        rocket_v19_draw_ctx.r5 = rocket_v19_saved_r5;
        rocket_v19_draw_ctx.r29 = rocket_v19_saved_sp;
        rocket_draw_func_8008B694(rdram, &rocket_v19_draw_ctx);
        if (rocket_v19_final_pass) {
            rocket_v19_final_ctx = rocket_v19_draw_ctx;
        }
    }

    /* An empty scene still needs Rocket's frame-tail/UI path and a normal guest
     * epilogue. Run the draw copy once with an empty queue as the final pass. */
    if (!rocket_v19_had_batch) {
        rocket_render_queue_prepare_empty_final(rdram, ctx);
        if (rocket_v19_saved_r5 != 0) {
            MEM_B(0XBC, rocket_v19_saved_r5) = rocket_v19_saved_tail_flags;
        }
        recomp_context rocket_v19_draw_ctx = rocket_v19_entry_ctx;
        rocket_v19_draw_ctx.r4 = rocket_v19_saved_r4;
        rocket_v19_draw_ctx.r5 = rocket_v19_saved_r5;
        rocket_v19_draw_ctx.r29 = rocket_v19_saved_sp;
        rocket_draw_func_8008B694(rdram, &rocket_v19_draw_ctx);
        rocket_v19_final_ctx = rocket_v19_draw_ctx;
    }

    rocket_render_queue_end();
    MEM_W(0X10, rocket_v19_saved_sp) = rocket_v19_saved_stack10;
    MEM_W(0X14, rocket_v19_saved_sp) = rocket_v19_saved_stack14;
    if (rocket_v19_saved_r5 != 0) {
        MEM_B(0XBC, rocket_v19_saved_r5) = rocket_v19_saved_tail_flags;
    }
    *ctx = rocket_v19_final_ctx;
}
'''


def function_span(text: str, marker: str):
    start = text.find(marker)
    if start < 0:
        raise RuntimeError(f"function marker not found: {marker}")
    brace = text.find("{", start)
    if brace < 0:
        raise RuntimeError(f"opening brace not found: {marker}")
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
                if depth == 0: return start, i + 1
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
    raise RuntimeError(f"unterminated function: {marker}")


def read_text(path: Path):
    raw = path.read_bytes()
    bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw.decode("utf-8-sig")
    nl = "\r\n" if "\r\n" in text else "\n"
    return text.replace("\r\n", "\n"), nl, bom


def write_text(path: Path, text: str, nl: str, bom: bool):
    text = text.replace("\r\n", "\n")
    if nl == "\r\n": text = text.replace("\n", "\r\n")
    data = text.encode("utf-8")
    if bom: data = b"\xef\xbb\xbf" + data
    path.write_bytes(data)


def unwrap_v18_renderer(text: str):
    if V18_MARKER not in text:
        return text

    original_marker = "RECOMP_FUNC void rocket_original_func_8008B694(uint8_t* rdram, recomp_context* ctx)"
    wrapper_marker = "RECOMP_FUNC void func_8008B694(uint8_t* rdram, recomp_context* ctx)"
    os, oe = function_span(text, original_marker)
    ws, we = function_span(text, wrapper_marker)
    original = text[os:oe]
    original = original.replace(original_marker, wrapper_marker, 1)
    original = original.replace(
        "if (!rocket_render_queue_replay_active()) { func_80092050(rdram, ctx); }",
        "func_80092050(rdram, ctx);", 1)
    original = original.replace(
        "if (!rocket_render_queue_replay_active()) { func_8008AEA0(rdram, ctx); }",
        "func_8008AEA0(rdram, ctx);", 1)
    original = re.sub(
        r"if \(!rocket_render_queue_replay_active\(\)\) \{\s*"
        r"(rocket_presentation_(?:task|frame)_submitted\(rdram, ctx\);)\s*\}",
        r"\1", original)

    decl_start = text.rfind("/* " + V18_MARKER + " */", 0, os)
    if decl_start < 0:
        raise RuntimeError("v18 renderer declarations marker missing")
    if ws < oe:
        raise RuntimeError("v18 wrapper unexpectedly precedes retail renderer copy")
    return text[:decl_start] + original + text[we:]


def build_capture_copy(base: str):
    marker = "RECOMP_FUNC void func_8008B694(uint8_t* rdram, recomp_context* ctx)"
    copy = base.replace(marker,
        "RECOMP_FUNC void rocket_capture_func_8008B694(uint8_t* rdram, recomp_context* ctx)", 1)
    traversal = "func_8008AEA0(rdram, ctx);"
    if copy.count(traversal) != 1:
        raise RuntimeError("expected exactly one func_8008AEA0 call in base renderer")
    copy = copy.replace(traversal, traversal + "\n    return; /* v19 capture-only: disposable context */", 1)
    for hook in (
        "rocket_presentation_task_submitted(rdram, ctx);",
        "rocket_presentation_frame_submitted(rdram, ctx);",
    ):
        copy = copy.replace(hook, "/* v19 capture copy suppresses presentation submission */")
    return copy


def build_draw_copy(base: str):
    marker = "RECOMP_FUNC void func_8008B694(uint8_t* rdram, recomp_context* ctx)"
    copy = base.replace(marker,
        "RECOMP_FUNC void rocket_draw_func_8008B694(uint8_t* rdram, recomp_context* ctx)", 1)
    if copy.count("func_80092050(rdram, ctx);") != 1:
        raise RuntimeError("expected exactly one func_80092050 call in base renderer")
    if copy.count("func_8008AEA0(rdram, ctx);") != 1:
        raise RuntimeError("expected exactly one func_8008AEA0 call in base renderer")
    copy = copy.replace("func_80092050(rdram, ctx);",
                        "/* v19 draw copy: frame init already ran during capture */", 1)
    copy = copy.replace("func_8008AEA0(rdram, ctx);",
                        "/* v19 draw copy: scene traversal already captured globally */", 1)
    for hook in (
        "rocket_presentation_task_submitted(rdram, ctx);",
        "rocket_presentation_frame_submitted(rdram, ctx);",
    ):
        copy = copy.replace(hook,
            f"if (rocket_render_queue_final_pass()) {{ {hook} }}")
    return copy


def patch_renderer(path: Path):
    text, nl, bom = read_text(path)
    if MARKER in text:
        return False
    text = unwrap_v18_renderer(text)
    marker = "RECOMP_FUNC void func_8008B694(uint8_t* rdram, recomp_context* ctx)"
    s, e = function_span(text, marker)
    base = text[s:e]
    if base.count("func_80092050(rdram, ctx);") != 1:
        raise RuntimeError(f"{path.name}: renderer init call count is not one")
    if base.count("func_8008AEA0(rdram, ctx);") != 1:
        raise RuntimeError(f"{path.name}: scene traversal call count is not one")
    replacement = DECLS + "\n" + build_capture_copy(base) + "\n\n" + build_draw_copy(base) + WRAPPER
    text = text[:s] + replacement + text[e:]
    write_text(path, text, nl, bom)
    return True


def patch_add_render_entry(path: Path):
    text, nl, bom = read_text(path)
    call = "rocket_render_queue_capture(rdram, ctx);"
    marker = "RECOMP_FUNC void add_render_entry(uint8_t* rdram, recomp_context* ctx)"
    s, e = function_span(text, marker)
    body = text[s:e]
    if call not in body:
        brace = text.find("{", s)
        if brace < 0 or brace >= e: raise RuntimeError("generated add_render_entry brace missing")
        text = text[:brace + 1] + "\n    " + call + text[brace + 1:]
    decl = "extern void rocket_render_queue_capture(uint8_t* rdram, recomp_context* ctx);"
    if decl not in text[:s + 200]:
        # Keep a declaration local to the generated translation unit.
        s2, _ = function_span(text, marker)
        text = text[:s2] + decl + "\n" + text[s2:]
    write_text(path, text, nl, bom)
    return call not in body


def find_function_source(root: Path, function_name: str):
    out = root / "runtime-recomp" / "RecompiledFuncs"
    if not out.is_dir(): raise RuntimeError(f"generated CPU directory missing: {out}")
    markers = [f"RECOMP_FUNC void {function_name}(uint8_t* rdram, recomp_context* ctx)"]
    if function_name == "func_8008B694":
        markers += [MARKER, V18_MARKER]
    matches = []
    for path in sorted(out.glob("*.c")):
        try: text = path.read_text(encoding="utf-8-sig")
        except UnicodeDecodeError: continue
        if any(m in text for m in markers): matches.append(path)
    if len(matches) != 1:
        raise RuntimeError(f"expected exactly one generated {function_name} source, found {len(matches)}")
    return matches[0]


def verify_renderer(path: Path):
    text = path.read_text(encoding="utf-8-sig")
    for token in (
        MARKER,
        "RECOMP_FUNC void rocket_capture_func_8008B694",
        "RECOMP_FUNC void rocket_draw_func_8008B694",
        "RECOMP_FUNC void func_8008B694",
        "rocket_render_queue_finalize_capture(rdram, ctx);",
        "rocket_render_queue_begin_batch(rdram, ctx)",
        "rocket_render_queue_final_pass()",
        "rocket_render_queue_prepare_empty_final(rdram, ctx);",
    ):
        if token not in text: raise RuntimeError("generated v19 renderer token missing: " + token)
    if V18_MARKER in text: raise RuntimeError("retired v18 renderer wrapper remains")
    if text.count("RECOMP_FUNC void func_8008B694(") != 1:
        raise RuntimeError("v19 wrapper func count wrong")
    if text.count("RECOMP_FUNC void rocket_capture_func_8008B694(") != 1:
        raise RuntimeError("v19 capture func count wrong")
    if text.count("RECOMP_FUNC void rocket_draw_func_8008B694(") != 1:
        raise RuntimeError("v19 draw func count wrong")


def verify_capture(path: Path):
    text = path.read_text(encoding="utf-8-sig")
    marker = "RECOMP_FUNC void add_render_entry(uint8_t* rdram, recomp_context* ctx)"
    s, e = function_span(text, marker)
    body = text[s:e]
    if body.count("rocket_render_queue_capture(rdram, ctx);") != 1:
        raise RuntimeError("add_render_entry direct capture missing/duplicated")
    if "// 0x" in body and body.find("rocket_render_queue_capture(rdram, ctx);") > body.find("// 0x"):
        raise RuntimeError("capture must precede retail add_render_entry body")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--verify", action="store_true")
    args = ap.parse_args()
    root = Path(args.root).resolve()
    renderer = find_function_source(root, "func_8008B694")
    addsrc = find_function_source(root, "add_render_entry")
    if not args.verify:
        a = patch_renderer(renderer)
        b = patch_add_render_entry(addsrc)
        print(f"[OK] {'Patched' if a else 'Already patched'} v19 generated renderer: {renderer}")
        print(f"[OK] {'Patched' if b else 'Already patched'} direct add_render_entry capture: {addsrc}")
    verify_renderer(renderer)
    verify_capture(addsrc)
    print("[OK] Generated render queue v19 capture-first/global-order wrapper PASS.")

if __name__ == "__main__":
    main()
