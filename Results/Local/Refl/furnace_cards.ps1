# Mesh-card surface cache energy check in the analytic white furnace (ReflectionAnalytic --furnace-only: closed room,
# Le = 1, rho = 0.5, every surface's radiance L = Le / (1 - rho) = 2): the mirror floor's traced reflection is the hit's
# value, which with reflection.lumen + surface_cache.mesh_cards is the hit's shading from the cards' irradiance.
# "M mean" is value / expected. The card set is the hand-made one (surface_cache.mesh_cards_test_set).
#   smoke  8 frames (first run of the kernels)
#   a  defaults      b  radiosity ray cap off      e  no radiosity (expected 0.5: the cards hold no light, only emission)
#   v  the read's valid view (g = 16 where a hit read a card, r = 16 where it read none)
# Arguments: run names to run only those. Run inside the GPU lock (Tools/CI/GpuLock.ps1 -Kind correctness).
$ErrorActionPreference = "Continue"
$root = "C:\Users\USER\UnravelNext-refl"
$exe = Join-Path $root "build\all\bin\unx_test_reflection_reflectionanalytic.exe"
$out = Join-Path $root "Results\Local\Refl\furnace_cards"
New-Item -ItemType Directory -Force $out | Out-Null
$base = @("--furnace-only", "--set", "reflection.lumen=true", "--set", "surface_cache.enabled=true", "--set", "surface_cache.mesh_cards=true",
          "--set", "surface_cache.mesh_cards_test_set=true", "--set", "reflection.lumen_tonemap_range=0",
          "--set", "reflection.lumen_max_ray_intensity=0", "--set", "reflection.lumen_disocclusion_tonemap=false", "--set", "reflection.lumen_screen_traces=false")
$runs = @(
  @{ name = "smoke"; args = @("--frames", "8") },
  @{ name = "a_default"; args = @() },
  @{ name = "b_nocap"; args = @("--set", "surface_cache.radiosity_max_ray_intensity=0") },
  @{ name = "e_noradiosity"; args = @("--set", "surface_cache.radiosity=false") },
  @{ name = "v_view"; args = @("--set", "reflection.lumen_surface_cache_view=true") }
)
$only = $args
if ($only.Count -gt 0) { $runs = @($runs | Where-Object { $only -contains $_.name }) }
foreach ($r in $runs) {
  $log = Join-Path $out ($r.name + ".log")
  & $exe @base @($r.args) *> $log
  $code = $LASTEXITCODE
  "{0}: exit {1}" -f $r.name, $code
  Select-String -Path $log -Pattern "removed|hung|DXGI_ERROR|card test set|D3D12 errors" | Select-Object -First 6 | ForEach-Object { "  " + $_.Line }
  Select-String -Path $log -Pattern "furnace \(value" | ForEach-Object { "  " + $_.Line }
  if (Select-String -Path $log -Pattern "DEVICE_REMOVED|DEVICE_HUNG|device removed" -Quiet) { "device removal: stopped"; exit 87 }
}
