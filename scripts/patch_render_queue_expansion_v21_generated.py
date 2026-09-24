#!/usr/bin/env python3
from pathlib import Path
import argparse

ADD_MARKER = 'RECOMP_FUNC void add_render_entry(uint8_t* rdram, recomp_context* ctx)'
B694_MARKER = 'RECOMP_FUNC void func_8008B694(uint8_t* rdram, recomp_context* ctx)'
CAPTURE_DECL = 'extern void rocket_render_queue_capture_entry(uint8_t* rdram, recomp_context* ctx);\n'
CAPTURE_CALL = 'rocket_render_queue_capture_entry(rdram, ctx);'
B694_DECLS = '''extern void rocket_render_queue_capture_begin(uint8_t* rdram, recomp_context* ctx);\nextern void rocket_render_queue_prepare_first_batch(uint8_t* rdram, recomp_context* ctx);\nextern int rocket_render_queue_batch_preordered(void);\nextern int rocket_render_queue_is_continuation(void);\nextern int rocket_render_queue_prepare_next_batch(uint8_t* rdram, recomp_context* ctx);\n'''
FIRST_BEGIN_CALL = 'rocket_render_queue_capture_begin(rdram, ctx);'
FIRST_STAGE_CALL = 'rocket_render_queue_prepare_first_batch(rdram, ctx);'
PROCESS_LABEL = 'ROCKET_QUEUE_V21_PROCESS_BATCH:'
PREORDER_CHECK = 'rocket_render_queue_batch_preordered()'
CONT_CHECK = 'rocket_render_queue_is_continuation()'
NEXT_CALL = 'rocket_render_queue_prepare_next_batch(rdram, ctx)'


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
            if c == '/' and n == '/':
                state = 'line'; i += 2; continue
            if c == '/' and n == '*':
                state = 'block'; i += 2; continue
            if c == '"': state = 'string'
            elif c == "'": state = 'char'
            elif c == '{': depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    return start, i + 1
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


def find_source(root: Path, marker: str) -> Path:
    out = root / 'runtime-recomp' / 'RecompiledFuncs'
    if not out.is_dir():
        raise RuntimeError(f'Generated CPU directory missing: {out}')
    matches = []
    for path in sorted(out.glob('*.c')):
        try:
            text = path.read_text(encoding='utf-8-sig')
        except UnicodeDecodeError:
            continue
        if marker in text:
            matches.append(path)
    if len(matches) != 1:
        raise RuntimeError(f'Expected exactly one generated source for {marker}, found {len(matches)}')
    return matches[0]


def preserve_write(path: Path, text: str, bom: bool, nl: str):
    text = text.replace('\r\n', '\n')
    if nl == '\r\n':
        text = text.replace('\n', '\r\n')
    data = text.encode('utf-8')
    if bom:
        data = b'\xef\xbb\xbf' + data
    path.write_bytes(data)


def patch_add(path: Path) -> bool:
    raw = path.read_bytes()
    bom = raw.startswith(b'\xef\xbb\xbf')
    text = raw.decode('utf-8-sig')
    nl = '\r\n' if '\r\n' in text else '\n'
    text = text.replace('\r\n', '\n')
    s, e = function_span(text, ADD_MARKER)
    body = text[s:e]

    forbidden = (
        'rocket_render_queue_capture(', 'rocket_original_func_8008B694',
        'rocket_render_queue_begin_batch(', 'RocketBuildGlobalRenderOrder',
        'rocket_capture_func_8008B694', 'rocket_draw_func_8008B694',
    )
    bad = [tok for tok in forbidden if tok in text]
    if bad:
        raise RuntimeError('Refusing to patch retired v18/v19 renderer tokens: ' + ', '.join(bad))

    changed = False
    if CAPTURE_CALL not in body:
        brace = text.find('{', s)
        if brace < 0 or brace >= e:
            raise RuntimeError('add_render_entry opening brace missing')
        if CAPTURE_DECL not in text[max(0, s - 600):s]:
            text = text[:s] + CAPTURE_DECL + text[s:]
            shift = len(CAPTURE_DECL)
            s += shift; e += shift; brace += shift
        # Keep the reliable v20 telemetry first when it exists, then capture the raw call.
        s, e = function_span(text, ADD_MARKER)
        body = text[s:e]
        anchor = 'rocket_popdiag_add_render_entry_attempt(rdram, ctx);'
        if anchor in body:
            pos = s + body.find(anchor) + len(anchor)
            text = text[:pos] + '\n    ' + CAPTURE_CALL + text[pos:]
        else:
            brace = text.find('{', s)
            text = text[:brace + 1] + '\n    ' + CAPTURE_CALL + text[brace + 1:]
        changed = True

    if text.count(CAPTURE_CALL) != 1:
        raise RuntimeError('v21 add_render_entry capture hook missing or duplicated')
    s, e = function_span(text, ADD_MARKER)
    body = text[s:e]
    first_retail = body.find('// 0x8008B24C:')
    if first_retail >= 0 and body.find(CAPTURE_CALL) > first_retail:
        raise RuntimeError('v21 capture hook must run before the retail add_render_entry body')

    if changed:
        preserve_write(path, text, bom, nl)
    return changed


