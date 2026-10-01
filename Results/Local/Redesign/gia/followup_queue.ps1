# After the deploy configuration check: the hall's off run on the re-exported scene, the level transient's cause, the two
# new switches (gi.hit_oriented_lights, gi.hit_accumulator_levels = 1) on the analytic tests and the lobby, then the frame
# cost of the deploy configuration per switch (timing lock). Binaries build\dev2 (837bb73). One lock hold per line.
$ErrorActionPreference = "Continue"
Set-Location 'C:\Users\USER\UnravelNext-redesign'
$env:PG_BIN = 'dev2'
$jobs = @(
  @('offnew', 'hall', '1920x1080', '', 'correctness'),
  @('modes', 'lounge', '1920x1080', 'visonly,t_nolocal,t_onebounce,t_notemporal,acc1', 'correctness'),
  @('acct', 'x', '1920x1080', '', 'correctness'),
  @('lobby', 'lobby', '1920x1080', 'oriented,acc1,oriented_acc1', 'correctness'),
  @('timing2', 'lounge', '1920x1080', 'full,nofilter,coldoff,nolayers,alloff', 'timing'),
  @('timing2', 'lobby', '1920x1080', 'full,alloff', 'timing')
)
foreach ($job in $jobs) {
  $env:PG_TURN = $job[0]; $env:PG_SCENE = $job[1]; $env:PG_RES = $job[2]; $env:PG_MODES = $job[3]; $env:PG_ROT = '0'; $env:PG_TESTS = ''
  $o = "Results\Local\Redesign\gia\fq_$($job[0])_$($job[1]).out"
  powershell -NoProfile -ExecutionPolicy Bypass -File Tools\CI\GpuLock.ps1 -Track R -Kind $job[4] -- powershell -NoProfile -File Results\Local\Redesign\gia\postgame_slice.ps1 *> $o
  "$($job[0]) $($job[1]) lock exit $LASTEXITCODE $(Get-Date -Format HH:mm:ss)"
  if ($LASTEXITCODE -eq 87) { 'STOP: device removed'; break }
}
