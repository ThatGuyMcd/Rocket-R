#!/usr/bin/env python3
from pathlib import Path
import argparse

V23_BEGIN = '    // === ROCKET-R GRAPHICS V23 AUTHORED SUBMODEL VISIBILITY RETENTION BEGIN ===\n'
V23_END   = '    // === ROCKET-R GRAPHICS V23 AUTHORED SUBMODEL VISIBILITY RETENTION END ===\n'

TARGETS = (
    {
        'func': 'func_8001E954',
        'call': '0x8001E974',
        'branch': '0x8001E97C',
        'label': 'normal-model',
    },
    {
        'func': 'func_800261D0',
        'call': '0x800261E4',
        'branch': '0x800261EC',
        'label': 'special-model-261D0',
    },
    {
        'func': 'func_80050728',
        'call': '0x8005073C',
        'branch': '0x80050744',
        'label': 'special-model-50728',
    },
)

HOOK_DECL = '    extern void rocket_popdiag_object_gate_result(recomp_context*);\n'
HOOK_CALL = '        rocket_popdiag_object_gate_result(ctx);\n'


def function_span(text: str, marker: str):
    start = text.find(marker)
    if start < 0:
        raise RuntimeError('Function marker not found: ' + marker)
    brace = text.find('{', start)
    if brace < 0:
        raise RuntimeError('Opening brace not found: ' + marker)
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
    raise RuntimeError('Unterminated function: ' + marker)


def preserve_write(path: Path, text: str, bom: bool, nl: str):
    text = text.replace('\r\n', '\n')
    if nl == '\r\n':
        text = text.replace('\n', '\r\n')
    data = text.encode('utf-8')
    if bom:
        data = b'\xef\xbb\xbf' + data
    path.write_bytes(data)


def strip_v23_from_text(text: str):
    changed = False
    while V23_BEGIN in text:
        s = text.find(V23_BEGIN)
        e = text.find(V23_END, s)
        if e < 0:
            raise RuntimeError('Found v23 begin marker without end marker')
        e += len(V23_END)
        text = text[:s] + text[e:]
        changed = True
    return text, changed


def find_target_source(root: Path, func: str) -> Path:
    marker = f'RECOMP_FUNC void {func}(uint8_t* rdram, recomp_context* ctx)'
    hits = []
    for path in sorted((root / 'runtime-recomp' / 'RecompiledFuncs').glob('funcs_*.c')):
        try:
            text = path.read_text(encoding='utf-8-sig')
        except Exception:
            continue
        if marker in text:
            hits.append(path)
    if len(hits) != 1:
        raise RuntimeError(f'Expected exactly one generated source containing {func}, found {len(hits)}')
    return hits[0]


def target_marker(t):
    return f'    // === ROCKET-R GRAPHICS V25 PRE-RENDER OBJECT GATE RETENTION BEGIN: {t["func"]} ===\n'


def target_end_marker(t):
    return f'    // === ROCKET-R GRAPHICS V25 PRE-RENDER OBJECT GATE RETENTION END: {t["func"]} ===\n'


def patch_target(path: Path, t) -> bool:
    raw = path.read_bytes()
    bom = raw.startswith(b'\xef\xbb\xbf')
    nl = '\r\n' if b'\r\n' in raw else '\n'
    text = raw.decode('utf-8-sig').replace('\r\n', '\n')
    text, stripped = strip_v23_from_text(text)

    marker = f'RECOMP_FUNC void {t["func"]}(uint8_t* rdram, recomp_context* ctx)'
    s, e = function_span(text, marker)
    body = text[s:e]
    begin = target_marker(t)
    if begin in body:
        if body.count(begin.strip()) != 1:
            raise RuntimeError(f'v25 marker duplicated in {t["func"]}')
        if body.count(HOOK_CALL.strip()) != 1:
            raise RuntimeError(f'v25 telemetry hook missing/duplicated in {t["func"]}')
        if body.count('ctx->r2 = 1; // v25: let downstream submodel/frustum/distance logic decide') != 1:
            raise RuntimeError(f'v25 continuation assignment missing/duplicated in {t["func"]}')
        if stripped:
            preserve_write(path, text, bom, nl)
        return stripped

    # N64Recomp emits the return-value branch after an after_0 label for all three
    # pinned Rocket US callsites. Anchor by the original MIPS branch address so we
    # never patch a different boolean in the same function.
    branch_comment = f'    // {t["branch"]}: beq'
    pos = body.find(branch_comment)
    if pos < 0:
        raise RuntimeError(f'Could not locate original reject branch {t["branch"]} in {t["func"]}')
    call_pos = body.rfind(f'// {t["call"]}: jal', 0, pos)
    if call_pos < 0:
        raise RuntimeError(f'Could not locate func_8001E824 call {t["call"]} before reject branch in {t["func"]}')
    actual_call = body.find('func_8001E824(rdram, ctx);', call_pos, pos)
    if actual_call < 0:
        raise RuntimeError(f'Generated func_8001E824 call missing between {t["call"]} and {t["branch"]}')

    injection = (
        target_marker(t) +
        '    // func_8001E824 is an upstream whole-object visibility gate. The attached\n'
        '    // reproduction proves models can remain rejected by this gate for many frames\n'
        '    // while visibly inside the camera view. Recover ONLY its zero result here.\n'
        '    // The original submodel visibility mask, viewport-aware frustum test, r7 render\n'
        '    // distance/fade, alpha logic and v21 >256 render-queue recovery remain downstream.\n'
        + HOOK_DECL +
        '    if (ctx->r2 == 0) {\n' +
        HOOK_CALL +
        '        ctx->r2 = 1; // v25: let downstream submodel/frustum/distance logic decide\n'
        '    }\n' +
        target_end_marker(t)
    )
    body = body[:pos] + injection + body[pos:]
    text = text[:s] + body + text[e:]
    preserve_write(path, text, bom, nl)
    return True


