#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import argparse

ADD_MARKER = 'RECOMP_FUNC void add_render_entry(uint8_t* rdram, recomp_context* ctx)'
MODEL_MARKER = 'RECOMP_FUNC void func_8001E954(uint8_t* rdram, recomp_context* ctx)'
RENDER_MARKER = 'RECOMP_FUNC void func_8008B694(uint8_t* rdram, recomp_context* ctx)'
V31_MARKER = 'ROCKET-R GRAPHICS V31 NO-DROP RETAIL-WORKING-QUEUE'
STAGE_BASE = '0X8040'
SORT_BASE = '0X8058'


def function_span(text: str, marker: str):
    start = text.find(marker)
    if start < 0:
        raise RuntimeError(f'Function marker not found: {marker}')
    brace = text.find('{', start)
    if brace < 0:
        raise RuntimeError(f'Opening brace not found: {marker}')
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
    raise RuntimeError(f'Unterminated function: {marker}')


def generated_dir(root: Path) -> Path:
    out = root / 'runtime-recomp' / 'RecompiledFuncs'
    if not out.is_dir():
        raise RuntimeError(f'Generated CPU directory missing: {out}')
    return out


def find_source(root: Path, marker: str) -> Path:
    matches = []
    for path in sorted(generated_dir(root).glob('*.c')):
        try:
            text = path.read_text(encoding='utf-8-sig')
        except UnicodeDecodeError:
            continue
        if marker in text:
            matches.append(path)
    if len(matches) != 1:
        raise RuntimeError(f'Expected exactly one generated source for {marker}, found {len(matches)}')
    return matches[0]


def read_preserved(path: Path):
    raw = path.read_bytes()
    bom = raw.startswith(b'\xef\xbb\xbf')
    text = raw.decode('utf-8-sig')
    nl = '\r\n' if '\r\n' in text else '\n'
    return text.replace('\r\n', '\n'), bom, nl


def write_preserved(path: Path, text: str, bom: bool, nl: str):
    text = text.replace('\r\n', '\n')
    if nl == '\r\n':
        text = text.replace('\n', '\r\n')
    data = text.encode('utf-8')
    if bom:
        data = b'\xef\xbb\xbf' + data
    path.write_bytes(data)


def replace_once(body: str, old: str, new: str, desc: str) -> str:
    n = body.count(old)
    if n != 1:
        raise RuntimeError(f'{desc}: expected exactly one retail pattern, found {n}')
    return body.replace(old, new, 1)


def add_global_decl(text: str, marker: str, decl: str) -> str:
    if decl in text:
        return text
    pos = text.find(marker)
    if pos < 0:
        raise RuntimeError(f'Cannot add declaration; marker missing: {marker}')
    return text[:pos] + decl + '\n' + text[pos:]


def reject_old_capacity_patches(text: str):
    bad_tokens = (
        'ROCKET-R GRAPHICS V30 EXPANDED NATIVE RENDER LIST',
        'ROCKET_QUEUE_V21_PROCESS_BATCH',
        'rocket_render_queue_v28_',
        'rocket_render_queue_prepare_first_batch',
    )
    bad = [x for x in bad_tokens if x in text]
    if bad:
        raise RuntimeError(
            'Refusing to stack v31 on an older capacity experiment: ' + ', '.join(bad))


def patch_add(path: Path) -> bool:
    text, bom, nl = read_preserved(path)
    reject_old_capacity_patches(text)
    decl = 'extern void rocket_render_capacity_v31_add_guard(uint8_t*, recomp_context*);'
    text = add_global_decl(text, ADD_MARKER, decl)
    s, e = function_span(text, ADD_MARKER)
    body = text[s:e]
    if V31_MARKER in body:
        return False
    anchor = '    int c1cs = 0;\n'
    if anchor not in body:
        raise RuntimeError('add_render_entry declaration anchor missing')
    body = body.replace(anchor, anchor +
        f'    // {V31_MARKER}: recycle only finalized retail scratch outside model scope.\n'
        '    rocket_render_capacity_v31_add_guard(rdram, ctx);\n', 1)
    text = text[:s] + body + text[e:]
    write_preserved(path, text, bom, nl)
    return True


