# Custom shaders

Rocket-R can use a precompiled shader for the final Video Interface image.
It supports one pass. Choose the shader in **Graphics > Image**, then restart
the game to load it.

Put shader files in the `shaders` folder inside Rocket-R's configuration folder.
Files with the same name are treated as one shader:

- `my-filter.dxil` for D3D12
- `my-filter.spv` for Vulkan

You need the file for the backend you're using. If it is missing or cannot be
loaded, RT64 logs the problem and uses its built-in shader.

## Shader interface

Start with [the example](custom-shaders/example-rocket-postprocess.hlsl).
Keep its `VideoInterfaceCB` layout, texture/sampler bindings and entry point:

```hlsl
Texture2D<float4> gInput : register(t1);
SamplerState gSampler : register(s2);
float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_TARGET;
```

The existing fullscreen vertex shader supplies the UV coordinates.
`videoResolution / textureResolution` gives the lower-right UV of the valid
N64 image within the backing texture.

## Compiling

Use a shell with DXC available. For D3D12:

```text
dxc -T ps_6_0 -E PSMain -Qstrip_debug -Qstrip_reflect -Fo my-filter.dxil my-filter.hlsl
```

For Vulkan:

```text
dxc -T ps_6_0 -E PSMain -spirv -fvk-use-dx-layout -Fo my-filter.spv my-filter.hlsl
```

Copy the compiled files into the configuration folder's `shaders` directory,
choose **Custom shader**, select the name and restart the game.
