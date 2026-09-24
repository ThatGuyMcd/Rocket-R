#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import argparse
import re

ADD_MARKER = 'RECOMP_FUNC void add_render_entry(uint8_t* rdram, recomp_context* ctx)'
B694_MARKER = 'RECOMP_FUNC void func_8008B694(uint8_t* rdram, recomp_context* ctx)'
B594_VRAM_MARKER = '// 0x8008B5C0: addiu       $v1, $v1, -0x2500'
B594_V32_MARKER = '// 0x8008B5C0: v32 live RenderEntry queue base (0x80600000)'
V32_MARKER = 'ROCKET-R GRAPHICS V32 SINGLE LIVE RENDER QUEUE'
ENTRY_BASE = '0X8060'
SORT_BASE = '0X8070'
CAPACITY = '0X8000'  # 32768 RenderEntry records


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


def containing_function_span(text: str, needle: str):
    pos = text.find(needle)
    if pos < 0:
        raise RuntimeError(f'VRAM marker not found: {needle}')
    start = text.rfind('RECOMP_FUNC ', 0, pos)
    if start < 0:
        raise RuntimeError(f'Could not find function start before: {needle}')
    marker_end = text.find('{', start)
    if marker_end < 0 or marker_end > pos:
        raise RuntimeError(f'Could not locate containing function for: {needle}')
    marker = text[start:marker_end].strip()
    return function_span(text, marker)


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




def find_b594_source(root: Path) -> Path:
    out = root / 'runtime-recomp' / 'RecompiledFuncs'
    if not out.is_dir():
        raise RuntimeError(f'Generated CPU directory missing: {out}')
    matches = []
    for path in sorted(out.glob('*.c')):
        try:
            text = path.read_text(encoding='utf-8-sig')
        except UnicodeDecodeError:
            continue
        if B594_VRAM_MARKER in text or B594_V32_MARKER in text:
            matches.append(path)
    if len(matches) != 1:
        raise RuntimeError(f'Expected exactly one generated source for func_8008B594 initializer, found {len(matches)}')
    return matches[0]

def preserve_write(path: Path, text: str, bom: bool, nl: str):
    text = text.replace('\r\n', '\n')
    if nl == '\r\n':
        text = text.replace('\n', '\r\n')
    data = text.encode('utf-8')
    if bom:
        data = b'\xef\xbb\xbf' + data
    path.write_bytes(data)


def exact_replace(body: str, old: str, new: str, desc: str) -> str:
    count = body.count(old)
    if count != 1:
        raise RuntimeError(f'{desc}: expected exactly one retail pattern, found {count}')
    return body.replace(old, new, 1)


def reject_experiments(text: str):
    forbidden = (
        'rocket_render_capacity_v31_',
        'ROCKET-R GRAPHICS V31',
        'ROCKET-R GRAPHICS V30',
        'rocket_render_queue_v28_',
        'ROCKET_QUEUE_V21_PROCESS_BATCH',
        'rocket_render_queue_prepare_first_batch',
    )
    bad = [x for x in forbidden if x in text]
    if bad:
        raise RuntimeError('Fresh N64Recomp output still contains an older queue experiment: ' + ', '.join(bad))


def patch_b594(path: Path) -> bool:
    raw = path.read_bytes(); bom = raw.startswith(b'\xef\xbb\xbf')
    text = raw.decode('utf-8-sig'); nl = '\r\n' if '\r\n' in text else '\n'; text = text.replace('\r\n','\n')
    span_marker = B594_VRAM_MARKER if B594_VRAM_MARKER in text else B594_V32_MARKER
    s,e = containing_function_span(text, span_marker); body = text[s:e]
    if V32_MARKER in body:
        verify_b594_body(body); return False
    reject_experiments(body)
    body = exact_replace(body,
'''    // 0x8008B5C0: addiu       $v1, $v1, -0x2500
    ctx->r3 = ADD32(ctx->r3, -0X2500);''',
'''    // 0x8008B5C0: v32 live RenderEntry queue base (0x80600000)
    // Retail initializes D_800AF300 here before BOTH renderer passes. Keep that
    // exact lifetime, only move the backing array into Expansion Pak RDRAM.
    ctx->r3 = S32(0X8060 << 16);''', 'func_8008B594 live queue initialization')
    brace = body.find('{')
    body = body[:brace+1] + f'\n    // {V32_MARKER}: first-pass queue initializer; no staging/copy pool.' + body[brace+1:]
    text = text[:s] + body + text[e:]
    preserve_write(path, text, bom, nl)
    return True


