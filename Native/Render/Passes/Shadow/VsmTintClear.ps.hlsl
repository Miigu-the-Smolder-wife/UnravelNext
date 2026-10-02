// unx-kernel: ps_6_6 main
// s.vsm.tint.clear (shadow.vsm.translucent_tint; VsmTint.hlsli): the tint texels of the pages drawn this frame back to
// "no glass caster" - rgb 1, depth 0 - before the glass casters' raster multiplies into them. VsmClearPages.ms draws a
// quad per page of a list.
float4 main() : SV_Target0 { return float4(1, 1, 1, 0); }
