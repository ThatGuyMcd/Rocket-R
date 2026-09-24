#!/usr/bin/env python3
from pathlib import Path
import argparse


def fail(msg):
    print('[ERROR] ' + msg)
    raise SystemExit(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    args = ap.parse_args()
    root = Path(args.root).resolve()

    paths = {
        'ui': root/'src/runtime_ui.cpp',
        'selfcheck': root/'scripts/self_check.py',
        'culling': root/'src/widescreen_culling.cpp',
        'presentation': root/'src/presentation_identity.cpp',
        'graphics': root/'src/graphics_enhancements.cpp',
        'oneclick': root/'scripts/OneClickBuild.ps1',
        'queuepatch': root/'scripts/patch_render_queue_generated.py',
    }
    for label, path in paths.items():
        if not path.is_file(): fail(f'Missing {label}: {path}')
    t = {k:p.read_text(encoding='utf-8-sig') for k,p in paths.items()}

    # Renderer baseline must be exactly the v18.1 lineage the user already installed.
    for token in ('v18 renderer-view-matrix viewport guard active','kViewMatrixOffset = 0x30','view_z_sign'):
        if token not in t['culling']: fail('v18 renderer-view culling missing: '+token)
    for token in ('ExpandedRenderEntry','rocket_render_queue_capture','rocket_render_queue_begin_batch','[render-queue] EXPANDED'):
        if token not in t['presentation']: fail('v18 expanded queue missing: '+token)
    for token in ('draw_distance_multiplier','static_cast<std::uint32_t>(context->r7)'):
        if token not in t['graphics']: fail('r7 Draw Distance baseline missing: '+token)
    if 'patch_render_queue_generated.py' not in t['oneclick']:
        fail('OneClickBuild persistent queue patch hook missing')

    # Current cleaned UI API: one helper, no retired v18.1 globals.
    if 'void ConfigureLauncherFont()' not in t['ui']:
        fail('current ConfigureLauncherFont() helper missing')
    if 'v18.2: RT64 Inspector owns a fresh ImGui context' not in t['ui']:
        fail('v18.2 Inspector font marker missing')
    pos = t['ui'].find('application.presentQueue->inspector->setIniPath')
    if pos < 0 or 'ConfigureLauncherFont();' not in t['ui'][pos:pos+900]:
        fail('Inspector context is not configured with current launcher font helper')
    for token in ('ConfigureLauncherFonts();','g_launcher_context_active','g_launcher_body_font','g_launcher_heading_font'):
        if token in t['ui']:
            fail('retired v18.1 UI token remains: '+token)

    if "'ConfigureLauncherFonts();'" in t['selfcheck'] or "'g_launcher_context_active = false;'" in t['selfcheck']:
        fail('stale v18.1 font assertion remains in source self-check')
    if "'ConfigureLauncherFont();'" not in t['selfcheck'] or "'v18.2: RT64 Inspector owns a fresh ImGui context'" not in t['selfcheck']:
        fail('v18.2 font assertions missing from source self-check')

    print('[OK] Rocket-R Graphics v18.2 compile hotfix verification PASS.')
    print('[OK] Renderer visibility/queue baseline preserved; overlay uses current singular font helper.')


if __name__ == '__main__':
    main()
