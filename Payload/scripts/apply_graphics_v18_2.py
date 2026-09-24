#!/usr/bin/env python3
from pathlib import Path
import argparse
import re


def read_text(path: Path):
    data = path.read_bytes()
    bom = data.startswith(b'\xef\xbb\xbf')
    if bom:
        data = data[3:]
    text = data.decode('utf-8')
    nl = '\r\n' if '\r\n' in text else '\n'
    return text.replace('\r\n', '\n'), nl, bom


def write_text(path: Path, text: str, nl: str, bom: bool):
    out = text.replace('\r\n', '\n')
    if nl == '\r\n':
        out = out.replace('\n', '\r\n')
    data = out.encode('utf-8')
    if bom:
        data = b'\xef\xbb\xbf' + data
    path.write_bytes(data)


def function_span(text: str, marker: str):
    start = text.find(marker)
    if start < 0:
        raise RuntimeError('Missing function: ' + marker)
    brace = text.find('{', start)
    if brace < 0:
        raise RuntimeError('Missing opening brace: ' + marker)
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


def verify_renderer_baseline(root: Path):
    checks = {
        root / 'src' / 'widescreen_culling.cpp': [
            'v18 renderer-view-matrix viewport guard active',
            'kViewMatrixOffset = 0x30',
            'view_z_sign',
        ],
        root / 'src' / 'presentation_identity.cpp': [
            'ExpandedRenderEntry',
            'rocket_render_queue_capture',
            'rocket_render_queue_begin_batch',
            '[render-queue] EXPANDED',
        ],
        root / 'src' / 'graphics_enhancements.cpp': [
            'draw_distance_multiplier',
            'static_cast<std::uint32_t>(context->r7)',
        ],
        root / 'scripts' / 'OneClickBuild.ps1': [
            'patch_render_queue_generated.py',
        ],
    }
    for path, tokens in checks.items():
        if not path.is_file():
            raise RuntimeError(f'v18.1 baseline file missing: {path}')
        text = path.read_text(encoding='utf-8-sig')
        for token in tokens:
            if token not in text:
                raise RuntimeError(f'v18.1 renderer baseline missing {token!r} in {path.name}; refusing font-only hotfix')


def patch_ui(path: Path):
    text, nl, bom = read_text(path)

    # The compile log proves current Rocket-R exposes the singular helper.
    # Validate that it really is the font-atlas helper before calling it from
    # RT64's second ImGui context.
    start, end = function_span(text, 'void ConfigureLauncherFont()')
    helper = text[start:end]
    if 'ImGui::GetIO()' not in helper:
        raise RuntimeError('ConfigureLauncherFont() is not the expected ImGui font helper (ImGui::GetIO missing)')
    if ('AddFontFromFileTTF' not in helper and 'AddFontDefault' not in helper and 'FontDefault' not in helper):
        raise RuntimeError('ConfigureLauncherFont() does not appear to configure an ImGui font atlas')

    # Remove only the bad symbols introduced by v18.1. These are undeclared in
    # the current cleaned UI and are exactly the five compiler errors reported.
    stale_names = (
        'g_launcher_context_active',
        'g_launcher_body_font',
        'g_launcher_heading_font',
    )
    lines = text.splitlines(True)
    filtered = []
    removed = 0
    for line in lines:
        if any(name in line for name in stale_names):
            # Fail closed if a declaration somehow exists; the user's current
            # compiler state has no such declarations, so this should only be
            # the stale v18.1 assignments/cleanup.
            stripped = line.strip()
            if stripped.startswith(('ImFont* ', 'bool ', 'static ImFont* ', 'static bool ')):
                raise RuntimeError('Unexpected declaration for retired v18.1 launcher globals; refusing to remove it')
            removed += 1
            continue
        filtered.append(line)
    text = ''.join(filtered)

    if 'ConfigureLauncherFonts();' in text:
        text = text.replace('ConfigureLauncherFonts();', 'ConfigureLauncherFont();')

    # Keep an explicit marker at the actual Inspector handoff. If v18.1 marker
    # exists, migrate it. If it was partially removed, re-anchor safely.
    text = text.replace(
        '// v18.1: RT64 Inspector owns a fresh ImGui context after the startup',
        '// v18.2: RT64 Inspector owns a fresh ImGui context after the startup')

    marker = '// v18.2: RT64 Inspector owns a fresh ImGui context after the startup'
    if marker not in text:
        anchor = '    application.presentQueue->inspector->setIniPath(g_config_directory / "rocket-r-ui.ini");\n'
        if anchor not in text:
            raise RuntimeError('Inspector setIniPath anchor missing from runtime_ui.cpp')
        insertion = (
            anchor +
            '    // v18.2: RT64 Inspector owns a fresh ImGui context after the startup\n'
            '    // launcher context is destroyed. Configure the current font helper\n'
            '    // against this context so the in-game overlay keeps Comic Sans.\n'
            '    ConfigureLauncherFont();\n'
        )
        text = text.replace(anchor, insertion, 1)
    else:
        # Ensure the migrated marker block actually calls the current helper.
        anchor_pos = text.find(marker)
        call_pos = text.find('ConfigureLauncherFont();', anchor_pos)
        if call_pos < 0 or call_pos - anchor_pos > 700:
            # Insert directly before the next logical statement after the comment block.
            setini = '    application.presentQueue->inspector->setIniPath(g_config_directory / "rocket-r-ui.ini");\n'
            pos = text.rfind(setini, 0, anchor_pos + 1)
            if pos < 0:
                raise RuntimeError('Could not re-anchor ConfigureLauncherFont() near Inspector setup')
            insert_at = pos + len(setini)
            text = text[:insert_at] + '    ConfigureLauncherFont();\n' + text[insert_at:]

    # No stale plural helper or stale globals may survive.
    for token in ('ConfigureLauncherFonts();',) + stale_names:
        if token in text:
            raise RuntimeError('stale v18.1 font token remains after hotfix: ' + token)

    # There must be a singular helper call in the Inspector setup region, not
    # merely the helper definition or the startup launcher call.
    inspector = text.find('application.presentQueue->inspector->setIniPath')
    if inspector < 0:
        raise RuntimeError('Inspector setup region missing after patch')
    nearby = text[inspector:inspector + 900]
    if 'ConfigureLauncherFont();' not in nearby:
        raise RuntimeError('Inspector context does not call ConfigureLauncherFont() after patch')

    write_text(path, text, nl, bom)
    return removed


