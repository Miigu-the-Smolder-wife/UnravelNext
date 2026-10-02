# The one hardware run of the Unreal-structure renderer (Docs/Status/UE6_PORT_STATUS_KO.md 5): the four game scenes on
# the game path (temporal upscale, auto exposure), per output resolution
#   pictures  the camera still for 600 frames (the world-space caches fill as they are in play: the first run showed the
#             mesh cards' bounces still rising at frame 300 after a cold start), a cut at frame 600 into a view turned by
#             90 degrees, then turning at 20 deg/s: captures of frame 599 (before the cut), 600, 601, 603 (the first frames
#             after the cut), 615, 660 and 719 (in motion), converted to PNG through the display rendering (pfm_to_png.py);
#   timings   600 frames turning at 20 deg/s and 600 frames still, per-pass GPU times in <out>\<scene>\<res>\timing_*.
# Every run goes through Tools\CI\GpuLock.ps1 and keeps its full log; a device removal (DXGI_ERROR_DEVICE_*) stops the
# script at once and names the log (UNX_DRED = 1 leaves the breadcrumbs in it).
#   powershell -File Tools\Verify\Run-Ue6Final.ps1 [-Scenes DIR] [-Out DIR] [-Resolutions 1080p,1440p,4K] [-Only bt_lobby]
#                                                  [-SkipTimings] [-SkipPictures] [-Layers gi,refl] [-Set k=v,k=v]
#                                                  [-Generated city_block,forest_thin] [-NoGame]
# -Generated adds scenegen's scenes by name (the gate generates them); -NoGame leaves the saved game scenes out.
# -Layers adds the main view's internal layers of the captured frames (the gate's --capture-layers) beside the final picture.
# The GPU lock's HOLD file (.gpulock\HOLD in the main checkout) must be gone: this script does not remove it.
param(
    [string]$Scenes = "C:\Users\USER\UnravelNext-refl\Cache\ReflJudge\scenes",
    [string]$Out = "Cache\Ue6Final",
    [string[]]$Resolutions = @("1080p", "1440p", "4K"),
    [string]$Only = "",
    [switch]$SkipTimings,
    [switch]$SkipPictures,
    [string[]]$Layers = @(),
    [string[]]$Set = @(),
    [string[]]$Generated = @(),
    [switch]$NoGame
)
$ErrorActionPreference = "Stop"
# (powershell -File passes "a,b" as one string)
$Resolutions = @($Resolutions | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$Layers = @($Layers | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$Set = @($Set | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$Generated = @($Generated | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
$exe = Join-Path $root "build\all\bin\unx_gate_shadow_renderergate.exe"
if (-not (Test-Path $exe)) { throw "build first: Tools\CI\Build.ps1 -Track all -Jobs 16" }
if (Test-Path "C:\Users\USER\UnravelNext\.gpulock\HOLD") { throw "the GPU lock is on HOLD (C:\Users\USER\UnravelNext\.gpulock\HOLD): no hardware runs until it is removed" }
$sets = @("gi.deterministic=true") + $Set
$setArgs = @()
foreach ($s in $sets) { $setArgs += @("--set", $s) }
# the runs' scenes: name (the output folder) and the gate's --scene argument
$entries = @()
if (-not $NoGame) {
    $files = Get-ChildItem -Path $Scenes -Filter *.unxscene | Where-Object { $Only -eq "" -or $_.BaseName -like "$Only*" }
    foreach ($f in $files) { $entries += @(@{ Name = ($f.BaseName -replace "_\d{8}_\d{4}$", ""); Arg = $f.FullName }) }
}
foreach ($gname in $Generated) { $entries += @(@{ Name = $gname; Arg = $gname }) }
if ($entries.Count -eq 0) { throw "no scene: no .unxscene in $Scenes and no -Generated name" }

function Invoke-Gate([string]$kind, [string]$log, [string[]]$gateArgs) {
    # The breadcrumbs cost GPU time (the device says "not for timings"): on for the pictures only.
    $env:UNX_DRED = if ($kind -eq "timing") { "0" } else { "1" }
    $all = @("-File", "Tools\CI\GpuLock.ps1", "-Track", "R", "-Kind", $kind, "--", $exe) + $gateArgs
    # The gate writes notes to stderr: with "Stop" PowerShell 5.1 turns the first such line into a terminating error.
    $ErrorActionPreference = "Continue"
    & powershell @all 2>&1 | ForEach-Object { "$_" } | Out-File -Encoding utf8 $log
    $code = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    $text = Get-Content $log -Raw
    if ($text -match "DEVICE_REMOVED|DEVICE_HUNG|DEVICE_RESET|device removed|device hung") {
        throw "device removal: $log"
    }
    # (a gate that ends with a failure - S's error bits, a budget - is reported and the next run follows: its log and
    # timings are kept)
    if ($code -ne 0) {
        Write-Host "   GATE FAILED ($code): $log"
        $script:failed += @($log)
    }
}
$failed = @()

foreach ($entry in $entries) {
    $name = $entry.Name
    $sceneArg = $entry.Arg
    foreach ($res in $Resolutions) {
        $dir = Join-Path $root (Join-Path $Out (Join-Path $name $res))
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        if (-not $SkipPictures) {
            Write-Host "== ${name} ${res}: pictures"
            $layerArgs = @()
            if ($Layers.Count -gt 0) { $layerArgs = @("--capture-layers", ((@("final") + $Layers) -join ",")) }
            Invoke-Gate "correctness" (Join-Path $dir "pictures.log") (@("--scene", $sceneArg, "--resolution", $res, "--frames", "720", "--warmup-frames", "0", "--auto-exposure",
                    "--path-rotate", "20", "--motion-start", "1000000", "--cut-at", "600:4.5",
                    "--capture-output", (Join-Path $dir "cut.pfm"), "--capture-frames", "599,600,601,603,615,660,719") + $layerArgs + $setArgs)
            & python Tools\Verify\pfm_to_png.py $dir | Out-Null
        }
        if (-not $SkipTimings) {
            Write-Host "== ${name} ${res}: timings (turning)"
            Invoke-Gate "timing" (Join-Path $dir "timing_turning.log") (@("--scene", $sceneArg, "--resolution", $res, "--frames", "600", "--auto-exposure",
                    "--path-rotate", "20", "--out", (Join-Path $dir "timing_turning")) + $setArgs)
            Write-Host "== ${name} ${res}: timings (still)"
            Invoke-Gate "timing" (Join-Path $dir "timing_still.log") (@("--scene", $sceneArg, "--resolution", $res, "--frames", "600", "--auto-exposure",
                    "--out", (Join-Path $dir "timing_still")) + $setArgs)
        }
    }
}
Write-Host "done: $Out"
if ($failed.Count -gt 0) {
    Write-Host "gates that ended with a failure:"
    $failed | ForEach-Object { Write-Host "  $_" }
    exit 1
}
