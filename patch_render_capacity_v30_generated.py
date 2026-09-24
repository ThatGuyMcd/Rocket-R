#!/usr/bin/env python3
from pathlib import Path
import argparse

ADD_MARKER = 'RECOMP_FUNC void add_render_entry(uint8_t* rdram, recomp_context* ctx)'
B694_MARKER = 'RECOMP_FUNC void func_8008B694(uint8_t* rdram, recomp_context* ctx)'
V30_MARKER = 'ROCKET-R GRAPHICS V30 EXPANDED NATIVE RENDER LIST'
ENTRY_BASE = '0X8040'
SORT_BASE = '0X8044'
CAPACITY = '0X2000'  # 8192 RenderEntry records


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


def find_source(root: Path, marker: str) -> Path:
    out = root / 'runtime-recomp' / 'RecompiledFuncs'
    if not out.is_dir():
        raise RuntimeError(f'Generated CPU directory missing: {out}')
    matches = []
    for path in sorted(out.glob('*.c')):
        try: text = path.read_text(encoding='utf-8-sig')
        except UnicodeDecodeError: continue
        if marker in text: matches.append(path)
    if len(matches) != 1:
        raise RuntimeError(f'Expected exactly one generated source for {marker}, found {len(matches)}')
    return matches[0]


def preserve_write(path: Path, text: str, bom: bool, nl: str):
    text = text.replace('\r\n', '\n')
    if nl == '\r\n': text = text.replace('\n', '\r\n')
    data = text.encode('utf-8')
    if bom: data = b'\xef\xbb\xbf' + data
    path.write_bytes(data)


def exact_replace(body: str, old: str, new: str, desc: str) -> str:
    count = body.count(old)
    if count != 1:
        raise RuntimeError(f'{desc}: expected exactly one retail pattern, found {count}')
    return body.replace(old, new, 1)


def patch_add(path: Path) -> bool:
    raw = path.read_bytes(); bom = raw.startswith(b'\xef\xbb\xbf')
    text = raw.decode('utf-8-sig'); nl = '\r\n' if '\r\n' in text else '\n'; text = text.replace('\r\n','\n')
    s,e = function_span(text, ADD_MARKER); body = text[s:e]
    if V30_MARKER in body:
        verify_add_body(body); return False
    forbidden = ('ROCKET_QUEUE_V21_PROCESS_BATCH', 'rocket_render_queue_v28_', 'rocket_render_queue_capture_entry(rdram, ctx);')
    bad = [x for x in forbidden if x in text]
    if bad: raise RuntimeError('Refusing to stack v30 on an experimental queue patch: ' + ', '.join(bad))

    # Only change the queue-base arithmetic and capacity gate. The retail body
    # still constructs every RenderEntry byte-for-byte exactly as Rocket did.
    body = exact_replace(body,
'''    // 0x8008B268: lui         $v1, 0x800B
    ctx->r3 = S32(0X800B << 16);
    // 0x8008B26C: addiu       $v1, $v1, -0x2500
    ctx->r3 = ADD32(ctx->r3, -0X2500);''',
'''    // 0x8008B268/26C: v30 Expansion Pak RenderEntry base (0x80400000)
    ctx->r3 = S32(0X8040 << 16);
    ctx->r3 = ADD32(ctx->r3, 0X0);''', 'add_render_entry queue base')
    body = exact_replace(body,
        '    ctx->r2 = ctx->r2 < 0X100 ? 1 : 0;',
        '    ctx->r2 = ctx->r2 < 0X2000 ? 1 : 0; // v30: 8192 entries',
        'add_render_entry capacity')
    brace = body.find('{')
    body = body[:brace+1] + f'\n    // {V30_MARKER}: retail entry construction, expanded storage only.' + body[brace+1:]
    text = text[:s] + body + text[e:]
    preserve_write(path,text,bom,nl)
    return True


