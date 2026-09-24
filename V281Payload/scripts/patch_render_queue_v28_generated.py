#!/usr/bin/env python3
from pathlib import Path
import argparse
import sys

ADD_MARKER = 'RECOMP_FUNC void add_render_entry(uint8_t* rdram, recomp_context* ctx)'
B694_MARKER = 'RECOMP_FUNC void func_8008B694(uint8_t* rdram, recomp_context* ctx)'

ADD_DECL = 'extern int rocket_render_queue_v28_add(uint8_t* rdram, recomp_context* ctx);\n'
ADD_CALL = 'if (rocket_render_queue_v28_add(rdram, ctx)) return;'

B694_DECLS = '''extern void rocket_render_queue_v28_begin(uint8_t* rdram, recomp_context* ctx);\nextern int rocket_render_queue_v28_prepare(uint8_t* rdram, recomp_context* ctx);\nextern uint32_t rocket_render_queue_v28_entry_address(uint8_t* rdram, int index);\nextern void rocket_render_queue_v28_end(uint8_t* rdram, recomp_context* ctx);\n'''
BEGIN_CALL = 'rocket_render_queue_v28_begin(rdram, ctx);'
PREPARE_MARKER = '// === ROCKET-R GRAPHICS V28 PREPARE UNBOUNDED QUEUE ==='
SELECT_MARKER = '// === ROCKET-R GRAPHICS V28 SELECT UNBOUNDED ENTRY ==='
END_CALL = 'rocket_render_queue_v28_end(rdram, ctx);'

RETIRED_TOKENS = (
    'ROCKET_QUEUE_V21_PROCESS_BATCH',
    'rocket_render_queue_capture_begin(',
    'rocket_render_queue_prepare_first_batch(',
    'rocket_render_queue_prepare_next_batch(',
    'ROCKET-R GRAPHICS V19 GLOBAL RENDER QUEUE',
    'rocket_original_func_8008B694',
    'rocket_capture_func_8008B694',
    'rocket_draw_func_8008B694',
)


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
    hits = []
    for path in sorted(out.glob('*.c')):
        try:
            text = path.read_text(encoding='utf-8-sig')
        except UnicodeDecodeError:
            continue
        if marker in text:
            hits.append(path)
    if len(hits) != 1:
        raise RuntimeError(f'Expected exactly one generated source for {marker}, found {len(hits)}')
    return hits[0]


def read_text(path: Path):
    raw = path.read_bytes()
    bom = raw.startswith(b'\xef\xbb\xbf')
    text = raw.decode('utf-8-sig').replace('\r\n', '\n')
    nl = '\r\n' if b'\r\n' in raw else '\n'
    return text, bom, nl


def write_text(path: Path, text: str, bom: bool, nl: str):
    text = text.replace('\r\n', '\n')
    if nl == '\r\n':
        text = text.replace('\n', '\r\n')
    data = text.encode('utf-8')
    if bom:
        data = b'\xef\xbb\xbf' + data
    path.write_bytes(data)


def ensure_no_retired_tokens(text: str, label: str):
    bad = [token for token in RETIRED_TOKENS if token in text]
    if bad:
        raise RuntimeError(
            f'{label} still contains retired renderer patch tokens: ' + ', '.join(bad) +
            '. OneClick must regenerate clean CPU source before v28 is applied.')


def patch_add(path: Path) -> bool:
    text, bom, nl = read_text(path)
    s, e = function_span(text, ADD_MARKER)
    body = text[s:e]
    ensure_no_retired_tokens(body, 'add_render_entry')

    if ADD_CALL in body:
        return False

    # The semantic interpolation hook must run first.  V28 then captures the
    # exact same raw add_render_entry arguments and returns before the retail
    # 256-slot storage body.  Calls outside the scene renderer fall back to the
    # retail body because rocket_render_queue_v28_add returns 0 when inactive.
    owner_hook = 'rocket_presentation_render_entry(rdram, ctx);'
    if owner_hook not in body:
        raise RuntimeError(
            'Stable presentation hook missing from generated add_render_entry; '
            'refusing to install v28 without interpolation capture.')

    if ADD_DECL not in text[max(0, s - 1000):s]:
        text = text[:s] + ADD_DECL + text[s:]
        s, e = function_span(text, ADD_MARKER)
        body = text[s:e]

    pos = body.find(owner_hook) + len(owner_hook)
    body = body[:pos] + '\n    // === ROCKET-R GRAPHICS V28 UNBOUNDED ADD ===\n    ' + ADD_CALL + body[pos:]
    text = text[:s] + body + text[e:]
    write_text(path, text, bom, nl)
    return True