def patch_add(path: Path) -> bool:
    raw = path.read_bytes(); bom = raw.startswith(b'\xef\xbb\xbf')
    text = raw.decode('utf-8-sig'); nl = '\r\n' if '\r\n' in text else '\n'; text = text.replace('\r\n','\n')
    s,e = function_span(text, ADD_MARKER); body = text[s:e]
    if V32_MARKER in body:
        verify_add_body(body); return False
    reject_experiments(body)
    body = exact_replace(body,
'''    // 0x8008B268: lui         $v1, 0x800B
    ctx->r3 = S32(0X800B << 16);
    // 0x8008B26C: addiu       $v1, $v1, -0x2500
    ctx->r3 = ADD32(ctx->r3, -0X2500);''',
'''    // 0x8008B268/26C: v32 live RenderEntry base (0x80600000)
    ctx->r3 = S32(0X8060 << 16);
    ctx->r3 = ADD32(ctx->r3, 0X0);''', 'add_render_entry live queue base')
    body = exact_replace(body,
        '    ctx->r2 = ctx->r2 < 0X100 ? 1 : 0;',
        '    ctx->r2 = ctx->r2 < 0X8000 ? 1 : 0; // v32: 32768 live entries',
        'add_render_entry capacity')
    brace = body.find('{')
    body = body[:brace+1] + f'\n    // {V32_MARKER}: original RenderEntry construction; expanded live array only.' + body[brace+1:]
    text = text[:s] + body + text[e:]
    preserve_write(path, text, bom, nl)
    return True


