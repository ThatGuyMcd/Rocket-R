#!/usr/bin/env python3
from pathlib import Path
import argparse

SIGS = (
    '[[nodiscard]] float RocketRenderDepth(const ExpandedRenderEntry& entry)',
    '[[nodiscard]] bool RocketRenderOpaque(const ExpandedRenderEntry& entry)',
    'void RocketRenderHeapSort(std::vector<std::size_t>& order,',
    'void RocketBuildGlobalRenderOrder()',
    'extern "C" void rocket_render_queue_begin(std::uint8_t*, recomp_context*)',
)

def definition_count(text, marker):
    # These complete signatures occur only at definitions in this source.
    return text.count(marker)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', required=True)
    args = ap.parse_args()
    root = Path(args.repo).resolve()
    p = (root/'src/presentation_identity.cpp').read_text(encoding='utf-8-sig')
    for sig in SIGS:
        count = definition_count(p, sig)
        if count != 1:
            raise RuntimeError(f'Expected exactly one definition/signature for {sig!r}; found {count}')
    for token in ('rocket_render_queue_finalize_capture','g_expanded_render_order',
                  'g_expanded_render_opaque_count','g_render_queue_final_pass'):
        if token not in p:
            raise RuntimeError('Required v19 queue token missing: ' + token)
    c = (root/'src/widescreen_culling.cpp').read_text(encoding='utf-8-sig')
    g = (root/'src/graphics_enhancements.cpp').read_text(encoding='utf-8-sig')
    if 'kNoSideCullRadiusBits = 0x7F7FFFFFU' not in c:
        raise RuntimeError('v19 distance-only side-cull policy missing')
    if 'draw_distance_multiplier' not in g or 'context->r7' not in g:
        raise RuntimeError('Draw Distance slider r7 path missing')
    print('[OK] Rocket-R Graphics v19.3.2 duplicate-helper repair verification PASS.')
    print('[OK] Exactly one global-order helper set remains; v19 visibility and Draw Distance policy preserved.')

if __name__ == '__main__':
    main()