def patch_b694(path: Path) -> bool:
    raw=path.read_bytes(); bom=raw.startswith(b'\xef\xbb\xbf')
    text=raw.decode('utf-8-sig'); nl='\r\n' if '\r\n' in text else '\n'; text=text.replace('\r\n','\n')
    s,e=function_span(text,B694_MARKER); body=text[s:e]
    if V30_MARKER in body:
        verify_b694_body(body); return False
    forbidden=('ROCKET_QUEUE_V21_PROCESS_BATCH','rocket_render_queue_v28_','rocket_render_queue_prepare_first_batch')
    bad=[x for x in forbidden if x in text]
    if bad: raise RuntimeError('Refusing to stack v30 on an experimental queue patch: '+', '.join(bad))

    # Arm the expanded arena immediately before Rocket's one and only scene traversal.
    anchor='''    after_0:
    // 0x8008B7E0: lw          $a1, 0x494($sp)'''
    repl=f'''    after_0:
    // {V30_MARKER}: begin this renderer pass in Expansion Pak RenderEntry RAM.
    // D_800AF300 remains Rocket's authoritative moving end pointer, so any
    // object-side code that snapshots/ranges it continues to see the real list.
    ctx->r2 = S32(0X800B << 16);
    MEM_W(-0XD00, ctx->r2) = S32(0X8040 << 16);
    // 0x8008B7E0: lw          $a1, 0x494($sp)'''
    body=exact_replace(body,anchor,repl,'renderer traversal-start arena arm')

    # Count from the new RenderEntry base using Rocket's original /24 arithmetic.
    body=exact_replace(body,
'''    // 0x8008B7F0: lui         $a0, 0x800B
    ctx->r4 = S32(0X800B << 16);
    // 0x8008B7F4: lw          $v1, -0xD00($v0)
    ctx->r3 = MEM_W(ctx->r2, -0XD00);
    // 0x8008B7F8: addiu       $a0, $a0, -0x2500
    ctx->r4 = ADD32(ctx->r4, -0X2500);''',
'''    // 0x8008B7F0/B7F8: v30 Expansion Pak RenderEntry base (0x80400000)
    ctx->r4 = S32(0X8040 << 16);
    // 0x8008B7F4: lw          $v1, -0xD00($v0)
    ctx->r3 = MEM_W(ctx->r2, -0XD00);
    ctx->r4 = ADD32(ctx->r4, 0X0);''', 'renderer queue-count base')

    # Build the pointer list in Expansion Pak RAM instead of the fixed 256-pointer stack array.
    body=exact_replace(body,
'''    // 0x8008B82C: addiu       $a3, $sp, 0x40
    ctx->r7 = ADD32(ctx->r29, 0X40);''',
'''    // 0x8008B82C: v30 sort-pointer arena (0x80440000)
    ctx->r7 = S32(0X8044 << 16);''', 'first sort-base assignment')
    body=exact_replace(body,
'''    // 0x8008B830: addiu       $v1, $sp, 0x20
    ctx->r3 = ADD32(ctx->r29, 0X20);''',
'''    // 0x8008B830: v30 pointer-list write cursor
    ctx->r3 = S32(0X8044 << 16);''', 'pointer-list cursor')
    body=exact_replace(body,
'''    // 0x8008B834: sw          $a0, 0x20($v1)
    MEM_W(0X20, ctx->r3) = ctx->r4;''',
'''    // 0x8008B834: store directly into v30 pointer arena
    MEM_W(0X0, ctx->r3) = ctx->r4;''', 'pointer-list store')
    body=exact_replace(body,
'''    // 0x8008B850: addiu       $a3, $sp, 0x40
    ctx->r7 = ADD32(ctx->r29, 0X40);''',
'''    // 0x8008B850: v30 sort-pointer arena (0x80440000)
    ctx->r7 = S32(0X8044 << 16);''', 'second sort-base assignment')
    body=exact_replace(body,
'''    // 0x8008B92C: addiu       $s0, $sp, 0x40
    ctx->r16 = ADD32(ctx->r29, 0X40);''',
'''    // 0x8008B92C: v30 sort-pointer arena passed to Rocket's original heapsort
    ctx->r16 = S32(0X8044 << 16);''', 'heapsort base')

    # The draw loop remains original. Only its two sortedEntryList address calculations
    # are redirected from sp+0x40 to the expanded pointer arena.
    body=exact_replace(body,
'''    // 0x8008B988: addu        $v0, $sp, $v0
    ctx->r2 = ADD32(ctx->r29, ctx->r2);
    // 0x8008B98C: lw          $s0, 0x40($v0)
    ctx->r16 = MEM_W(ctx->r2, 0X40);''',
'''    // 0x8008B988/B98C: v30 opaque pointer from expanded sortedEntryList
    ctx->r2 = ADD32(S32(0X8044 << 16), ctx->r2);
    ctx->r16 = MEM_W(ctx->r2, 0X0);''', 'opaque draw pointer')
    body=exact_replace(body,
'''    // 0x8008B9A4: addu        $v0, $sp, $v0
    ctx->r2 = ADD32(ctx->r29, ctx->r2);
    // 0x8008B9A8: lw          $s0, 0x3C($v0)
    ctx->r16 = MEM_W(ctx->r2, 0X3C);''',
'''    // 0x8008B9A4/B9A8: v30 transparent pointer from expanded sortedEntryList
    ctx->r2 = ADD32(S32(0X8044 << 16), ctx->r2);
    ctx->r16 = MEM_W(ctx->r2, -0X4);''', 'transparent draw pointer')

    text=text[:s]+body+text[e:]
    preserve_write(path,text,bom,nl)
    return True