def patch_model(path: Path) -> bool:
    text, bom, nl = read_preserved(path)
    reject_old_capacity_patches(text)
    d1 = 'extern void rocket_render_capacity_v31_model_begin(uint8_t*, recomp_context*);'
    d2 = 'extern void rocket_render_capacity_v31_model_end(uint8_t*, recomp_context*);'
    text = add_global_decl(text, MODEL_MARKER, d1)
    text = add_global_decl(text, MODEL_MARKER, d2)
    s, e = function_span(text, MODEL_MARKER)
    body = text[s:e]
    if V31_MARKER in body:
        return False
    anchor = '    int c1cs = 0;\n'
    if anchor not in body:
        raise RuntimeError('func_8001E954 declaration anchor missing')
    body = body.replace(anchor, anchor +
        f'    // {V31_MARKER}: outer model owns the retail scratch range until post-processing completes.\n'
        '    rocket_render_capacity_v31_model_begin(rdram, ctx);\n', 1)
    epilogue = 'L_8001E9FC:\n'
    if epilogue not in body:
        raise RuntimeError('func_8001E954 common epilogue label missing')
    body = body.replace(epilogue, epilogue +
        '    // v31: func_80020134 has completed; entries are now safe to finalize.\n'
        '    rocket_render_capacity_v31_model_end(rdram, ctx);\n', 1)
    text = text[:s] + body + text[e:]
    write_preserved(path, text, bom, nl)
    return True