def patch_b694(path: Path) -> bool:
    raw = path.read_bytes()
    bom = raw.startswith(b'\xef\xbb\xbf')
    text = raw.decode('utf-8-sig')
    nl = '\r\n' if '\r\n' in text else '\n'
    text = text.replace('\r\n', '\n')

    forbidden = (
        'rocket_original_func_8008B694', 'rocket_capture_func_8008B694',
        'rocket_draw_func_8008B694', 'rocket_render_queue_begin_batch(',
        'ROCKET-R GRAPHICS V19 GLOBAL RENDER QUEUE',
    )
    bad = [tok for tok in forbidden if tok in text]
    if bad:
        raise RuntimeError('Refusing to patch retired v18/v19 renderer tokens: ' + ', '.join(bad))

    changed = False
    s, e = function_span(text, B694_MARKER)
    body = text[s:e]

    if B694_DECLS not in text[max(0, s - 1200):s]:
        text = text[:s] + B694_DECLS + text[s:]
        changed = True

    # Refresh span after declaration insertion.
    s, e = function_span(text, B694_MARKER)
    body = text[s:e]

    if FIRST_BEGIN_CALL not in body:
        anchor = '    after_0:\n    // 0x8008B7E0: lw          $a1, 0x494($sp)'
        if anchor not in body:
            raise RuntimeError('Could not locate func_8008B694 traversal-begin anchor')
        repl = '    after_0:\n    ' + FIRST_BEGIN_CALL + '\n    // 0x8008B7E0: lw          $a1, 0x494($sp)'
        body = body.replace(anchor, repl, 1)
        changed = True

    if FIRST_STAGE_CALL not in body:
        anchor = '    after_1:\n    // 0x8008B7EC: lui         $v0, 0x800B'
        if anchor not in body:
            raise RuntimeError('Could not locate func_8008B694 post-traversal anchor')
        repl = ('    after_1:\n    ' + FIRST_STAGE_CALL + '\n' + PROCESS_LABEL +
                '\n    // 0x8008B7EC: lui         $v0, 0x800B')
        body = body.replace(anchor, repl, 1)
        changed = True
    elif PROCESS_LABEL not in body:
        raise RuntimeError('v21 first-batch staging exists without process label')

    if PREORDER_CHECK not in body:
        anchor = ('    // 0x8008B84C: blez        $s3, L_8008B950\n'
                  '    if (SIGNED(ctx->r19) <= 0) {')
        if anchor not in body:
            raise RuntimeError('Could not locate func_8008B694 post-pointer-fill sort anchor')
        injected = (
            '    // v21: host has already reproduced Rocket\'s global partition/heap-sort order.\n'
            '    // The guest pointer array is still used, but sorting is skipped only for\n'
            '    // staged overflow-recovery batches so exact global order survives chunking.\n'
            '    if (' + PREORDER_CHECK + ') {\n'
            '        MEM_W(0X440, ctx->r29) = ctx->r19;\n'
            '        goto L_8008B950;\n'
            '    }\n'
        )
        body = body.replace(anchor, injected + anchor, 1)
        changed = True

    if CONT_CHECK not in body:
        anchor = ('L_8008B950:\n'
                  '    // 0x8008B950: addu        $s4, $zero, $zero\n'
                  '    ctx->r20 = ADD32(0, 0);')
        if anchor not in body:
            raise RuntimeError('Could not locate func_8008B694 matrix-state reset anchor')
        repl = (
            'L_8008B950:\n'
            '    // Preserve the current model-matrix cache across v21 continuation batches.\n'
            '    // Retail/non-expanded frames still execute the original s4=0; s2=s4 sequence.\n'
            '    if (!' + CONT_CHECK + ') {\n'
            '        ctx->r20 = ADD32(0, 0);\n'
            '    }\n'
            '    // s2 is the per-batch draw index. It is always zero at the start of a batch;\n'
            '    // on the retail path this is exactly equivalent to addu s2,s4 after s4=0.\n'
            '    ctx->r18 = ADD32(0, 0);\n'
            '    // 0x8008B950: addu        $s4, $zero, $zero'
        )
        body = body.replace(anchor, repl, 1)
        # Remove the original s2=s4 assignment immediately following the preserved comment.
        body = body.replace(
            '    // 0x8008B950: addu        $s4, $zero, $zero\n'
            '    // 0x8008B954: addu        $s2, $s4, $zero\n'
            '    ctx->r18 = ADD32(ctx->r20, 0);',
            '    // 0x8008B950: addu        $s4, $zero, $zero\n'
            '    // 0x8008B954: addu        $s2, $s4, $zero',
            1)
        changed = True

    if NEXT_CALL not in body:
        anchor = 'L_8008BC08:\n    // 0x8008BC08: lw          $t3, 0x494($sp)'
        if anchor not in body:
            raise RuntimeError('Could not locate func_8008B694 pre-tail anchor')
        repl = (
            'L_8008BC08:\n'
            '    // v21: draw every recovered entry inside this same retail renderer invocation.\n'
            '    // Traversal/material setup and the retail tail execute only once.\n'
            '    if (' + NEXT_CALL + ') {\n'
            '        ctx->r18 = 0;\n'
            '        goto ' + PROCESS_LABEL[:-1] + ';\n'
            '    }\n'
            '    // 0x8008BC08: lw          $t3, 0x494($sp)'
        )
        body = body.replace(anchor, repl, 1)
        changed = True

    # Replace body in full text after all edits.
    s2, e2 = function_span(text, B694_MARKER)
    text = text[:s2] + body + text[e2:]

    # Structural verification.
    s, e = function_span(text, B694_MARKER)
    body = text[s:e]
    required = (FIRST_BEGIN_CALL, FIRST_STAGE_CALL, PROCESS_LABEL, PREORDER_CHECK, CONT_CHECK, NEXT_CALL)
    missing = [tok for tok in required if body.count(tok) != 1]
    if missing:
        raise RuntimeError('v21 generated renderer verification failed: ' + ', '.join(missing))
    if body.count('func_8008AEA0(rdram, ctx);') != 1:
        raise RuntimeError('v21 must retain exactly one scene traversal call')
    if body.count('func_80092050(rdram, ctx);') != 1:
        raise RuntimeError('v21 must retain exactly one material-update call')
    if body.count('draw_rectangle(rdram, ctx);') > 1 or body.count('func_80091F54(rdram, ctx);') > 1:
        raise RuntimeError('v21 must not duplicate the retail tail')

    if changed:
        preserve_write(path, text, bom, nl)
    return changed


