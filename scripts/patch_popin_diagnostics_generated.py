#!/usr/bin/env python3
from pathlib import Path
import argparse

CALL = 'rocket_popdiag_add_render_entry_attempt(rdram, ctx);'
DECL = 'extern void rocket_popdiag_add_render_entry_attempt(uint8_t* rdram, recomp_context* ctx);\n'


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
        n = text[i+1] if i+1 < len(text) else ''
        if state == 'code':
            if c == '/' and n == '/': state='line'; i += 2; continue
            if c == '/' and n == '*': state='block'; i += 2; continue
            if c == '"': state='string'
            elif c == "'": state='char'
            elif c == '{': depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    return start, i + 1
        elif state == 'line':
            if c == '\n': state='code'
        elif state == 'block':
            if c == '*' and n == '/': state='code'; i += 2; continue
        elif state == 'string':
            if c == '\\': i += 2; continue
            if c == '"': state='code'
        elif state == 'char':
            if c == '\\': i += 2; continue
            if c == "'": state='code'
        i += 1
    raise RuntimeError(f'Unterminated function: {marker}')


def find_add_source(root: Path):
    out = root / 'runtime-recomp' / 'RecompiledFuncs'
    if not out.is_dir():
        raise RuntimeError(f'Generated CPU directory missing: {out}')
    marker = 'RECOMP_FUNC void add_render_entry(uint8_t* rdram, recomp_context* ctx)'
    matches = []
    for path in sorted(out.glob('*.c')):
        try:
            text = path.read_text(encoding='utf-8-sig')
        except UnicodeDecodeError:
            continue
        if marker in text:
            matches.append(path)
    if len(matches) != 1:
        raise RuntimeError(f'Expected exactly one generated add_render_entry source, found {len(matches)}')
    return matches[0]


def patch(path: Path):
    raw = path.read_bytes()
    bom = raw.startswith(b'\xef\xbb\xbf')
    text = raw.decode('utf-8-sig')
    nl = '\r\n' if '\r\n' in text else '\n'
    marker = 'RECOMP_FUNC void add_render_entry(uint8_t* rdram, recomp_context* ctx)'
    s, e = function_span(text, marker)
    body = text[s:e]

    # This diagnostic patch must never coexist with the retired v18/v19 replay system.
    forbidden = ('rocket_render_queue_capture(', 'rocket_original_func_8008B694',
                 'rocket_render_queue_begin_batch(', 'RocketBuildGlobalRenderOrder')
    bad = [tok for tok in forbidden if tok in text]
    if bad:
        raise RuntimeError('Refusing to instrument a retired v18/v19 generated renderer: ' + ', '.join(bad))

    changed = False
    if CALL not in body:
        brace = text.find('{', s)
        if brace < 0 or brace >= e:
            raise RuntimeError('add_render_entry opening brace missing')
        if DECL not in text[max(0, s-300):s]:
            text = text[:s] + DECL + text[s:]
            s += len(DECL)
            e += len(DECL)
            brace += len(DECL)
        text = text[:brace+1] + '\n    ' + CALL + text[brace+1:]
        changed = True

    # Verify exactly one diagnostic call and that it precedes the retail body.
    if text.count(CALL) != 1:
        raise RuntimeError('Diagnostic add_render_entry call missing or duplicated')
    s, e = function_span(text, marker)
    body = text[s:e]
    if '// 0x' in body and body.find(CALL) > body.find('// 0x'):
        raise RuntimeError('Diagnostic call must run before the retail add_render_entry capacity gate')

    if changed:
        text = text.replace('\r\n', '\n')
        if nl == '\r\n':
            text = text.replace('\n', '\r\n')
        data = text.encode('utf-8')
        if bom:
            data = b'\xef\xbb\xbf' + data
        path.write_bytes(data)
    return changed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    ap.add_argument('--verify', action='store_true')
    args = ap.parse_args()
    root = Path(args.root).resolve()
    path = find_add_source(root)
    changed = False if args.verify else patch(path)
    text = path.read_text(encoding='utf-8-sig')
    if text.count(CALL) != 1:
        raise RuntimeError('Generated diagnostic instrumentation verification failed')
    print(f"[OK] {'Instrumented' if changed else 'Verified'} pre-capacity add_render_entry telemetry: {path}")

if __name__ == '__main__':
    main()
