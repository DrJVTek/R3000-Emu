# UE5 Custom Material Code

Ce document donne deux snippets `Custom` node pour Unreal:

- un décodeur `display plane` pour `Image/Video` en `15-bit` ou `24-bit`
- un décodeur `GPU primitive` pour `4bpp / 8bpp / 15bpp` avec `CLUT`

Contexte code:

- le contrat des UV du renderer GPU 2D est décrit dans [`integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Public/R3000GpuComponent.h`](../integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Public/R3000GpuComponent.h)
- `VramTexture` stocke les mots VRAM PS1 bruts dans une texture UE `PF_B8G8R8A8`
- encodage de `VramTexture`:
  - `B` = octet bas du mot VRAM
  - `G` = octet haut du mot VRAM
  - `R` = `0`
  - `A` = `255`
- cette conversion est faite dans [`integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Private/R3000VramViewerComponent.cpp`](../integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Private/R3000VramViewerComponent.cpp)

## 1. Plane `Image` / `Video` en 15-bit ou 24-bit

Usage:

- matériau `Unlit`
- `Emissive Color = Custom.rgb`
- `Opacity Mask = Custom.a` si `Masked`
- ou `Opacity = Custom.a` si `Translucent`

Inputs du `Custom` node:

- `VramTexture` : `TextureObject`
- `UV` : `float2`
- `DisplayX` : `float`
- `DisplayY` : `float`
- `DisplayW` : `float`
- `DisplayH` : `float`
- `Is24Bit` : `float`
- `UseMaskBitAsAlpha` : `float`

Output type:

- `CMOT Float4`

Code:

```hlsl
static const float2 kVramSize = float2(1024.0, 512.0);
static const float kRowStrideBytes = 2048.0;

float4 SampleRaw(float2 vramPx)
{
    float2 uv = (vramPx + 0.5) / kVramSize;
    return Texture2DSample(VramTexture, VramTextureSampler, uv);
}

uint SampleWord(float2 vramPx)
{
    float4 s = SampleRaw(vramPx);
    uint lo = (uint)round(s.b * 255.0);
    uint hi = (uint)round(s.g * 255.0);
    return lo | (hi << 8);
}

float3 Decode555(uint w)
{
    float r = (float)(w & 31u) / 31.0;
    float g = (float)((w >> 5) & 31u) / 31.0;
    float b = (float)((w >> 10) & 31u) / 31.0;
    return float3(r, g, b);
}

float2 ClampPixel(float2 p, float2 sizePx)
{
    return clamp(p, float2(0.0, 0.0), sizePx - 1.0);
}

float2 pixel = ClampPixel(floor(UV * float2(DisplayW, DisplayH)), float2(DisplayW, DisplayH));

if (Is24Bit > 0.5)
{
    float baseByte = DisplayY * kRowStrideBytes + DisplayX * 2.0;
    float pixelByte = pixel.y * kRowStrideBytes + pixel.x * 3.0;
    float byteIndex = baseByte + pixelByte;

    float wordIndex0 = floor(byteIndex * 0.5);
    float wordIndex1 = floor((byteIndex + 1.0) * 0.5);
    float wordIndex2 = floor((byteIndex + 2.0) * 0.5);

    float2 wordPx0 = float2(fmod(wordIndex0, 1024.0), floor(wordIndex0 / 1024.0));
    float2 wordPx1 = float2(fmod(wordIndex1, 1024.0), floor(wordIndex1 / 1024.0));
    float2 wordPx2 = float2(fmod(wordIndex2, 1024.0), floor(wordIndex2 / 1024.0));

    uint w0 = SampleWord(wordPx0);
    uint w1 = SampleWord(wordPx1);
    uint w2 = SampleWord(wordPx2);

    uint b0 = (((uint)byteIndex + 0u) & 1u) == 0u ? (w0 & 0xFFu) : ((w0 >> 8) & 0xFFu);
    uint b1 = (((uint)byteIndex + 1u) & 1u) == 0u ? (w1 & 0xFFu) : ((w1 >> 8) & 0xFFu);
    uint b2 = (((uint)byteIndex + 2u) & 1u) == 0u ? (w2 & 0xFFu) : ((w2 >> 8) & 0xFFu);

    return float4((float)b0 / 255.0, (float)b1 / 255.0, (float)b2 / 255.0, 1.0);
}
else
{
    float2 vramPx = float2(DisplayX + pixel.x, DisplayY + pixel.y);
    uint w = SampleWord(vramPx);
    float a = 1.0;

    if (UseMaskBitAsAlpha > 0.5)
    {
        a = ((w & 0x8000u) != 0u) ? 1.0 : 0.0;
    }

    if ((w & 0x7FFFu) == 0u)
    {
        a = 0.0;
    }

    return float4(Decode555(w), a);
}
```

Notes:

