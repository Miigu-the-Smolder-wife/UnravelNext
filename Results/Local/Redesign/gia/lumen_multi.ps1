$ErrorActionPreference = "Continue"
# gi.lumen: several short runs of one scene inside one GPU lock hold. PG_SCENE, PG_BIN, PG_RUNS = "tag:set,set;tag:set"
# (each run: gi.lumen=true plus its sets), PG_OUTPUT=1: the upscaled path (--capture-output), PG_FRAMES, PG_CAPTURES,
# PG_LAYERS (default final,gi). Stops at a device removal (exit 87).
Set-Location 'C:\Users\USER\UnravelNext-redesign'
$bin = 'C:\Users\USER\UnravelNext-redesign\build\' + $(if ($env:PG_BIN) { $env:PG_BIN } else { 'dev2' }) + '\bin'
$out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\lumen'
$scenes = @{ lounge = 'C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lounge_reference.unxscene'; lobby = 'C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lobby.unxscene'; hall = 'C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\bath_reference.unxscene'; train = 'C:\Users\USER\UnravelGames\TrainExorcist\Artifacts\Look\lounge_reference.unxscene' }
$k = if ($env:PG_SCENE) { $env:PG_SCENE } else { 'lobby' }
$frames = if ($env:PG_FRAMES) { $env:PG_FRAMES } else { '40' }
$captures = if ($env:PG_CAPTURES) { $env:PG_CAPTURES } else { '1,16,39' }
$layers = if ($env:PG_LAYERS) { $env:PG_LAYERS } else { 'final,gi' }
$more = if ($env:PG_EXTRA) { $env:PG_EXTRA -split ' ' } else { @() }  # extra gate arguments, e.g. --path-rotate 90 --path-time 0 --cut-at 60:1.0
$capture = if ($env:PG_OUTPUT -eq '1') { '--capture-output' } else { '--capture' }
foreach ($run in ($env:PG_RUNS -split ';')) {
  $tag, $sets = $run -split ':', 2
  $extra = @()
  if ($sets) { foreach ($s in ($sets -split ',')) { $extra += @('--set', $s) } }
  $log = Join-Path $out "${tag}_$k.log"
  $sw = [Diagnostics.Stopwatch]::StartNew()
  & (Join-Path $bin 'unx_gate_shadow_renderergate.exe') (@('--scene', $scenes[$k], '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', $frames, '--capture-frames', $captures, '--capture-layers', $layers, $capture, "$out\${tag}_$k.pfm", '--set', 'gi.lumen=true') + $more + $extra) *> $log
  $code = $LASTEXITCODE
  $hung = [bool](Select-String -Path $log -Pattern "DEVICE_(HUNG|REMOVED|RESET)|887A0005|887A0006")
  "${tag}_$k exit $code $([int]$sw.Elapsed.TotalSeconds) s"
  if ($hung) { "STOP device removed in ${tag}_$k"; exit 87 }
}
