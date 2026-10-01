# Deploy configuration check (coordinator, 2026-10-01 18:00): the defaults (accumulator off, GI reconstruction and cold
# start on, reflection layers on) on the game scenes and the regression tests, one lock hold per line. Binaries build\dev.
$ErrorActionPreference = "Continue"
Set-Location 'C:\Users\USER\UnravelNext-redesign'
$env:PG_BIN = 'dev'
$jobs = @(
  @('deploy', 'lounge', '1920x1080', '', ''),
  @('deploy', 'hall', '1920x1080', '', ''),
  @('deploy', 'train', '1920x1080', '', ''),
  @('tests', 'x', '1920x1080', '', ''),
  @('tests', 'y', '1920x1080', 'unx_test_shadow_vsmtests,unx_test_shadow_localshadowtests,unx_test_visibility_visibilitytests', ''),
  @('lobby', 'lobby', '1920x1080', '', 'full')
)
foreach ($job in $jobs) {
  $env:PG_TURN = $job[0]; $env:PG_SCENE = $job[1]; $env:PG_RES = $job[2]; $env:PG_TESTS = $job[3]; $env:PG_MODES = $job[4]; $env:PG_ROT = '1'
  $o = "Results\Local\Redesign\gia\dq_$($job[0])_$($job[1]).out"
  powershell -NoProfile -ExecutionPolicy Bypass -File Tools\CI\GpuLock.ps1 -Track R -Kind correctness -- powershell -NoProfile -File Results\Local\Redesign\gia\postgame_slice.ps1 *> $o
  "$($job[0]) $($job[1]) lock exit $LASTEXITCODE $(Get-Date -Format HH:mm:ss)"
  if ($LASTEXITCODE -eq 87) { 'STOP: device removed'; break }
}