- pour les composants actuels [`R3000VideoComponent.cpp`](../integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Private/R3000VideoComponent.cpp) et [`R3000ImageComponent.cpp`](../integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Private/R3000ImageComponent.cpp), ce `Custom` n'est pas obligatoire:
  - ils uploadent deja une texture `RGBA8` decodee
  - un `Texture Sample Parameter2D` simple suffit
- ce code devient utile si tu veux sampler `VramTexture` directement dans le matériau

## 2. Materiau GPU 2D: `4bpp / 8bpp / 15bpp`

Usage:

- matériau `Unlit`
- pour l'opaque: `Blend Mode = Masked`
- pour les semi modes: un matériau par mode, comme deja prevu par [`R3000GpuComponent.h`](../integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Public/R3000GpuComponent.h)

Inputs du `Custom` node:

- `VramTexture` : `TextureObject`
- `UV0` : `float2` depuis `TexCoord[0]`
- `UV1` : `float2` depuis `TexCoord[1]`
- `UV2` : `float2` depuis `TexCoord[2]`
- `UV3` : `float2` depuis `TexCoord[3]`
- `ShadeColor` : `float3` depuis `VertexColor.rgb`
- `UseMaskBitAsAlpha` : `float`

Output type:

- `CMOT Float4`

Code:

```hlsl
static const float2 kVramSize = float2(1024.0, 512.0);

float4 SampleRaw(float2 vramPx)
{
    float2 uv = (vramPx + 0.5) / kVramSize;
    return Texture2DSample(VramTexture, VramTextureSampler, uv);
}

uint SampleWord(float2 vramPx)
{
    float4 s = SampleRaw(vramPx);
    uint lo = (uint)round(s.b * 255.0);
    uint hi = (uint)round(s.g * 255.0);
    return lo | (hi << 8);
}

float3 Decode555(uint w)
{
    float r = (float)(w & 31u) / 31.0;
    float g = (float)((w >> 5) & 31u) / 31.0;
    float b = (float)((w >> 10) & 31u) / 31.0;
    return float3(r, g, b);
}

float AlphaFromWord(uint w, float UseMaskBitAsAlpha)
{
    if ((w & 0x7FFFu) == 0u)
        return 0.0;

    if (UseMaskBitAsAlpha > 0.5)
        return ((w & 0x8000u) != 0u) ? 1.0 : 0.0;

    return 1.0;
}

uint texMode = (uint)round(UV3.x);
uint flags = (uint)round(UV3.y);
bool isRawTexture = (flags & 0x8u) != 0u;

float3 outRgb = ShadeColor;
float outA = 1.0;

if (texMode == 0u)
{
    return float4(outRgb, outA);
}

uint texelWord = 0u;
float u = floor(UV0.x);
float v = floor(UV0.y);

if (texMode == 1u)
{
    float2 wordPx = float2(UV1.x + floor(u * 0.25), UV1.y + v);
    uint packed = SampleWord(wordPx);
    uint shift = ((uint)u & 3u) * 4u;
    uint index = (packed >> shift) & 0xFu;
    texelWord = SampleWord(float2(UV2.x + (float)index, UV2.y));
}
else if (texMode == 2u)
{
    float2 wordPx = float2(UV1.x + floor(u * 0.5), UV1.y + v);
    uint packed = SampleWord(wordPx);
    uint index = (((uint)u & 1u) == 0u) ? (packed & 0xFFu) : ((packed >> 8) & 0xFFu);
    texelWord = SampleWord(float2(UV2.x + (float)index, UV2.y));
}
else
{
    texelWord = SampleWord(float2(UV1.x + u, UV1.y + v));
}

float3 texRgb = Decode555(texelWord);
outA = AlphaFromWord(texelWord, UseMaskBitAsAlpha);
outRgb = isRawTexture ? texRgb : (texRgb * ShadeColor);

return float4(outRgb, outA);
```

Branchements recommandes:

- `Emissive Color = Custom.rgb`
- `Opacity Mask = Custom.a` pour le matériau opaque/masked
- `Opacity = Custom.a` pour les matériaux translucents

Notes:

- ce code gere les formats texture PS1 reels du bridge GPU:
  - `4bpp + CLUT`
  - `8bpp + CLUT`
  - `15bpp direct`
- le `24-bit` n'est pas un mode texture standard ici, mais un mode d'affichage framebuffer
- donc le `24-bit` se traite plutot avec le snippet de la section 1

## 3. Rappel simple

Si ton but est seulement:

- `ImageComponent`
- `VideoComponent`

alors le plus simple reste:

- un matériau `Unlit`
- un `Texture Sample Parameter2D`
- `RGB -> Emissive`

Le decode `15-bit / 24-bit` est deja fait en C++ dans:

- [`integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Private/R3000ImageComponent.cpp`](../integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Private/R3000ImageComponent.cpp)
- [`integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Private/R3000VideoComponent.cpp`](../integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Private/R3000VideoComponent.cpp)