def patch_renderer(path: Path) -> bool:
    text, bom, nl = read_preserved(path)
    reject_old_capacity_patches(text)
    d1 = 'extern void rocket_render_capacity_v31_frame_begin(uint8_t*, recomp_context*);'
    d2 = 'extern void rocket_render_capacity_v31_prepare(uint8_t*, recomp_context*);'
    text = add_global_decl(text, RENDER_MARKER, d1)
    text = add_global_decl(text, RENDER_MARKER, d2)
    s, e = function_span(text, RENDER_MARKER)
    body = text[s:e]
    if V31_MARKER in body:
        verify_renderer_body(body)
        return False

    # Start a fresh host-side finalized list immediately before Rocket performs
    # its one and only scene traversal.  The retail guest queue itself remains
    # untouched here and is still initialized/used by Rocket normally.
    anchor = '''    after_0:
    // 0x8008B7E0: lw          $a1, 0x494($sp)'''
    repl = f'''    after_0:
    // {V31_MARKER}: preserve Rocket's retail working queue during traversal.
    rocket_render_capacity_v31_frame_begin(rdram, ctx);
    // 0x8008B7E0: lw          $a1, 0x494($sp)'''
    body = replace_once(body, anchor, repl, 'renderer frame-begin hook')

    # After traversal, finalize remaining retail scratch entries into the host
    # list and stage EVERY finalized entry in extended memory.  Model-side code
    # never sees these staged addresses.
    old_count = '''    after_1:
    // 0x8008B7EC: lui         $v0, 0x800B
    ctx->r2 = S32(0X800B << 16);
    // 0x8008B7F0: lui         $a0, 0x800B
    ctx->r4 = S32(0X800B << 16);
    // 0x8008B7F4: lw          $v1, -0xD00($v0)
    ctx->r3 = MEM_W(ctx->r2, -0XD00);
    // 0x8008B7F8: addiu       $a0, $a0, -0x2500
    ctx->r4 = ADD32(ctx->r4, -0X2500);
    // 0x8008B7FC: subu        $v1, $v1, $a0
    ctx->r3 = SUB32(ctx->r3, ctx->r4);
    // 0x8008B800: sll         $v0, $v1, 2
    ctx->r2 = S32(ctx->r3 << 2);
    // 0x8008B804: addu        $v0, $v0, $v1
    ctx->r2 = ADD32(ctx->r2, ctx->r3);
    // 0x8008B808: sll         $v1, $v0, 4
    ctx->r3 = S32(ctx->r2 << 4);
    // 0x8008B80C: addu        $v0, $v0, $v1
    ctx->r2 = ADD32(ctx->r2, ctx->r3);
    // 0x8008B810: sll         $v1, $v0, 8
    ctx->r3 = S32(ctx->r2 << 8);
    // 0x8008B814: addu        $v0, $v0, $v1
    ctx->r2 = ADD32(ctx->r2, ctx->r3);
    // 0x8008B818: sll         $v1, $v0, 16
    ctx->r3 = S32(ctx->r2 << 16);
    // 0x8008B81C: addu        $v0, $v0, $v1
    ctx->r2 = ADD32(ctx->r2, ctx->r3);
    // 0x8008B820: negu        $v0, $v0
    ctx->r2 = SUB32(0, ctx->r2);
    // 0x8008B824: sra         $s3, $v0, 3
    ctx->r19 = S32(SIGNED(ctx->r2) >> 3);'''
    new_count = '''    after_1:
    // v31: finalize ALL scene entries after retail-compatible model processing.
    rocket_render_capacity_v31_prepare(rdram, ctx);
    ctx->r19 = ctx->r2; // total finalized RenderEntry count, no 256-entry ceiling
    // Final renderer only: staged RenderEntries begin at 0x80400000.
    ctx->r4 = S32(0X8040 << 16);'''
    body = replace_once(body, old_count, new_count, 'renderer finalized-count handoff')

    # The original local sortedEntryList[256] lived on the stack.  Redirect that
    # pointer list only (not model queue state) to a 65536-pointer extended arena.
    body = replace_once(body,
'''    // 0x8008B82C: addiu       $a3, $sp, 0x40
    ctx->r7 = ADD32(ctx->r29, 0X40);''',
'''    // 0x8008B82C: v31 expanded final sort-pointer arena (0x80580000)
    ctx->r7 = S32(0X8058 << 16);''', 'first sort base')
    body = replace_once(body,
'''    // 0x8008B830: addiu       $v1, $sp, 0x20
    ctx->r3 = ADD32(ctx->r29, 0X20);''',
'''    // 0x8008B830: v31 pointer-list write cursor
    ctx->r3 = S32(0X8058 << 16);''', 'sort cursor')
    body = replace_once(body,
'''    // 0x8008B834: sw          $a0, 0x20($v1)
    MEM_W(0X20, ctx->r3) = ctx->r4;''',
'''    // 0x8008B834: v31 direct pointer-list store
    MEM_W(0X0, ctx->r3) = ctx->r4;''', 'sort pointer store')
    body = replace_once(body,
'''    // 0x8008B850: addiu       $a3, $sp, 0x40
    ctx->r7 = ADD32(ctx->r29, 0X40);''',
'''    // 0x8008B850: v31 expanded final sort-pointer arena (0x80580000)
    ctx->r7 = S32(0X8058 << 16);''', 'second sort base')
    body = replace_once(body,
'''    // 0x8008B92C: addiu       $s0, $sp, 0x40
    ctx->r16 = ADD32(ctx->r29, 0X40);''',
'''    // 0x8008B92C: v31 original heapsort operates on expanded pointer arena
    ctx->r16 = S32(0X8058 << 16);''', 'heapsort base')
    body = replace_once(body,
'''    // 0x8008B988: addu        $v0, $sp, $v0
    ctx->r2 = ADD32(ctx->r29, ctx->r2);
    // 0x8008B98C: lw          $s0, 0x40($v0)
    ctx->r16 = MEM_W(ctx->r2, 0X40);''',
'''    // 0x8008B988/B98C: v31 opaque pointer from expanded sort arena
    ctx->r2 = ADD32(S32(0X8058 << 16), ctx->r2);
    ctx->r16 = MEM_W(ctx->r2, 0X0);''', 'opaque draw pointer')
    body = replace_once(body,
'''    // 0x8008B9A4: addu        $v0, $sp, $v0
    ctx->r2 = ADD32(ctx->r29, ctx->r2);
    // 0x8008B9A8: lw          $s0, 0x3C($v0)
    ctx->r16 = MEM_W(ctx->r2, 0X3C);''',
'''    // 0x8008B9A4/B9A8: v31 transparent pointer from expanded sort arena
    ctx->r2 = ADD32(S32(0X8058 << 16), ctx->r2);
    ctx->r16 = MEM_W(ctx->r2, -0X4);''', 'transparent draw pointer')

    text = text[:s] + body + text[e:]
    write_preserved(path, text, bom, nl)
    return True