def patch_b694(path: Path) -> bool:
    raw = path.read_bytes(); bom = raw.startswith(b'\xef\xbb\xbf')
    text = raw.decode('utf-8-sig'); nl = '\r\n' if '\r\n' in text else '\n'; text = text.replace('\r\n','\n')
    s,e = function_span(text, B694_MARKER); body = text[s:e]
    if V32_MARKER in body:
        verify_b694_body(body); return False
    reject_experiments(body)

    # CRITICAL: do NOT write D_800AF300 before func_8008AEA0. Retail Rocket
    # carries the same live list from func_8008B594 through this second pass.
    # v30 broke that continuity by inserting a second reset here.
    body = exact_replace(body,
'''    // 0x8008B7F0: lui         $a0, 0x800B
    ctx->r4 = S32(0X800B << 16);
    // 0x8008B7F4: lw          $v1, -0xD00($v0)
    ctx->r3 = MEM_W(ctx->r2, -0XD00);
    // 0x8008B7F8: addiu       $a0, $a0, -0x2500
    ctx->r4 = ADD32(ctx->r4, -0X2500);''',
'''    // 0x8008B7F0/B7F8: v32 count from the SAME live queue initialized by func_8008B594
    ctx->r4 = S32(0X8060 << 16);
    // 0x8008B7F4: lw          $v1, -0xD00($v0)
    ctx->r3 = MEM_W(ctx->r2, -0XD00);
    ctx->r4 = ADD32(ctx->r4, 0X0);''', 'renderer live queue count base')

    # Move ONLY the fixed 256-pointer sortedEntryList scratch array out of the
    # stack frame. The RenderEntry population itself remains one continuous list.
    body = exact_replace(body,
'''    // 0x8008B82C: addiu       $a3, $sp, 0x40
    ctx->r7 = ADD32(ctx->r29, 0X40);''',
'''    // 0x8008B82C: v32 expanded sortedEntryList scratch (0x80700000)
    ctx->r7 = S32(0X8070 << 16);''', 'first sort-base assignment')
    body = exact_replace(body,
'''    // 0x8008B830: addiu       $v1, $sp, 0x20
    ctx->r3 = ADD32(ctx->r29, 0X20);''',
'''    // 0x8008B830: v32 sortedEntryList write cursor
    ctx->r3 = S32(0X8070 << 16);''', 'pointer-list cursor')
    body = exact_replace(body,
'''    // 0x8008B834: sw          $a0, 0x20($v1)
    MEM_W(0X20, ctx->r3) = ctx->r4;''',
'''    // 0x8008B834: v32 direct sortedEntryList store
    MEM_W(0X0, ctx->r3) = ctx->r4;''', 'pointer-list store')
    body = exact_replace(body,
'''    // 0x8008B850: addiu       $a3, $sp, 0x40
    ctx->r7 = ADD32(ctx->r29, 0X40);''',
'''    // 0x8008B850: v32 expanded sortedEntryList scratch (0x80700000)
    ctx->r7 = S32(0X8070 << 16);''', 'second sort-base assignment')
    body = exact_replace(body,
'''    // 0x8008B92C: addiu       $s0, $sp, 0x40
    ctx->r16 = ADD32(ctx->r29, 0X40);''',
'''    // 0x8008B92C: v32 expanded sortedEntryList passed to Rocket's original heapsort
    ctx->r16 = S32(0X8070 << 16);''', 'heapsort base')
    body = exact_replace(body,
'''    // 0x8008B988: addu        $v0, $sp, $v0
    ctx->r2 = ADD32(ctx->r29, ctx->r2);
    // 0x8008B98C: lw          $s0, 0x40($v0)
    ctx->r16 = MEM_W(ctx->r2, 0X40);''',
'''    // 0x8008B988/B98C: v32 opaque pointer from expanded sortedEntryList
    ctx->r2 = ADD32(S32(0X8070 << 16), ctx->r2);
    ctx->r16 = MEM_W(ctx->r2, 0X0);''', 'opaque draw pointer')
    body = exact_replace(body,
'''    // 0x8008B9A4: addu        $v0, $sp, $v0
    ctx->r2 = ADD32(ctx->r29, ctx->r2);
    // 0x8008B9A8: lw          $s0, 0x3C($v0)
    ctx->r16 = MEM_W(ctx->r2, 0X3C);''',
'''    // 0x8008B9A4/B9A8: v32 transparent pointer from expanded sortedEntryList
    ctx->r2 = ADD32(S32(0X8070 << 16), ctx->r2);
    ctx->r16 = MEM_W(ctx->r2, -0X4);''', 'transparent draw pointer')
    brace = body.find('{')
    body = body[:brace+1] + f'\n    // {V32_MARKER}: second pass continues the first-pass live queue; no reset/copy/staging.' + body[brace+1:]
    text = text[:s] + body + text[e:]
    preserve_write(path, text, bom, nl)
    return True


def verify_b594_body(body: str):
    req = (V32_MARKER, '0x8008B5C0: v32 live RenderEntry queue base', 'ctx->r3 = S32(0X8060 << 16);', 'MEM_W(-0XD00, ctx->r2) = ctx->r3;')
    miss = [x for x in req if x not in body]
    if miss:
        raise RuntimeError('v32 first-pass initializer verification failed: ' + ', '.join(miss))
    if 'ctx->r3 = ADD32(ctx->r3, -0X2500);' in body:
        raise RuntimeError('v32 first-pass initializer still points at retail RenderEntry storage')


def verify_add_body(body: str):
    req = (V32_MARKER, 'S32(0X8060 << 16)', 'ctx->r2 < 0X8000')
    miss = [x for x in req if x not in body]
    if miss:
        raise RuntimeError('v32 add_render_entry verification failed: ' + ', '.join(miss))
    if 'ctx->r2 < 0X100 ? 1 : 0;' in body:
        raise RuntimeError('Retail 256-entry capacity gate still active')


