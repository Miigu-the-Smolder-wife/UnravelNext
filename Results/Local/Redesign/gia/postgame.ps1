# Post-game GPU verification queue of R (CLOUD_BRIEF "세션 13 이어서 (5)"): one lock hold per line, in order. Run only when
# .gpulock\HOLD is gone. Before starting: rebuild build\dev2 from the committed tree
#   powershell -File Tools\CI\Build.ps1 -Track dev2 -Tracks all
# A device removal (exit 87) stops the queue: report it to the coordinator.
$ErrorActionPreference = "Continue"
Set-Location 'C:\Users\USER\UnravelNext-redesign'
$jobs = @(
  @('acc1', 'lounge', '1920x1080', 'correctness'),   # new kernel first: accumulator pool, audit, GiAnalytic 9
  @('vsm2', 'x', '1920x1080', 'correctness'),        # per-view LOD bound: shadow layers, error bits, tests
  @('filter', 'lounge', '1920x1080', 'correctness'), # L_gi with the per-pass sigma: frames 1/4/16/299, rotation
  @('cold', 'lounge', '1920x1080', 'correctness'),   # bounce visibility / closure, each alone and both
  @('tests', 'x', '1920x1080', 'correctness'),       # regression with the defaults
  @('scene', 'train', '1920x1080', 'correctness'),
  @('scene', 'hall', '1920x1080', 'correctness'),
  @('scene', 'hall', '2560x1440', 'correctness'),
  @('acc2', 'x', '1920x1080', 'correctness'),        # bath 2000 frames x 4 (mean, SD)
  @('acc3', 'lounge', '1920x1080', 'correctness'),
  @('vsmtime', 'lounge', '1920x1080', 'timing'),
  @('vsmtime', 'train', '1920x1080', 'timing'),
  @('timing', 'lounge', '2560x1440', 'timing'),
  @('timing', 'train', '1920x1080', 'timing')
)
foreach ($job in $jobs) {
  $env:PG_TURN = $job[0]; $env:PG_SCENE = $job[1]; $env:PG_RES = $job[2]
  $o = "Results\Local\Redesign\gia\pg_$($job[0])_$($job[1])_$($job[2]).out"
  powershell -NoProfile -ExecutionPolicy Bypass -File Tools\CI\GpuLock.ps1 -Track R -Kind $job[3] -- powershell -NoProfile -File Results\Local\Redesign\gia\postgame_slice.ps1 *> $o
  "$($job -join ' ') lock exit $LASTEXITCODE $(Get-Date -Format HH:mm:ss)"
  if ($LASTEXITCODE -eq 87) { 'STOP: device removed'; break }
}