def verify_renderer_body(body: str):
    required = (
        V31_MARKER,
        'rocket_render_capacity_v31_frame_begin(rdram, ctx);',
        'rocket_render_capacity_v31_prepare(rdram, ctx);',
        'ctx->r19 = ctx->r2;',
        'S32(0X8040 << 16)',
        'S32(0X8058 << 16)',
    )
    missing = [x for x in required if x not in body]
    if missing:
        raise RuntimeError('v31 renderer verification failed: ' + ', '.join(missing))
    if body.count('func_8008AEA0(rdram, ctx);') != 1:
        raise RuntimeError('v31 must retain exactly one Rocket scene traversal')
    if body.count('func_8008B4BC(rdram, ctx);') != 2:
        raise RuntimeError('v31 must retain Rocket\'s two original heap-sort calls')
    if body.count('func_80092050(rdram, ctx);') != 1:
        raise RuntimeError('v31 must retain Rocket\'s original material setup')


def verify(root: Path):
    add = find_source(root, ADD_MARKER)
    model = find_source(root, MODEL_MARKER)
    renderer = find_source(root, RENDER_MARKER)

    at = add.read_text(encoding='utf-8-sig')
    mt = model.read_text(encoding='utf-8-sig')
    rt = renderer.read_text(encoding='utf-8-sig')

    sa, ea = function_span(at, ADD_MARKER)
    sm, em = function_span(mt, MODEL_MARKER)
    sr, er = function_span(rt, RENDER_MARKER)
    ab = at[sa:ea]
    mb = mt[sm:em]
    rb = rt[sr:er]

    if 'rocket_render_capacity_v31_add_guard(rdram, ctx);' not in ab:
        raise RuntimeError('v31 add guard missing')
    if mb.count('rocket_render_capacity_v31_model_begin(rdram, ctx);') != 1:
        raise RuntimeError('v31 model-begin hook missing or duplicated')
    if mb.count('rocket_render_capacity_v31_model_end(rdram, ctx);') != 1:
        raise RuntimeError('v31 model-end hook missing or duplicated')
    verify_renderer_body(rb)

    # Crucial invariant: v31 does NOT relocate D_800AF300 or D_800ADB00 during
    # scene/model traversal.  The retail 256-entry region remains the exact
    # guest working queue expected by func_80020134 and other hidden consumers.
    if 'MEM_W(-0XD00, ctx->r2) = S32(0X8040 << 16)' in rb:
        raise RuntimeError('v31 must not expose an extended queue pointer to model code')

    return add, model, renderer


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    ap.add_argument('--verify', action='store_true')
    args = ap.parse_args()
    root = Path(args.root).resolve()

    add = find_source(root, ADD_MARKER)
    model = find_source(root, MODEL_MARKER)
    renderer = find_source(root, RENDER_MARKER)

    if not args.verify:
        ca = patch_add(add)
        cm = patch_model(model)
        cr = patch_renderer(renderer)
        print(f"[OK] {'Patched' if ca else 'Verified'} v31 retail add guard: {add}")
        print(f"[OK] {'Patched' if cm else 'Verified'} v31 model finalization boundary: {model}")
        print(f"[OK] {'Patched' if cr else 'Verified'} v31 full-scene final renderer: {renderer}")

    verify(root)
    print('[OK] v31 generated verification PASS: retail model queue preserved; no-drop finalized scene; one traversal; original partition/heapsort/draw loop retained.')


if __name__ == '__main__':
    main()
