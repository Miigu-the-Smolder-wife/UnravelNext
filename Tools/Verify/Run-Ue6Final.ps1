# The one hardware run of the Unreal-structure renderer (Docs/Status/UE6_PORT_STATUS_KO.md 5): the four game scenes on
# the game path (temporal upscale, auto exposure), per output resolution
#   pictures  the camera still for 60 frames (the caches warm), a cut at frame 60 into a view turned by 90 degrees, then
#             turning at 20 deg/s: captures of frame 59 (warm), 60, 61, 63 (the first frames after the cut), 75, 120 and
#             179 (in motion), converted to PNG through the display rendering (pfm_to_png.py);
#   timings   600 frames turning at 20 deg/s and 600 frames still, per-pass GPU times in <out>\<scene>\<res>\timing_*.
# Every run goes through Tools\CI\GpuLock.ps1 and keeps its full log; a device removal (DXGI_ERROR_DEVICE_*) stops the
# script at once and names the log (UNX_DRED = 1 leaves the breadcrumbs in it).
#   powershell -File Tools\Verify\Run-Ue6Final.ps1 [-Scenes DIR] [-Out DIR] [-Resolutions 1080p,1440p,4K] [-Only bt_lobby]
#                                                  [-SkipTimings] [-Set k=v,k=v]
# The GPU lock's HOLD file (.gpulock\HOLD in the main checkout) must be gone: this script does not remove it.
param(
    [string]$Scenes = "C:\Users\USER\UnravelNext-refl\Cache\ReflJudge\scenes",
    [string]$Out = "Cache\Ue6Final",
    [string[]]$Resolutions = @("1080p", "1440p", "4K"),
    [string]$Only = "",
    [switch]$SkipTimings,
    [string[]]$Set = @()
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
$exe = Join-Path $root "build\all\bin\unx_gate_shadow_renderergate.exe"
if (-not (Test-Path $exe)) { throw "build first: Tools\CI\Build.ps1 -Track all -Jobs 16" }
if (Test-Path "C:\Users\USER\UnravelNext\.gpulock\HOLD") { throw "the GPU lock is on HOLD (C:\Users\USER\UnravelNext\.gpulock\HOLD): no hardware runs until it is removed" }
$env:UNX_DRED = "1"
$sets = @("gi.deterministic=true") + $Set
$setArgs = @()
foreach ($s in $sets) { $setArgs += @("--set", $s) }
$files = Get-ChildItem -Path $Scenes -Filter *.unxscene | Where-Object { $Only -eq "" -or $_.BaseName -like "$Only*" }
if (-not $files) { throw "no .unxscene in $Scenes" }

function Invoke-Gate([string]$kind, [string]$log, [string[]]$gateArgs) {
    $all = @("-File", "Tools\CI\GpuLock.ps1", "-Track", "R", "-Kind", $kind, "--", $exe) + $gateArgs
    & powershell @all *> $log
    $code = $LASTEXITCODE
    $text = Get-Content $log -Raw
    if ($text -match "DEVICE_REMOVED|DEVICE_HUNG|DEVICE_RESET|device removed|device hung") {
        throw "device removal: $log"
    }
    if ($code -ne 0) { throw "the gate failed ($code): $log" }
}

foreach ($file in $files) {
    $name = $file.BaseName -replace "_\d{8}_\d{4}$", ""
    foreach ($res in $Resolutions) {
        $dir = Join-Path $root (Join-Path $Out (Join-Path $name $res))
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        Write-Host "== $name $res: pictures"
        Invoke-Gate "correctness" (Join-Path $dir "pictures.log") (@("--scene", $file.FullName, "--resolution", $res, "--frames", "180", "--warmup-frames", "0", "--auto-exposure",
                "--path-rotate", "20", "--motion-start", "1000000", "--cut-at", "60:4.5",
                "--capture-output", (Join-Path $dir "cut.pfm"), "--capture-frames", "59,60,61,63,75,120,179") + $setArgs)
        & python Tools\Verify\pfm_to_png.py $dir | Out-Null
        if (-not $SkipTimings) {
            Write-Host "== $name $res: timings (turning)"
            Invoke-Gate "timing" (Join-Path $dir "timing_turning.log") (@("--scene", $file.FullName, "--resolution", $res, "--frames", "600", "--auto-exposure",
                    "--path-rotate", "20", "--out", (Join-Path $dir "timing_turning")) + $setArgs)
            Write-Host "== $name $res: timings (still)"
            Invoke-Gate "timing" (Join-Path $dir "timing_still.log") (@("--scene", $file.FullName, "--resolution", $res, "--frames", "600", "--auto-exposure",
                    "--out", (Join-Path $dir "timing_still")) + $setArgs)
        }
    }
}
Write-Host "done: $Out"
