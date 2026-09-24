#!/usr/bin/env python3
from pathlib import Path
import argparse

SIGS = (
    '[[nodiscard]] float RocketRenderDepth(const ExpandedRenderEntry& entry)',
    '[[nodiscard]] bool RocketRenderOpaque(const ExpandedRenderEntry& entry)',
    'void RocketRenderHeapSort(std::vector<std::size_t>& order,',
    'void RocketBuildGlobalRenderOrder()',
)
BEGIN = 'extern "C" void rocket_render_queue_begin(std::uint8_t*, recomp_context*)'


def read_text(path: Path):
    data = path.read_bytes()
    bom = data.startswith(b'\xef\xbb\xbf')
    if bom:
        data = data[3:]
    text = data.decode('utf-8')
    nl = '\r\n' if '\r\n' in text else '\n'
    return text.replace('\r\n', '\n'), nl, bom


def write_text(path: Path, text: str, nl: str, bom: bool):
    out = text.replace('\n', nl).encode('utf-8')
    if bom:
        out = b'\xef\xbb\xbf' + out
    path.write_bytes(out)


def function_span_at(text: str, marker: str, start: int):
    brace = text.find('{', start)
    if brace < 0:
        raise RuntimeError('Missing opening brace for: ' + marker)
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


def find_definition_spans(text: str, marker: str):
    spans = []
    pos = 0
    while True:
        start = text.find(marker, pos)
        if start < 0:
            break
        spans.append(function_span_at(text, marker, start))
        pos = spans[-1][1]
    return spans


def patch(path: Path):
    text, nl, bom = read_text(path)
    if text.find(BEGIN) < 0:
        raise RuntimeError('v19 queue begin function not found; refusing to guess')

    removed = {}
    # Remove later definitions independently. This intentionally makes no
    # assumptions about comments/globals/other queue code between duplicates.
    all_delete = []
    for marker in SIGS:
        spans = find_definition_spans(text, marker)
        if not spans:
            raise RuntimeError('Missing expected v19 helper definition: ' + marker)
        removed[marker] = max(0, len(spans) - 1)
        all_delete.extend(spans[1:])

    for start, end in sorted(all_delete, reverse=True):
        # Eat at most the immediately following blank lines; never consume
        # comments, declarations, or another function.
        tail = end
        while tail < len(text) and text[tail] in ' \t':
            tail += 1
        if tail < len(text) and text[tail] == '\n':
            tail += 1
            second = tail
            while second < len(text) and text[second] in ' \t':
                second += 1
            if second < len(text) and text[second] == '\n':
                tail = second + 1
        text = text[:start] + text[tail:]

    write_text(path, text, nl, bom)
    return removed


def verify(path: Path):
    text = path.read_text(encoding='utf-8-sig')
    for marker in SIGS:
        count = len(find_definition_spans(text, marker))
        if count != 1:
            raise RuntimeError(f'Expected exactly one v19 helper definition for {marker!r}; found {count}')
    if len(find_definition_spans(text, BEGIN)) != 1:
        raise RuntimeError('Expected exactly one rocket_render_queue_begin definition')
    for token in (
        'rocket_render_queue_finalize_capture',
        'g_expanded_render_order',
        'g_expanded_render_opaque_count',
        'g_render_queue_final_pass',
    ):
        if token not in text:
            raise RuntimeError('Required v19 queue token missing after dedupe: ' + token)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', required=True)
    args = ap.parse_args()
    root = Path(args.repo).resolve()
    target = root / 'src' / 'presentation_identity.cpp'
    if not target.is_file():
        raise RuntimeError('Missing ' + str(target))
    removed = patch(target)
    verify(target)
    total = sum(removed.values())
    if total:
        print(f'[OK] Removed {total} duplicate v19 global-render helper definition(s); exactly one of each remains.')
    else:
        print('[OK] v19 global-render helper definitions were already canonical: exactly one of each remains.')
    print('[OK] Queue API, Draw Distance/culling source and interpolation identity code were not modified.')

if __name__ == '__main__':
    main()
