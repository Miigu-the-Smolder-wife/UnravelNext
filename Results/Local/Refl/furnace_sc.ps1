# Surface cache energy check in the analytic white furnace (ReflectionAnalytic --furnace-only: closed room, Le = 1,
# rho = 0.5, every surface's radiance L = Le / (1 - rho) = 2): the mirror floor's traced reflection is the hit's value,
# which with reflection.lumen + surface_cache.enabled is the cell's final lighting. "M mean" is value / expected.
#   a  the cache with its defaults      b  radiosity ray cap off      c  cap off, 1024 frames
#   d  no surface cache (the hit shading of before)      e  the cache without radiosity (expected 0.5: the cells hold no light, only emission)
# Arguments: run names to run only those.
# Run inside the GPU lock (Tools/CI/GpuLock.ps1 -Kind correctness).
$ErrorActionPreference = "Continue"
$root = "C:\Users\USER\UnravelNext-refl"
$exe = Join-Path $root "build\all\bin\unx_test_reflection_reflectionanalytic.exe"
$out = Join-Path $root "Results\Local\Refl\furnace_sc"
New-Item -ItemType Directory -Force $out | Out-Null
$base = @("--furnace-only", "--set", "reflection.lumen=true", "--set", "surface_cache.enabled=true", "--set", "reflection.lumen_tonemap_range=0",
          "--set", "reflection.lumen_max_ray_intensity=0", "--set", "reflection.lumen_disocclusion_tonemap=false", "--set", "reflection.lumen_screen_traces=false")
$runs = @(
  @{ name = "a_default"; args = @() },
  @{ name = "b_nocap"; args = @("--set", "surface_cache.radiosity_max_ray_intensity=0") },
  @{ name = "c_nocap_1024"; args = @("--set", "surface_cache.radiosity_max_ray_intensity=0", "--frames", "1024") },
  @{ name = "d_nosc"; args = @("--set", "surface_cache.enabled=false") },
  @{ name = "e_noradiosity"; args = @("--set", "surface_cache.radiosity=false") }
)
$only = $args
if ($only.Count -gt 0) { $runs = @($runs | Where-Object { $only -contains $_.name }) }
foreach ($r in $runs) {
  $log = Join-Path $out ($r.name + ".log")
  & $exe @base @($r.args) *> $log
  "{0}: exit {1}" -f $r.name, $LASTEXITCODE
  Select-String -Path $log -Pattern "removed|hung|DXGI_ERROR" | Select-Object -First 3 | ForEach-Object { "  " + $_.Line }
  Select-String -Path $log -Pattern "furnace \(value" | ForEach-Object { "  " + $_.Line }
}
