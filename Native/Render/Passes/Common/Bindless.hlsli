// Root signature contract shared by every kernel (Device.cpp): 32 root constants at b0, a root CBV at b1,
// static samplers s0-s5, CBV/SRV/UAV and sampler heaps directly indexed (SM 6.6 ResourceDescriptorHeap[]).
#ifndef UNX_BINDLESS_HLSLI
#define UNX_BINDLESS_HLSLI

cbuffer PassConstants : register(b0)
{
    uint4 P[12];  // Device::kRootConstantCount (48)
};

SamplerState g_pointClamp : register(s0);
SamplerState g_linearClamp : register(s1);
SamplerState g_linearWrap : register(s2);
SamplerState g_anisoWrap : register(s3);
SamplerComparisonState g_shadowCompare : register(s4);
SamplerState g_anisoClamp : register(s5);  // anisotropic 16, clamp (textures with scene::Texture::wrap = false)

#endif