def patch_b694(path: Path) -> bool:
    text, bom, nl = read_text(path)
    s, e = function_span(text, B694_MARKER)
    body = text[s:e]
    ensure_no_retired_tokens(body, 'func_8008B694')

    # v28.1 compile hotfix: upgrade an already-patched v28 selector ABI that
    # omitted the rdram pointer required by recomp.h's MEM_* macros.
    legacy_decl = 'extern uint32_t rocket_render_queue_v28_entry_address(int index);\n'
    legacy_call = 'ctx->r16 = (gpr)rocket_render_queue_v28_entry_address((int)SIGNED(ctx->r18));'
    compat_changed = False
    if legacy_decl in text:
        text = text.replace(legacy_decl,
            'extern uint32_t rocket_render_queue_v28_entry_address(uint8_t* rdram, int index);\n', 1)
        compat_changed = True
        s, e = function_span(text, B694_MARKER)
        body = text[s:e]
    if legacy_call in body:
        body = body.replace(legacy_call,
            'ctx->r16 = (gpr)rocket_render_queue_v28_entry_address(rdram, (int)SIGNED(ctx->r18));', 1)
        compat_changed = True

    if all(marker in body for marker in (BEGIN_CALL, PREPARE_MARKER, SELECT_MARKER, END_CALL)):
        if compat_changed:
            s2, e2 = function_span(text, B694_MARKER)
            text = text[:s2] + body + text[e2:]
            write_text(path, text, bom, nl)
            return True
        return False

    if B694_DECLS not in text[max(0, s - 1400):s]:
        text = text[:s] + B694_DECLS + text[s:]
        s, e = function_span(text, B694_MARKER)
        body = text[s:e]

    if BEGIN_CALL not in body:
        anchor = '    after_0:\n    // 0x8008B7E0: lw          $a1, 0x494($sp)'
        if anchor not in body:
            raise RuntimeError('Could not locate renderer traversal-begin anchor')
        body = body.replace(
            anchor,
            '    after_0:\n    ' + BEGIN_CALL + '\n    // 0x8008B7E0: lw          $a1, 0x494($sp)',
            1)

    # After the one retail scene traversal, replace the fixed 256-pointer stack
    # list + partition/sort setup with V28's host-side exact final order.  The
    # original draw-state loop and renderer tail remain untouched.
    if PREPARE_MARKER not in body:
        start_anchor = '    after_1:\n'
        end_anchor = 'L_8008B950:\n'
        start = body.find(start_anchor)
        if start < 0:
            raise RuntimeError('Could not locate renderer post-traversal label')
        start += len(start_anchor)
        end = body.find(end_anchor, start)
        if end < 0:
            raise RuntimeError('Could not locate renderer draw-loop setup label')
        replacement = (
            '    ' + PREPARE_MARKER + '\n'
            '    ctx->r19 = (gpr)rocket_render_queue_v28_prepare(rdram, ctx);\n'
            '    MEM_W(0X440, ctx->r29) = ctx->r19;\n'
            '    goto L_8008B950;\n'
        )
        body = body[:start] + replacement + body[end:]

    # The final order already includes opaque-forward and transparent-reverse.
    # Replace only the retail stack-pointer selection block; the matrix cache,
    # RenderParams transitions, alpha transitions, display-list emission, and
    # tail all remain the original generated instructions.
    if SELECT_MARKER not in body:
        start_anchor = '    // 0x8008B978: lw          $v1, 0x440($sp)\n'
        end_anchor = 'L_8008B9AC:\n'
        start = body.find(start_anchor)
        if start < 0:
            raise RuntimeError('Could not locate retail RenderEntry selection block')
        end = body.find(end_anchor, start)
        if end < 0:
            raise RuntimeError('Could not locate RenderEntry selection merge label')
        replacement = (
            '    ' + SELECT_MARKER + '\n'
            '    ctx->r16 = (gpr)rocket_render_queue_v28_entry_address(rdram, (int)SIGNED(ctx->r18));\n'
            '    if (ctx->r16 == 0) goto L_8008BC08;\n'
        )
        body = body[:start] + replacement + body[end:]

    if END_CALL not in body:
        anchor = 'L_8008BC08:\n'
        if anchor not in body:
            raise RuntimeError('Could not locate renderer pre-tail label')
        body = body.replace(anchor, anchor + '    ' + END_CALL + '\n', 1)

    # Replace the function body in the full file.
    s2, e2 = function_span(text, B694_MARKER)
    text = text[:s2] + body + text[e2:]
    write_text(path, text, bom, nl)
    return True


