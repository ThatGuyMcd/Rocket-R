#!/usr/bin/env python3
from __future__ import annotations
import argparse
from pathlib import Path

HELPERS = r'''float4 RocketDeband(float2 sampleUV, float4 center) {
    const float strength = saturate(gConstants.rocketDebandStrength);
    if (strength <= 0.0001f) {
        return center;
    }

    const float2 texel = 1.0f / gConstants.textureResolution;
    const float3 average = (
        SampleInput(sampleUV + float2(texel.x, 0.0f)).rgb +
        SampleInput(sampleUV - float2(texel.x, 0.0f)).rgb +
        SampleInput(sampleUV + float2(0.0f, texel.y)).rgb +
        SampleInput(sampleUV - float2(0.0f, texel.y)).rgb) * 0.25f;
    const float3 delta3 = abs(average - center.rgb);
    const float delta = max(delta3.r, max(delta3.g, delta3.b));
    // Blend only low-contrast neighbourhoods. Edges and texture detail stay
    // on the authored sample while visible 16-bit gradient steps are softened.
    const float threshold = 0.035f;
    const float lowContrast = saturate((threshold - delta) / threshold);
    center.rgb = lerp(center.rgb, average, lowContrast * strength * 0.65f);
    return center;
}

float3 RocketPostProcess(float2 sampleUV, float2 pixelPosition, float3 color) {
    const uint mode = gConstants.rocketPostProcessMode;
    const float strength = saturate(gConstants.rocketPostProcessStrength);
    if ((mode == 0U) || (mode >= 3U) || (strength <= 0.0001f)) {
        return color;
    }

    const float nativeY = sampleUV.y * gConstants.videoResolution.y;
    const float scanWave = 0.5f + 0.5f * cos(3.14159265359f * nativeY);
    const float scan = lerp(1.0f, 0.80f + 0.20f * scanWave, strength);
    color *= scan;

    if (mode == 2U) {
        const float2 lowerRight = gConstants.videoResolution /
                                  gConstants.textureResolution;
        const float2 screenUV = saturate(sampleUV / max(lowerRight, 0.0001f));
        const float2 centered = screenUV * 2.0f - 1.0f;
        const float vignette = saturate(1.0f - dot(centered, centered) *
                                        (0.16f * strength));
        const float mask = 0.95f + 0.05f *
            cos(pixelPosition.x * 2.09439510239f);
        color *= vignette * lerp(1.0f, mask, strength * 0.55f);
    }
    return color;
}
'''

PSMAIN = r'''float4 PSMain(in float4 pos : SV_Position, in float2 uv : TEXCOORD0) : SV_TARGET {
    const float2 sampleUV = (uv / gConstants.textureResolution) *
                            gConstants.videoResolution;
#ifdef PIXEL_ANTIALIASING
    float4 color = PixelAntialiasing(uv);
#else
    float4 color = SampleInput(sampleUV);
#endif
    color = RocketDeband(sampleUV, color);
    color.rgb = RocketPostProcess(sampleUV, pos.xy, color.rgb);
    color.a = 1.0f;
    return color;
}'''


def find_function_span(text: str, signature: str) -> tuple[int, int]:
    start = text.find(signature)
    if start < 0:
        raise RuntimeError(f'VI shader function not found: {signature}')
    brace = text.find('{', start)
    if brace < 0:
        raise RuntimeError(f'VI shader function opening brace not found: {signature}')
    depth = 0
    i = brace
    while i < len(text):
        ch = text[i]
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                return start, i + 1
        i += 1
    raise RuntimeError(f'VI shader function closing brace not found: {signature}')


def validate(text: str) -> None:
    required = [
        'float4 SampleInput(float2 uv)',
        'float4 PixelAntialiasing(float2 uv)',
        'float4 RocketDeband(float2 sampleUV, float4 center)',
        'float3 RocketPostProcess(float2 sampleUV, float2 pixelPosition, float3 color)',
        'float4 color = PixelAntialiasing(uv);',
        'float4 color = SampleInput(sampleUV);',
        'color = RocketDeband(sampleUV, color);',
        'color.rgb = RocketPostProcess(sampleUV, pos.xy, color.rgb);',
    ]
    for marker in required:
        if marker not in text:
            raise RuntimeError('VI shader transformation missing marker: ' + marker)
    if text.count('float4 RocketDeband(') != 1:
        raise RuntimeError('VI shader has duplicate RocketDeband definitions.')
    if text.count('float3 RocketPostProcess(') != 1:
        raise RuntimeError('VI shader has duplicate RocketPostProcess definitions.')
    if text.count('float4 PSMain(') != 1:
        raise RuntimeError('VI shader must contain exactly one PSMain definition.')


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', required=True)
    args = ap.parse_args()
    repo = Path(args.repo).resolve()
    shader = repo / 'src/shaders/VideoInterfacePS.hlsl'
    vi_header = repo / 'src/shared/rt64_video_interface.h'
    if not shader.is_file():
        raise RuntimeError('Pinned RT64 VI shader is missing: ' + str(shader))
    if not vi_header.is_file():
        raise RuntimeError('Pinned RT64 VI shared header is missing: ' + str(vi_header))

    header_text = vi_header.read_text(encoding='utf-8-sig')
    for marker in ['rocketViFilterMode', 'rocketDebandStrength', 'rocketPostProcessMode', 'rocketPostProcessStrength']:
        if marker not in header_text:
            raise RuntimeError('RT64 0013 core patch did not add VI constant: ' + marker)

    text = shader.read_text(encoding='utf-8-sig')
    # Idempotent path for an already transformed worktree.
    if 'float4 RocketDeband(' in text or 'float3 RocketPostProcess(' in text:
        validate(text)
        print('[OK] RT64 VI shader already has Rocket-R v9.4 transformation.')
        return 0

    if 'float4 PixelAntialiasing(float2 uv)' not in text or 'float4 SampleInput(float2 uv)' not in text:
        raise RuntimeError('Pinned RT64 VI shader shape is not recognised; refusing to modify it.')

    ps_start, ps_end = find_function_span(
        text, 'float4 PSMain(in float4 pos : SV_Position, in float2 uv : TEXCOORD0) : SV_TARGET')
    # Preserve all pinned shader code before PSMain, add project helpers, and
    # replace only PSMain. No line-number or git-patch context is involved.
    before = text[:ps_start].rstrip() + '\n\n'
    after = text[ps_end:].lstrip('\r\n')
    transformed = before + HELPERS.rstrip() + '\n\n' + PSMAIN + '\n'
    if after:
        transformed += after
    validate(transformed)
    shader.write_text(transformed, encoding='utf-8', newline='\n')
    print('[OK] Applied Rocket-R deterministic VI shader transformation v9.4.')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except Exception as exc:
        print('[ERROR] ' + str(exc))
        raise SystemExit(1)