def verify(root: Path):
    all_text = ''
    for path in sorted((root / 'runtime-recomp' / 'RecompiledFuncs').glob('funcs_*.c')):
        try:
            all_text += path.read_text(encoding='utf-8-sig')
        except Exception:
            pass
    if 'ROCKET-R GRAPHICS V23 AUTHORED SUBMODEL VISIBILITY RETENTION BEGIN' in all_text:
        raise RuntimeError('v23 authored-mask bypass is still present in generated code')

    for t in TARGETS:
        path = find_target_source(root, t['func'])
        text = path.read_text(encoding='utf-8-sig')
        s, e = function_span(text, f'RECOMP_FUNC void {t["func"]}(uint8_t* rdram, recomp_context* ctx)')
        body = text[s:e]
        if body.count(target_marker(t).strip()) != 1:
            raise RuntimeError(f'v25 marker missing/duplicated in {t["func"]}')
        if body.count('func_8001E824(rdram, ctx);') != 1:
            raise RuntimeError(f'Original func_8001E824 call must remain exactly once in {t["func"]}')
        gate_pos = body.find(target_marker(t).strip())
        branch_pos = body.find(f'// {t["branch"]}: beq')
        call_pos = body.find(f'// {t["call"]}: jal')
        if not (0 <= call_pos < gate_pos < branch_pos):
            raise RuntimeError(f'v25 continuation is not between gate call and reject branch in {t["func"]}')
        if body.count(HOOK_CALL.strip()) != 1:
            raise RuntimeError(f'v25 telemetry hook missing/duplicated in {t["func"]}')
        if body.count('ctx->r2 = 1; // v25: let downstream submodel/frustum/distance logic decide') != 1:
            raise RuntimeError(f'v25 continuation assignment missing/duplicated in {t["func"]}')
        # All three renderers must still call the retail model renderer after the recovered gate.
        if 'func_8001ECEC(rdram, ctx);' not in body:
            raise RuntimeError(f'{t["func"]} no longer reaches func_8001ECEC')
    print('[OK] v25 generated object-gate verification PASS: 3 exact render callsites recovered; v23 bypass absent.')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    ap.add_argument('--verify', action='store_true')
    args = ap.parse_args()
    root = Path(args.root).resolve()

    if not (root / 'runtime-recomp' / 'RecompiledFuncs').is_dir():
        raise RuntimeError('runtime-recomp/RecompiledFuncs is missing')

    if args.verify:
        verify(root)
        return

    # Strip the now-disproven v23 block from whichever generated file owns it.
    for path in sorted((root / 'runtime-recomp' / 'RecompiledFuncs').glob('funcs_*.c')):
        raw = path.read_bytes()
        text = raw.decode('utf-8-sig').replace('\r\n', '\n')
        clean, changed = strip_v23_from_text(text)
        if changed:
            preserve_write(path, clean, raw.startswith(b'\xef\xbb\xbf'), '\r\n' if b'\r\n' in raw else '\n')
            print(f'[OK] Removed disproven v23 authored-mask bypass: {path}')

    for t in TARGETS:
        path = find_target_source(root, t['func'])
        changed = patch_target(path, t)
        print(f"[OK] {'Patched' if changed else 'Verified'} v25 pre-render gate {t['call']} -> {t['branch']}: {path}")

    verify(root)


if __name__ == '__main__':
    main()