def verify_b694_body(body: str):
    req = (
        V32_MARKER,
        'S32(0X8060 << 16)',
        'S32(0X8070 << 16)',
        'ctx->r16 = MEM_W(ctx->r2, 0X0);',
        'ctx->r16 = MEM_W(ctx->r2, -0X4);',
    )
    miss = [x for x in req if x not in body]
    if miss:
        raise RuntimeError('v32 renderer verification failed: ' + ', '.join(miss))
    if body.count('func_8008AEA0(rdram, ctx);') != 1:
        raise RuntimeError('v32 must retain exactly one second-pass scene traversal')
    if body.count('func_8008B4BC(rdram, ctx);') != 2:
        raise RuntimeError('v32 must retain Rocket\'s two original heapsort calls')
    if body.count('func_80092050(rdram, ctx);') != 1:
        raise RuntimeError('v32 must retain Rocket\'s original material setup')
    # The ONLY D_800AF300 store in B694 must remain the retail NULL reset at the tail.
    if 'MEM_W(-0XD00, ctx->r2) = S32(0X8060 << 16)' in body:
        raise RuntimeError('v32 must not reset/re-arm the live queue between first and second renderer passes')
    if 'rocket_render_capacity_v31_' in body or 'rocket_render_queue_v28_' in body:
        raise RuntimeError('staging/capture queue code leaked into v32 renderer')


def verify_hidden_consumers(root: Path):
    # These are the audited hidden consumers that must continue to follow the
    # authoritative D_800AF300 pointer. We intentionally do not patch them.
    markers = (
        '// 0x80058654: lw          $s6, -0xD00($v0)',
        '// 0x8001E96C: lw          $s2, -0xD00($v0)',
        '// 0x800201F0: lw          $a0, -0xD00($t1)',
        '// 0x8002066C: sw          $a3, -0xD00($v0)',
    )
    out = root / 'runtime-recomp' / 'RecompiledFuncs'
    corpus = ''
    for p in sorted(out.glob('*.c')):
        try: corpus += p.read_text(encoding='utf-8-sig')
        except UnicodeDecodeError: pass
    missing = [m for m in markers if m not in corpus]
    if missing:
        raise RuntimeError('Audited live-queue consumer marker missing from generated output: ' + ', '.join(missing))


def verify(root: Path):
    b594 = find_b594_source(root)
    add = find_source(root, ADD_MARKER)
    b694 = find_source(root, B694_MARKER)
    bt = b594.read_text(encoding='utf-8-sig'); span_marker = B594_VRAM_MARKER if B594_VRAM_MARKER in bt else B594_V32_MARKER; s,e = containing_function_span(bt, span_marker); verify_b594_body(bt[s:e])
    at = add.read_text(encoding='utf-8-sig'); s,e = function_span(at, ADD_MARKER); verify_add_body(at[s:e])
    rt = b694.read_text(encoding='utf-8-sig'); s,e = function_span(rt, B694_MARKER); verify_b694_body(rt[s:e])
    verify_hidden_consumers(root)
    return b594, add, b694


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    ap.add_argument('--verify', action='store_true')
    args = ap.parse_args()
    root = Path(args.root).resolve()
    b594 = find_b594_source(root)
    add = find_source(root, ADD_MARKER)
    b694 = find_source(root, B694_MARKER)
    if not args.verify:
        c1 = patch_b594(b594)
        c2 = patch_add(add)
        c3 = patch_b694(b694)
        print(f"[OK] {'Patched' if c1 else 'Verified'} first-pass live queue initializer: {b594}")
        print(f"[OK] {'Patched' if c2 else 'Verified'} 32768-entry live add path: {add}")
        print(f"[OK] {'Patched' if c3 else 'Verified'} original global sort/draw over live queue: {b694}")
    verify(root)
    print('[OK] v32 verification PASS: ONE live RenderEntry population, first+second renderer-pass continuity preserved, 32768-entry capacity, original global partition/heapsort/draw retained.')


if __name__ == '__main__':
    main()
