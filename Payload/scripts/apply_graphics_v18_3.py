#!/usr/bin/env python3
from pathlib import Path
import argparse
import re

GOOD_LITERAL = '"retail %zu-entry list; replaying safely in %zu retail-sized passes.\\n",'
BAD_LITERAL_RE = re.compile(r'"retail %zu-entry list; replaying safely in %zu retail-sized passes\.\r?\n",')
SELF_CHECK_MARKER = '# v18.3: render-queue expansion diagnostic must remain a valid escaped C++ string literal.'


def read_text(path: Path):
    raw = path.read_bytes()
    bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw.decode("utf-8-sig")
    nl = "\r\n" if "\r\n" in text else "\n"
    return text, nl, bom


def write_text(path: Path, text: str, nl: str, bom: bool):
    text = text.replace("\r\n", "\n")
    if nl == "\r\n":
        text = text.replace("\n", "\r\n")
    data = text.encode("utf-8")
    if bom:
        data = b"\xef\xbb\xbf" + data
    path.write_bytes(data)


def verify_queue_baseline(root: Path):
    p = root / 'src' / 'presentation_identity.cpp'
    q = root / 'scripts' / 'patch_render_queue_generated.py'
    o = root / 'scripts' / 'OneClickBuild.ps1'
    for path in (p, q, o):
        if not path.is_file():
            raise RuntimeError(f'Required v18 queue baseline file missing: {path}')
    text = p.read_text(encoding='utf-8-sig')
    for token in (
        'ExpandedRenderEntry',
        'rocket_render_queue_capture',
        'rocket_render_queue_begin_batch',
        'rocket_render_queue_next',
        'kGuestRenderQueueBase = 0x800ADB00U',
        '[render-queue] EXPANDED',
    ):
        if token not in text:
            raise RuntimeError(f'v18 queue baseline token missing from presentation_identity.cpp: {token}')
    if 'scripts\\patch_render_queue_generated.py' not in o.read_text(encoding='utf-8-sig'):
        raise RuntimeError('OneClickBuild persistent generated queue patch hook is missing')


def patch_presentation(path: Path):
    text, nl, bom = read_text(path)
    matches = list(BAD_LITERAL_RE.finditer(text))
    if len(matches) > 1:
        raise RuntimeError(f'Found {len(matches)} malformed queue diagnostic literals; refusing ambiguous repair')
    if len(matches) == 1:
        text = BAD_LITERAL_RE.sub(GOOD_LITERAL.replace('\\', r'\\'), text, count=1)
        # re.sub replacement escaping is awkward; normalize the exact result below.
        text = text.replace(
            '"retail %zu-entry list; replaying safely in %zu retail-sized passes.\\\\n",',
            GOOD_LITERAL,
            1,
        )
        write_text(path, text, nl, bom)
        return True
    if GOOD_LITERAL in text:
        return False
    raise RuntimeError('Neither the known malformed nor corrected v18 queue diagnostic literal was found')


def patch_self_check(path: Path):
    text, nl, bom = read_text(path)
    if SELF_CHECK_MARKER in text:
        return False
    if '_v181_presentation' not in text:
        raise RuntimeError('v18 presentation self-check variable is missing; refusing to guess')
    block = (
        '\n' + SELF_CHECK_MARKER + '\n'
        "_v183_queue_status_literal = '\"retail %zu-entry list; replaying safely in %zu retail-sized passes.\\\\n\",' in _v181_presentation\n"
        "if not _v183_queue_status_literal:\n"
        "    raise SystemExit('SOURCE SELF-CHECK FAILED: v18.3 render-queue diagnostic string literal is malformed')\n"
    )
    pos = text.rfind('\nprint(')
    if pos < 0:
        pos = len(text)
    text = text[:pos] + block + text[pos:]
    write_text(path, text, nl, bom)
    return True


def patch_old_verifier(path: Path):
    if not path.is_file():
        return False
    text, nl, bom = read_text(path)
    marker = "v18.3 queue diagnostic literal verification"
    if marker in text:
        return False
    anchor = "    if '[render-queue] SATURATION' in p:\n        fail('retired saturation-only diagnostic remains')\n"
    if anchor not in text:
        return False
    insertion = anchor + (
        "\n    # v18.3 queue diagnostic literal verification\n"
        "    if '\"retail %zu-entry list; replaying safely in %zu retail-sized passes.\\\\n\",' not in p:\n"
        "        fail('v18.3 queue expansion diagnostic has an invalid C++ newline/string literal')\n"
    )
    text = text.replace(anchor, insertion, 1)
    write_text(path, text, nl, bom)
    return True


def patch_extracted_v181_payload(path: Path):
    # Optional only: users commonly extract patch zips directly into D:\\Rocket-R.
    # Repair the old one-time migration payload too, so manually re-running v18.1
    # cannot recreate the malformed source string. It is NOT used by OneClickBuild.
    if not path.is_file():
        return False
    text, nl, bom = read_text(path)
    if GOOD_LITERAL in text:
        return False
    matches = list(BAD_LITERAL_RE.finditer(text))
    if len(matches) != 1:
        return False
    s, e = matches[0].span()
    text = text[:s] + GOOD_LITERAL + text[e:]
    write_text(path, text, nl, bom)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    args = ap.parse_args()
    root = Path(args.root).resolve()

    verify_queue_baseline(root)
    presentation = root / 'src' / 'presentation_identity.cpp'
    self_check = root / 'scripts' / 'self_check.py'
    if not self_check.is_file():
        raise RuntimeError(f'Missing source self-check: {self_check}')

    changed = patch_presentation(presentation)
    patch_self_check(self_check)
    patch_old_verifier(root / 'scripts' / 'verify_graphics_v18_1.py')
    payload_fixed = patch_extracted_v181_payload(root / 'Payload' / 'scripts' / 'apply_graphics_v18_1.py')

    print('[OK] ' + ('Repaired' if changed else 'Confirmed') + ' v18 queue expansion diagnostic C++ string literal.')
    print('[OK] Added a source self-check guard for the escaped queue diagnostic newline.')
    if payload_fixed:
        print('[OK] Also repaired the extracted one-time v18.1 migration payload so manual re-application cannot recreate this typo.')
    print('[OK] Generated queue patcher, renderer replay, culling, Draw Distance and overlay-font code were not modified.')

if __name__ == '__main__':
    main()