def verify(root: Path):
    add = find_source(root, ADD_MARKER)
    b694 = find_source(root, B694_MARKER)
    add_text = add.read_text(encoding='utf-8-sig')
    b_text = b694.read_text(encoding='utf-8-sig')
    sa, ea = function_span(add_text, ADD_MARKER)
    sb, eb = function_span(b_text, B694_MARKER)
    add_body = add_text[sa:ea]
    b_body = b_text[sb:eb]

    ensure_no_retired_tokens(add_body, 'add_render_entry')
    ensure_no_retired_tokens(b_body, 'func_8008B694')
    if add_body.count(ADD_CALL) != 1:
        raise RuntimeError('v28 add_render_entry interception missing/duplicated')
    for marker in (BEGIN_CALL, PREPARE_MARKER, SELECT_MARKER, END_CALL):
        if b_body.count(marker) != 1:
            raise RuntimeError(f'v28 renderer marker missing/duplicated: {marker}')
    if 'extern uint32_t rocket_render_queue_v28_entry_address(uint8_t* rdram, int index);' not in b_text:
        raise RuntimeError('v28.1 selector declaration must receive rdram')
    if 'rocket_render_queue_v28_entry_address(rdram, (int)SIGNED(ctx->r18))' not in b_body:
        raise RuntimeError('v28.1 selector call must pass rdram')
    if 'rocket_render_queue_v28_entry_address((int)SIGNED(ctx->r18))' in b_body:
        raise RuntimeError('retired v28 selector call without rdram is still present')
    if b_body.count('func_8008AEA0(rdram, ctx);') != 1:
        raise RuntimeError('v28 must retain exactly one retail scene traversal call')
    if b_body.count('func_80092050(rdram, ctx);') != 1:
        raise RuntimeError('v28 must retain exactly one retail material-update call')
    if 'func_8008B4BC(rdram, ctx);' in b_body:
        raise RuntimeError('v28 renderer still executes the fixed-stack retail sort path')
    if b_body.count('gSPDisplayList'):
        # Generated source emits command words directly, so this is usually zero;
        # do not make verification dependent on decompiler naming.
        pass
    return add, b694


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    ap.add_argument('--verify', action='store_true')
    ns = ap.parse_args()
    root = Path(ns.root).resolve()

    add = find_source(root, ADD_MARKER)
    b694 = find_source(root, B694_MARKER)
    if not ns.verify:
        ca = patch_add(add)
        cb = patch_b694(b694)
        print(f"[OK] {'Patched' if ca else 'Verified'} v28 unbounded add path: {add}")
        print(f"[OK] {'Patched' if cb else 'Verified'} v28 single-pass renderer path: {b694}")
    add, b694 = verify(root)
    print('[OK] v28 generated renderer verification PASS: one traversal, one draw loop, no 256-entry replay batches.')


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        print('v28 generated render-queue patch failed: ' + str(exc), file=sys.stderr)
        raise SystemExit(1)