def verify(root: Path):
    add = find_source(root, ADD_MARKER)
    b694 = find_source(root, B694_MARKER)
    add_text = add.read_text(encoding='utf-8-sig')
    b_text = b694.read_text(encoding='utf-8-sig')
    if add_text.count(CAPTURE_CALL) != 1:
        raise RuntimeError('add_render_entry v21 hook verification failed')
    s, e = function_span(b_text, B694_MARKER)
    body = b_text[s:e]
    for tok in (FIRST_BEGIN_CALL, FIRST_STAGE_CALL, PROCESS_LABEL, PREORDER_CHECK, CONT_CHECK, NEXT_CALL):
        if body.count(tok) != 1:
            raise RuntimeError(f'func_8008B694 v21 marker missing/duplicated: {tok}')
    if body.count('func_8008AEA0(rdram, ctx);') != 1:
        raise RuntimeError('func_8008B694 scene traversal count changed')
    return add, b694


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    ap.add_argument('--verify', action='store_true')
    args = ap.parse_args()
    root = Path(args.root).resolve()
    add = find_source(root, ADD_MARKER)
    b694 = find_source(root, B694_MARKER)
    if not args.verify:
        ca = patch_add(add)
        cb = patch_b694(b694)
        print(f"[OK] {'Patched' if ca else 'Verified'} v21 capture hook: {add}")
        print(f"[OK] {'Patched' if cb else 'Verified'} v21 in-function queue batches: {b694}")
    add, b694 = verify(root)
    print('[OK] v21 generated renderer verification PASS: one traversal, one material update, retail draw/tail retained.')

if __name__ == '__main__':
    main()
