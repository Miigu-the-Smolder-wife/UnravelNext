# The one batch of hardware runs after a stretch of code work (Docs/Status/UE6_PORT_STATUS_KO.md 5): everything the code
# work needs answered, in one go, with one summary sheet - so that nothing is run piece by piece afterwards.
#   1  the furnace room, night and day outside, and the day room without probe occlusion (furnace.py: every stage's light
#      against its closed form; the day room's excess is what came through the walls);
#   2  the four game scenes: cut pictures at 1080p with the gi and direct layers (which stage a cut frame's blotch is in),
#      timings at 1080p, 1440p and 4K;
#   3  scenegen's scenes (outdoors, night, forest, water, interior): cut pictures and timings at 1080p, timings at 4K;
#   4  the high tier's timings (lobby, 1080p and 4K); the height fog on (lobby, city block, night city, ridge: pictures
#      and timings); the far field off (ridge, city block, lake: what the far rays add and cost); the cloud layer at
#      coverage 0.5 with and without its temporal accumulation (ridge, city block).
# The summary (<out>\summary.txt): the furnace sheets, every timing run's GPU frame and largest pass groups, the gates
# that failed. A device removal stops the batch.
#   powershell -File Tools\Verify\Run-Ue6Batch.ps1 [-Out Cache\Ue6Batch] [-Skip furnace,game,generated,variants]
param(
    [string]$Out = "Cache\Ue6Batch",
    [string[]]$Skip = @(),
    [string]$GeneratedScenes = "city_block,forest_thin,waterside,interior,city_night,ridge_sunset,forest_combat"
)
$ErrorActionPreference = "Stop"
$Skip = @($Skip | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
$outDir = Join-Path $root $Out
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$batchLog = Join-Path $outDir "batch.log"
"batch started $(Get-Date -Format s)" | Out-File -Encoding utf8 $batchLog

function Invoke-Step([string]$title, [string[]]$stepArgs) {
    "== $title" | Out-File -Encoding utf8 -Append $batchLog
    Write-Host "== $title"
    $ErrorActionPreference = "Continue"
    & powershell -NoProfile @stepArgs 2>&1 | ForEach-Object { "$_" } | Out-File -Encoding utf8 -Append $batchLog
    $ErrorActionPreference = "Stop"
    if ((Get-Content $batchLog -Raw) -match "device removal") { throw "device removal in '$title': $batchLog" }
}

if ($Skip -notcontains "furnace") {
    $layers = "gi,carddirect,cardindirect"
    # (the fog is on in the furnace runs: it is the medium that shows what the air volume and the translucency volume
    # let through the walls)
    $fog = "atmosphere.fog.enabled=true"
    Invoke-Step "furnace room (night outside)" @("-File", "Tools\Verify\Run-Ue6Still.ps1", "-Name", "batch_furnace", "-Scene", "furnace_room", "-Layers", $layers, "-Set", $fog)
    Invoke-Step "furnace room (day outside)" @("-File", "Tools\Verify\Run-Ue6Still.ps1", "-Name", "batch_furnace_day", "-Scene", "furnace_room_day", "-Layers", $layers, "-Set", $fog)
    Invoke-Step "furnace room (day outside, no probe occlusion)" @("-File", "Tools\Verify\Run-Ue6Still.ps1", "-Name", "batch_furnace_day_noocc", "-Scene", "furnace_room_day",
        "-Layers", $layers, "-Set", "$fog,lumen.radiance_cache_probe_occlusion=false")
    # what the fog adds to the day room, and which of its terms: no fog; the fog without its indirect light; the fog
    # sampled along the whole slice (the part past the wall too)
    Invoke-Step "furnace room (day outside, no fog)" @("-File", "Tools\Verify\Run-Ue6Still.ps1", "-Name", "batch_furnace_day_nofog", "-Scene", "furnace_room_day",
        "-Layers", $layers, "-Set", "atmosphere.fog.enabled=false")
    Invoke-Step "furnace room (day outside, fog without indirect light)" @("-File", "Tools\Verify\Run-Ue6Still.ps1", "-Name", "batch_furnace_day_fognoamb", "-Scene", "furnace_room_day",
        "-Layers", $layers, "-Set", "$fog,atmosphere.fog.indirect_light=false")
    Invoke-Step "furnace room (day outside, slices not clipped at the surface)" @("-File", "Tools\Verify\Run-Ue6Still.ps1", "-Name", "batch_furnace_day_noclip", "-Scene", "furnace_room_day",
        "-Layers", $layers, "-Set", "$fog,atmosphere.froxels.clip_at_surface=false")
    Invoke-Step "furnace room (day outside, the reference's depth offset threshold)" @("-File", "Tools\Verify\Run-Ue6Still.ps1", "-Name", "batch_furnace_day_ltvref", "-Scene", "furnace_room_day",
        "-Layers", $layers, "-Set", "$fog,lumen.translucency_volume_depth_offset_threshold=1.0")
}
if ($Skip -notcontains "game") {
    Invoke-Step "game scenes 1080p (pictures with layers, timings)" @("-File", "Tools\Verify\Run-Ue6Final.ps1", "-Out", $Out, "-Resolutions", "1080p", "-Layers", "gi,direct")
    Invoke-Step "game scenes 1440p, 4K (timings)" @("-File", "Tools\Verify\Run-Ue6Final.ps1", "-Out", $Out, "-Resolutions", "1440p,4K", "-SkipPictures")
}
if ($Skip -notcontains "generated") {
    Invoke-Step "generated scenes 1080p (pictures, timings)" @("-File", "Tools\Verify\Run-Ue6Final.ps1", "-Out", $Out, "-NoGame", "-Generated", $GeneratedScenes, "-Resolutions", "1080p")
    Invoke-Step "generated scenes 4K (timings)" @("-File", "Tools\Verify\Run-Ue6Final.ps1", "-Out", $Out, "-NoGame", "-Generated", $GeneratedScenes, "-Resolutions", "4K", "-SkipPictures")
}
if ($Skip -notcontains "variants") {
    Invoke-Step "high tier (lobby timings)" @("-File", "Tools\Verify\Run-Ue6Final.ps1", "-Out", "$Out\tier_high", "-Only", "bt_lobby", "-Resolutions", "1080p,4K", "-SkipPictures",
        "-Set", "output.tier=high")
    Invoke-Step "fog on (lobby)" @("-File", "Tools\Verify\Run-Ue6Final.ps1", "-Out", "$Out\fog", "-Only", "bt_lobby", "-Resolutions", "1080p",
        "-Set", "atmosphere.fog.enabled=true")
    Invoke-Step "fog on (city block, night city, ridge)" @("-File", "Tools\Verify\Run-Ue6Final.ps1", "-Out", "$Out\fog", "-NoGame", "-Generated", "city_block,city_night,ridge_sunset",
        "-Resolutions", "1080p", "-Set", "atmosphere.fog.enabled=true")
    Invoke-Step "far field off (ridge, city block, lake)" @("-File", "Tools\Verify\Run-Ue6Final.ps1", "-Out", "$Out\far_off", "-NoGame", "-Generated", "ridge_sunset,city_block,waterside",
        "-Resolutions", "1080p", "-Layers", "gi", "-Set", "lumen.radiance_cache_far_field=false")
    Invoke-Step "far field on, the gi layer (ridge, city block)" @("-File", "Tools\Verify\Run-Ue6Final.ps1", "-Out", "$Out\far_on", "-NoGame", "-Generated", "ridge_sunset,city_block",
        "-Resolutions", "1080p", "-Layers", "gi", "-SkipTimings")
    Invoke-Step "clouds (ridge, city block)" @("-File", "Tools\Verify\Run-Ue6Final.ps1", "-Out", "$Out\clouds", "-NoGame", "-Generated", "ridge_sunset,city_block",
        "-Resolutions", "1080p", "-GateArgs", "--clouds 0.5")
    Invoke-Step "clouds without temporal accumulation (ridge, city block)" @("-File", "Tools\Verify\Run-Ue6Final.ps1", "-Out", "$Out\clouds_notemporal", "-NoGame",
        "-Generated", "ridge_sunset,city_block", "-Resolutions", "1080p", "-GateArgs", "--clouds 0.5", "-Set", "atmosphere.clouds.temporal=false")
    Invoke-Step "clouds 4K (ridge timings)" @("-File", "Tools\Verify\Run-Ue6Final.ps1", "-Out", "$Out\clouds", "-NoGame", "-Generated", "ridge_sunset",
        "-Resolutions", "4K", "-SkipPictures", "-GateArgs", "--clouds 0.5")
}
& python Tools\Verify\batch_summary.py $outDir (Join-Path $root "Cache\Ue6Diag") | Out-File -Encoding utf8 (Join-Path $outDir "summary.txt")
"batch finished $(Get-Date -Format s)" | Out-File -Encoding utf8 -Append $batchLog
Write-Host "done: $outDir\summary.txt"
