# Rocket-R custom final-VI shaders

Rocket-R Graphics v9 can replace the final RT64 Video Interface pixel shader with a custom, **single-pass precompiled** pixel shader.

The launcher scans the Rocket-R configuration directory's `shaders` folder for matching shader stems. For a shader named `my-filter`:

- D3D12 loads `my-filter.dxil`.
- Vulkan loads `my-filter.spv`.

A matching binary for the active backend is required. The shader is loaded when RT64 starts, so changing the selected custom shader requires restarting the game from the launcher.

## Shader contract

The custom shader reuses RT64's full-screen vertex shader, descriptor bindings and the Rocket-R Video Interface push constants. It must expose:

- `Texture2D<float4> gInput : register(t1)`
- `SamplerState gSampler : register(s2)`
- `PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_TARGET`
- the `VideoInterfaceCB` layout shown in `example-rocket-postprocess.hlsl`

The full-screen triangle supplies UVs in the same form as RT64's normal Video Interface shader. `videoResolution / textureResolution` is the lower-right UV of the valid N64 image inside the backing texture.

## Compiling with DXC

From a Developer Command Prompt or any shell containing `dxc`:

### D3D12 / DXIL

```text
dxc -T ps_6_0 -E PSMain -Qstrip_debug -Qstrip_reflect -Fo my-filter.dxil my-filter.hlsl
```

### Vulkan / SPIR-V

```text
dxc -T ps_6_0 -E PSMain -spirv -fvk-use-dx-layout -Fo my-filter.spv my-filter.hlsl
```

Copy the resulting binary/binaries into the `shaders` folder shown by Rocket-R's Graphics page, choose **Custom shader**, select the stem, and restart the game.

This v9 interface is deliberately single-pass and precompiled. It avoids adding a second runtime shader compiler to Linux/Android and keeps the renderer failure mode safe: if the selected backend binary is unavailable or invalid, RT64 logs the problem and uses its built-in VI path instead.
