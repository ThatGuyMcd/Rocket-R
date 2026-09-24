#!/usr/bin/env python3
from pathlib import Path
import argparse

FUNC_MARKER = 'RECOMP_FUNC void func_8001ECEC(uint8_t* rdram, recomp_context* ctx)'
BEGIN = '    // === ROCKET-R GRAPHICS V23 AUTHORED SUBMODEL VISIBILITY RETENTION BEGIN ===\n'
END = '    // === ROCKET-R GRAPHICS V23 AUTHORED SUBMODEL VISIBILITY RETENTION END ===\n'
HOOK_DECL = '    extern void rocket_popdiag_visibility_mask_result(recomp_context*);\n'
HOOK_CALL = '    rocket_popdiag_visibility_mask_result(ctx);\n'


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
    raise RuntimeError('Unterminated function: ' + marker)


def find_source(root: Path) -> Path:
    candidates = sorted((root / 'runtime-recomp' / 'RecompiledFuncs').glob('funcs_*.c'))
    hits = []
    for path in candidates:
        try:
            text = path.read_text(encoding='utf-8-sig')
        except Exception:
            continue
        if FUNC_MARKER in text:
            hits.append(path)
    if len(hits) != 1:
        raise RuntimeError(f'Expected exactly one generated source containing func_8001ECEC, found {len(hits)}')
    return hits[0]


def preserve_write(path: Path, text: str, bom: bool, nl: str):
    text = text.replace('\r\n', '\n')
    if nl == '\r\n':
        text = text.replace('\n', '\r\n')
    data = text.encode('utf-8')
    if bom:
        data = b'\xef\xbb\xbf' + data
    path.write_bytes(data)


def verify(path: Path):
    text = path.read_text(encoding='utf-8-sig')
    s, e = function_span(text, FUNC_MARKER)
    body = text[s:e]
    if body.count('ROCKET-R GRAPHICS V23 AUTHORED SUBMODEL VISIBILITY RETENTION BEGIN') != 1:
        raise RuntimeError('v23 visibility-retention marker missing or duplicated')
    if body.count(HOOK_CALL.strip()) != 1:
        raise RuntimeError('v23 visibility telemetry hook missing or duplicated')
    if body.count('ctx->r2 = 1; // v23: allow this authored-mask rejection to proceed to normal frustum/distance testing') != 1:
        raise RuntimeError('v23 authored-mask retention assignment missing or duplicated')
    # The original reject branch must still physically exist immediately below our hook;
    # we recover its result rather than deleting/restructuring guest control flow.
    marker_pos = body.find('ROCKET-R GRAPHICS V23 AUTHORED SUBMODEL VISIBILITY RETENTION BEGIN')
    branch_pos = body.find('// 0x8001EE1C: beq         $v0, $zero, L_8001F08C')
    if marker_pos < 0 or branch_pos < 0 or marker_pos > branch_pos:
        raise RuntimeError('v23 hook is not located at the authored-mask gate')
    # The normal frustum test remains present later in the same renderer function.
    if 'frustum_test(rdram, ctx);' not in body:
        raise RuntimeError('v23 must preserve func_8001ECEC frustum_test')


def patch(path: Path) -> bool:
    raw = path.read_bytes()
    bom = raw.startswith(b'\xef\xbb\xbf')
    text = raw.decode('utf-8-sig').replace('\r\n', '\n')
    nl = '\r\n' if b'\r\n' in raw else '\n'
    s, e = function_span(text, FUNC_MARKER)
    body = text[s:e]

    if 'ROCKET-R GRAPHICS V23 AUTHORED SUBMODEL VISIBILITY RETENTION BEGIN' in body:
        verify(path)
        return False

    anchor = (
        'L_8001EE1C:\n'
        '    // 0x8001EE1C: beq         $v0, $zero, L_8001F08C\n'
        '    if (ctx->r2 == 0) {'
    )
    if body.count(anchor) != 1:
        raise RuntimeError('Could not uniquely locate func_8001ECEC authored submodel visibility-mask gate')

    injection = (
        'L_8001EE1C:\n'
        + BEGIN +
        '    // This r2 value is the result of (submodel->unk24 & camera visibility mask at +CC/+D0).\n'
        '    // Recover ONLY this authored visibility ownership reject. The ordinary forward\n'
        '    // frustum test, render-distance test/fade, alpha rules and v21 queue recovery\n'
        '    // remain authoritative immediately after this gate.\n'
        + HOOK_DECL + HOOK_CALL +
        '    ctx->r2 = 1; // v23: allow this authored-mask rejection to proceed to normal frustum/distance testing\n'
        + END +
        '    // 0x8001EE1C: beq         $v0, $zero, L_8001F08C\n'
        '    if (ctx->r2 == 0) {'
    )
    body = body.replace(anchor, injection, 1)
    text = text[:s] + body + text[e:]
    preserve_write(path, text, bom, nl)
    verify(path)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    ap.add_argument('--verify', action='store_true')
    args = ap.parse_args()
    root = Path(args.root).resolve()
    path = find_source(root)
    if args.verify:
        verify(path)
        print(f'[OK] v23 authored visibility retention verification PASS: {path}')
        return
    changed = patch(path)
    print(f"[OK] {'Patched' if changed else 'Verified'} v23 authored submodel visibility gate: {path}")


if __name__ == '__main__':
    main()
