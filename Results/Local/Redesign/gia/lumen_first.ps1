$ErrorActionPreference = "Continue"
# gi.lumen: first run of the new kernels inside the GPU lock (no TDR, no errors, picture not black). Lounge 1080p,
# 40 frames, final + GI layer at frames 1, 16, 39. PG_SCENE (lounge | lobby | train), PG_TAG (output name).
Set-Location 'C:\Users\USER\UnravelNext-redesign'
$bin = 'C:\Users\USER\UnravelNext-redesign\build\dev2\bin'
$out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\lumen'
New-Item -ItemType Directory -Force $out | Out-Null
$scenes = @{ lounge = 'C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lounge_reference.unxscene'; lobby = 'C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lobby.unxscene'; train = 'C:\Users\USER\UnravelGames\TrainExorcist\Artifacts\Look\lounge_reference.unxscene' }
$k = if ($env:PG_SCENE) { $env:PG_SCENE } else { 'lounge' }
$tag = if ($env:PG_TAG) { $env:PG_TAG } else { 'first' }
$extra = if ($env:PG_SETS) { $env:PG_SETS -split ',' | ForEach-Object { '--set'; $_ } } else { @() }
$more = if ($env:PG_ARGS) { $env:PG_ARGS -split ' ' } else { @() }  # e.g. --capture-output: the game's path (internal resolution + temporal upscale)
$frames = if ($env:PG_FRAMES) { $env:PG_FRAMES } else { '40' }; $captures = if ($env:PG_CAPTURES) { $env:PG_CAPTURES } else { '1,16,39' }
$log = Join-Path $out "${tag}_$k.log"
$sw = [Diagnostics.Stopwatch]::StartNew()
& (Join-Path $bin 'unx_gate_shadow_renderergate.exe') (@('--scene', $scenes[$k], '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', $frames, '--capture-frames', $captures, '--capture-layers', 'final,gi', $(if ($env:PG_OUTPUT -eq '1') { '--capture-output' } else { '--capture' }), "$out\${tag}_$k.pfm", '--set', 'gi.lumen=true') + $extra + $more) *> $log
$code = $LASTEXITCODE
$hung = [bool](Select-String -Path $log -Pattern "DEVICE_(HUNG|REMOVED|RESET)|887A0005|887A0006")
"${tag}_$k exit $code $([int]$sw.Elapsed.TotalSeconds) s"
if ($hung) { "STOP device removed"; exit 87 }