def verify_add_body(body: str):
    req=(V30_MARKER,'S32(0X8040 << 16)','ctx->r2 < 0X2000')
    miss=[x for x in req if x not in body]
    if miss: raise RuntimeError('v30 add_render_entry verification failed: '+', '.join(miss))
    if 'ctx->r2 < 0X100 ? 1 : 0;' in body:
        raise RuntimeError('Retail 256-entry capacity gate still active')


def verify_b694_body(body: str):
    req=(V30_MARKER,'MEM_W(-0XD00, ctx->r2) = S32(0X8040 << 16);','S32(0X8044 << 16)',
         'ctx->r16 = MEM_W(ctx->r2, 0X0);','ctx->r16 = MEM_W(ctx->r2, -0X4);')
    miss=[x for x in req if x not in body]
    if miss: raise RuntimeError('v30 renderer verification failed: '+', '.join(miss))
    if body.count('func_8008AEA0(rdram, ctx);') != 1:
        raise RuntimeError('v30 must retain exactly one scene traversal')
    if body.count('func_8008B4BC(rdram, ctx);') != 2:
        raise RuntimeError('v30 must retain Rocket\'s two original heapsort calls')
    if body.count('func_80092050(rdram, ctx);') != 1:
        raise RuntimeError('v30 must retain exactly one material setup call')
    if body.count('draw_rectangle(rdram, ctx);') > 1 or body.count('func_80091F54(rdram, ctx);') > 1:
        raise RuntimeError('v30 must not duplicate Rocket\'s renderer tail')


def verify(root: Path):
    add=find_source(root,ADD_MARKER); b=find_source(root,B694_MARKER)
    at=add.read_text(encoding='utf-8-sig'); bt=b.read_text(encoding='utf-8-sig')
    s,e=function_span(at,ADD_MARKER); verify_add_body(at[s:e])
    s,e=function_span(bt,B694_MARKER); verify_b694_body(bt[s:e])
    return add,b


def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--root',required=True); ap.add_argument('--verify',action='store_true')
    args=ap.parse_args(); root=Path(args.root).resolve()
    add=find_source(root,ADD_MARKER); b=find_source(root,B694_MARKER)
    if not args.verify:
        ca=patch_add(add); cb=patch_b694(b)
        print(f"[OK] {'Patched' if ca else 'Verified'} expanded native RenderEntry arena: {add}")
        print(f"[OK] {'Patched' if cb else 'Verified'} expanded native sort/draw arena: {b}")
    verify(root)
    print('[OK] v30 generated renderer verification PASS: 8192 entries, one traversal, original two heapsorts, original draw loop/tail.')

if __name__=='__main__':
    main()