def patch_self_check(path: Path):
    text, nl, bom = read_text(path)
    if '_v181_font_required' not in text:
        raise RuntimeError('v18.1 self-check font block missing')

    # Migrate only the v18.1 font expectations. Renderer/culling requirements
    # remain byte-for-byte unchanged.
    text = text.replace("'ConfigureLauncherFonts();'", "'ConfigureLauncherFont();'")
    text = text.replace("'v18.1: RT64 Inspector owns a fresh ImGui context'", "'v18.2: RT64 Inspector owns a fresh ImGui context'")
    text = text.replace("    'g_launcher_context_active = false;',\n", "    'void ConfigureLauncherFont()',\n")
    text = text.replace(
        '# v18.1: renderer-view-matrix culling + direct add_render_entry capture + safe multi-pass render queue + overlay fonts.',
        '# v18.2: v18.1 renderer visibility/queue baseline + current singular overlay font helper.')
    text = text.replace(
        'SOURCE SELF-CHECK FAILED: FIXED34/v18.1 render visibility / queue / overlay-font policy missing',
        'SOURCE SELF-CHECK FAILED: FIXED34/v18.2 render visibility / queue / overlay-font policy missing')

    if "'ConfigureLauncherFonts();'" in text or "'g_launcher_context_active = false;'" in text:
        raise RuntimeError('stale v18.1 font assertions remain in self_check.py')
    if "'ConfigureLauncherFont();'" not in text or "'v18.2: RT64 Inspector owns a fresh ImGui context'" not in text:
        raise RuntimeError('v18.2 font assertions were not installed in self_check.py')
    write_text(path, text, nl, bom)


def patch_old_verifier(path: Path):
    if not path.is_file():
        return
    text, nl, bom = read_text(path)
    text = text.replace("'v18.1: RT64 Inspector owns a fresh ImGui context'", "'v18.2: RT64 Inspector owns a fresh ImGui context'")
    text = text.replace('"v18.1: RT64 Inspector owns a fresh ImGui context"', '"v18.2: RT64 Inspector owns a fresh ImGui context"')
    text = text.replace("'ConfigureLauncherFonts();'", "'ConfigureLauncherFont();'")
    text = text.replace('"ConfigureLauncherFonts();"', '"ConfigureLauncherFont();"')
    write_text(path, text, nl, bom)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    args = ap.parse_args()
    root = Path(args.root).resolve()

    verify_renderer_baseline(root)
    ui = root / 'src' / 'runtime_ui.cpp'
    sc = root / 'scripts' / 'self_check.py'
    if not ui.is_file() or not sc.is_file():
        raise RuntimeError('Required runtime_ui.cpp/self_check.py missing')

    removed = patch_ui(ui)
    patch_self_check(sc)
    patch_old_verifier(root / 'scripts' / 'verify_graphics_v18_1.py')

    print(f'[OK] Repaired current singular ConfigureLauncherFont() Inspector handoff; removed {removed} stale v18.1 global-reference line(s).')
    print('[OK] Left v18.1 renderer-view culling, r7 Draw Distance, generated queue capture and multi-pass replay untouched.')
    print('[OK] Updated source self-check to the current overlay-font API.')


if __name__ == '__main__':
    main()
