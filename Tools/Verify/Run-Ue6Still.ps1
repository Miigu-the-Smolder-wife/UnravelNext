# One still-camera run of a game scene with settings on top of the defaults: the final picture (and layers) of the named
# frames, and - for the lobby, whose path-traced reference is kept (unx_reference, host camera, 480x270, 4096 spp, EV 4) -
# the picture against the reference by blocks (ref_blocks.py). For energy questions (which cap or limit loses light), not
# for judging cuts or motion.
#   powershell -File Tools\Verify\Run-Ue6Still.ps1 -Name cap_off [-Scene bt_lobby] [-Set k=v,k=v] [-Frames 600]
#                                                  [-Capture 599] [-Layers gi] [-Resolution 1080p] [-GateArgs "--clouds 0.5,600,2600"]
# -GateArgs: extra gate arguments, space separated (as Run-Ue6Final.ps1's).
# -Scene furnace_room -Layers gi,carddirect,cardindirect: the closed room's energy balance, stage by stage (furnace.py).
# -Scene furnace_room_day: the same with the sun up outside - what a stage holds over its number came through the walls.
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [string]$Scene = "bt_lobby",
    [string[]]$Set = @(),
    [int]$Frames = 600,
    [string]$Capture = "599",
    [string[]]$Layers = @(),
    [string]$Resolution = "1080p",
    [string]$GateArgs = "",
    [string]$Scenes = "C:\Users\USER\UnravelNext-refl\Cache\ReflJudge\scenes",
    [string]$Reference = "C:\Users\USER\UnravelNext-refl\Cache\Reference\lobby\host_ev4_480x270_4096_7bfe882f2c4eb7f8_69c5228d43176e01"
)
$ErrorActionPreference = "Stop"
$Set = @($Set | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$Layers = @($Layers | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
$exe = Join-Path $root "build\all\bin\unx_gate_shadow_renderergate.exe"
if (Test-Path "C:\Users\USER\UnravelNext\.gpulock\HOLD") { throw "the GPU lock is on HOLD" }
# a saved game scene by its name's start, else a generated scene by name (city_block, forest_thin, ..., furnace_room)
$file = Get-ChildItem -Path $Scenes -Filter "$Scene*.unxscene" | Select-Object -First 1
$sceneArg = if ($file) { $file.FullName } else { $Scene }
$dir = Join-Path $root "Cache\Ue6Diag\$Name"
New-Item -ItemType Directory -Force -Path $dir | Out-Null
# (the list is not named gateArgs: PowerShell's names ignore case, and that is the parameter above)
$runArgs = @("--scene", $sceneArg, "--resolution", $Resolution, "--frames", "$Frames", "--warmup-frames", "0", "--auto-exposure",
    "--capture-output", (Join-Path $dir "still.pfm"), "--capture-frames", $Capture, "--set", "gi.deterministic=true")
if ($Layers.Count -gt 0) { $runArgs += @("--capture-layers", ((@("final") + $Layers) -join ",")) }
foreach ($s in $Set) { $runArgs += @("--set", $s) }
if ($GateArgs -ne "") { $runArgs += @($GateArgs -split " " | Where-Object { $_ }) }
$env:UNX_DRED = "1"
$log = Join-Path $dir "run.log"
$ErrorActionPreference = "Continue"
& powershell -NoProfile -File Tools\CI\GpuLock.ps1 -Track R -Kind correctness -- $exe @runArgs 2>&1 | ForEach-Object { "$_" } | Out-File -Encoding utf8 $log
$code = $LASTEXITCODE
$ErrorActionPreference = "Stop"
$text = Get-Content $log -Raw
if ($text -match "DEVICE_REMOVED|DEVICE_HUNG|DEVICE_RESET|device removed|device hung") { throw "device removal: $log" }
Write-Host "== $Name ($($Set -join ' ')): exit $code"
& python Tools\Verify\pfm_to_png.py $dir | Out-Null
if ($Scene -like "furnace_room*") { & python Tools\Verify\furnace.py $dir }
if ($Scene -eq "bt_lobby" -and $Resolution -eq "1080p") {
    foreach ($line in (Select-String -Path $log -Pattern "captured final .*frame (\d+), ev100 ([-0-9.]+)")) {
        $f = $line.Matches[0].Groups[1].Value
        $ev = $line.Matches[0].Groups[2].Value
        Write-Host "frame $f (ev100 $ev)"
        & python Tools\Verify\ref_blocks.py (Join-Path $dir "still_f$f.pfm") $ev "$Reference.pfm" --halves "$Reference.halfA.pfm" "$Reference.halfB.pfm" --reference-ev100 4
    }
}
