# The one batch of hardware runs after a stretch of code work (Docs/Status/UE6_PORT_STATUS_KO.md 5): everything the code
# work needs answered, in one go, with one summary sheet - so that nothing is run piece by piece afterwards.
#   1  the furnace room, night and day outside, and the day room without probe occlusion (furnace.py: every stage's light
#      against its closed form; the day room's excess is what came through the walls);
#   2  the four game scenes: cut pictures at 1080p with the gi and direct layers (which stage a cut frame's blotch is in),
#      timings at 1080p, 1440p and 4K;
#   3  scenegen's scenes (outdoors, night, forest, water, interior): cut pictures and timings at 1080p, timings at 4K;
#   4  variants, in groups (-Variants high,fog,fogab,fogvol,far,clouds,thin,bandb,specks,nopass,grids; default all): the high tier's timings; the
#      height fog on (pictures, timings) and each of its parts off in turn; the far field off; the cloud layer with and
#      without its temporal accumulation; the frame without per-pass timestamps; the view-angle grids against pixel-sized.
# The summary (<out>\summary.txt): the furnace sheets, every timing run's GPU frame and largest pass groups, the gates
# that failed. A device removal stops the batch.
#   powershell -File Tools\Verify\Run-Ue6Batch.ps1 [-Out Cache\Ue6Batch] [-Skip furnace,game,generated,variants] [-Variants high,fog,...]
param(
    [string]$Out = "Cache\Ue6Batch",
    [string[]]$Skip = @(),
    [string]$GeneratedScenes = "city_block,forest_thin,waterside,interior,city_night,ridge_sunset,forest_combat",
    [string[]]$Variants = @("high", "fog", "fogab", "fogvol", "far", "clouds", "thin", "bandb", "specks", "nopass", "grids")
)
$ErrorActionPreference = "Stop"
$Skip = @($Skip | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$Variants = @($Variants | ForEach-Object { $_ -split "," } | Where-Object { $_ })
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
    # Each group answers one question; -Variants picks groups (default: all).
    function Final([string]$title, [string]$dir, [string[]]$more) {
        Invoke-Step $title (@("-File", "Tools\Verify\Run-Ue6Final.ps1", "-Out", "$Out\$dir") + $more)
    }
    $fogOn = "atmosphere.fog.enabled=true"
    if ($Variants -contains "high") {
        # the tiers at 4K: timings, and the cut pictures to judge what each gives up (epic's are the game group's at 1080p:
        # here at 4K too)
        Final "high tier (lobby)" "tier_high" @("-Only", "bt_lobby", "-Resolutions", "1080p,4K", "-SkipPictures", "-Set", "output.tier=high")
        Final "high tier (lobby 4K pictures)" "tier_high" @("-Only", "bt_lobby", "-Resolutions", "4K", "-SkipTimings", "-Set", "output.tier=high")
        Final "performance tier (lobby 4K)" "tier_performance" @("-Only", "bt_lobby", "-Resolutions", "4K", "-Set", "output.tier=performance")
        Final "epic tier (lobby 4K pictures)" "tier_epic" @("-Only", "bt_lobby", "-Resolutions", "4K", "-SkipTimings")
        # the upscale's kernel as the reference has it (an output pixel wide from the second frame after a cut)
        Final "performance tier, the reference's kernel rule (lobby 4K pictures)" "tier_performance_refkernel" @("-Only", "bt_lobby", "-Resolutions", "4K", "-SkipTimings",
            "-Set", "output.tier=performance,output.upscale_tsr_kernel_by_samples=false")
    }
    if ($Variants -contains "fog") {
        # the fog as it is by default when on: the sky through it, far shadows, the density's variation, the reflections' rays,
        # the lake's mirror
        Final "fog on (lobby)" "fog" @("-Only", "bt_lobby", "-Resolutions", "1080p", "-Set", $fogOn)
        Final "fog on (city block, night city, ridge, lake)" "fog" @("-NoGame", "-Generated", "city_block,city_night,ridge_sunset,waterside", "-Resolutions", "1080p", "-Set", $fogOn)
        Final "fog on, 4K (lobby, night city timings)" "fog" @("-Only", "bt_lobby", "-Resolutions", "4K", "-SkipPictures", "-Set", $fogOn)
    }
    if ($Variants -contains "fogab") {
        # one switch off at a time against the group above (pictures only)
        Final "fog: no far shadows (ridge)" "fog_nofarshadow" @("-NoGame", "-Generated", "ridge_sunset", "-Resolutions", "1080p", "-SkipTimings", "-Set", "$fogOn,atmosphere.fog.far_shadows=false")
        Final "fog: uniform density (night city)" "fog_nonoise" @("-NoGame", "-Generated", "city_night", "-Resolutions", "1080p", "-SkipTimings", "-Set", "$fogOn,atmosphere.fog.noise_amount=0")
        Final "fog: not on reflection rays (night city)" "fog_norays" @("-NoGame", "-Generated", "city_night", "-Resolutions", "1080p", "-SkipTimings", "-Set", "$fogOn,atmosphere.fog.on_rays=false")
        Final "fog: none in the mirror views (lake)" "fog_nomirror" @("-NoGame", "-Generated", "waterside", "-Resolutions", "1080p", "-SkipTimings", "-Set", "$fogOn,atmosphere.fog.secondary_views=false")
        Final "fog under the cloud layer (ridge, city block)" "fog_clouds" @("-NoGame", "-Generated", "ridge_sunset,city_block", "-Resolutions", "1080p", "-SkipTimings", "-Set", $fogOn,
            "-GateArgs", "--clouds 0.5")
    }
    if ($Variants -contains "fogvol") {
        # a local fog volume alone (ground mist in the street ahead of the camera, a box 16 x 5 x 60 m), and inside the height fog
        Final "local fog volume (night city)" "fog_volume" @("-NoGame", "-Generated", "city_night", "-Resolutions", "1080p",
            "-GateArgs", "--fog-volume -38,2.5,80,8,2.5,30,0.06,1,2")
        Final "local fog volume in the height fog (night city)" "fog_volume_in_fog" @("-NoGame", "-Generated", "city_night", "-Resolutions", "1080p", "-SkipTimings",
            "-Set", $fogOn, "-GateArgs", "--fog-volume -38,2.5,80,8,2.5,30,0.06,1,2")
    }
    if ($Variants -contains "far") {
        Final "far field off (ridge, city block, lake)" "far_off" @("-NoGame", "-Generated", "ridge_sunset,city_block,waterside", "-Resolutions", "1080p", "-Layers", "gi",
            "-Set", "lumen.radiance_cache_far_field=false")
        Final "far field on, the gi layer (ridge, city block)" "far_on" @("-NoGame", "-Generated", "ridge_sunset,city_block", "-Resolutions", "1080p", "-Layers", "gi", "-SkipTimings")
    }
    if ($Variants -contains "clouds") {
        Final "clouds (ridge, city block)" "clouds" @("-NoGame", "-Generated", "ridge_sunset,city_block", "-Resolutions", "1080p", "-GateArgs", "--clouds 0.5")
        Final "clouds without temporal accumulation (ridge, city block)" "clouds_notemporal" @("-NoGame", "-Generated", "ridge_sunset,city_block", "-Resolutions", "1080p",
            "-GateArgs", "--clouds 0.5", "-Set", "atmosphere.clouds.temporal=false")
        Final "clouds 4K (ridge timings)" "clouds" @("-NoGame", "-Generated", "ridge_sunset", "-Resolutions", "4K", "-SkipPictures", "-GateArgs", "--clouds 0.5")
        Final "clouds with the whole sun path marched (ridge, city block)" "clouds_sunexact" @("-NoGame", "-Generated", "ridge_sunset,city_block", "-Resolutions", "1080p",
            "-GateArgs", "--clouds 0.5", "-Set", "atmosphere.clouds.sun_steps=0")
    }
    if ($Variants -contains "thin") {
        # thin geometry: the LOD that keeps the area off (the clusters as they were), and the fuller clustering of
        # disconnected geometry on top of it (visibility.toml: one hierarchy per grass clump instead of one per orientation)
        Final "thin geometry LOD off (forest, lake)" "thin_off" @("-NoGame", "-Generated", "forest_thin,waterside", "-Resolutions", "1080p",
            "-Set", "visibility.lod_thin_preserve_area=false")
        Final "fuller clusters of disconnected geometry (forests, lake)" "thin_fill" @("-NoGame", "-Generated", "forest_thin,forest_combat,waterside", "-Resolutions", "1080p",
            "-Set", "visibility.cluster_vertices=128,visibility.cluster_min_triangles=64,visibility.sheet_orientation_min_width=0.016")
    }
    if ($Variants -contains "bandb") {
        # the coverage layer's share of thin geometry: band B starts under 1.5 px by default; with the temporal upscale's
        # jitter the visibility buffer can take more of it (what the coverage layer then costs, and what the picture loses
        # in motion: the turning frames)
        Final "band A down to 0.75 px (lake, train lounge)" "band_a_075" @("-NoGame", "-Generated", "waterside", "-Resolutions", "1080p", "-Set", "visibility.band_a_min_width_px=0.75,visibility.band_a_hysteresis_px=1.0")
        Final "band A down to 0.75 px (train lounge)" "band_a_075" @("-Only", "te_lounge", "-Resolutions", "1080p", "-Set", "visibility.band_a_min_width_px=0.75,visibility.band_a_hysteresis_px=1.0")
        Final "band A down to 0.4 px (lake)" "band_a_040" @("-NoGame", "-Generated", "waterside", "-Resolutions", "1080p", "-Set", "visibility.band_a_min_width_px=0.4,visibility.band_a_hysteresis_px=0.6")
        Final "band A down to 0.4 px (train lounge)" "band_a_040" @("-Only", "te_lounge", "-Resolutions", "1080p", "-Set", "visibility.band_a_min_width_px=0.4,visibility.band_a_hysteresis_px=0.6")
    }
    if ($Variants -contains "specks") {
        # shading_ball: dark facet-sized specks on the spheres (seen 2026-10-02). One switch at a time says which light
        # path makes them: the sampled local lights' shadow rays (their bias, or the path off), the visible LOD (a finer
        # cut lies nearer the traced surface), the sun's shadow map (its receiver bias).
        $ball = @("-File", "Tools\Verify\Run-Ue6Still.ps1", "-Scene", "shading_ball", "-Frames", "300", "-Capture", "299")
        Invoke-Step "specks: as it is" ($ball + @("-Name", "specks_base"))
        Invoke-Step "specks: local lights without sampling" ($ball + @("-Name", "specks_noml", "-Set", "shading.mega_lights=false"))
        Invoke-Step "specks: shadow rays' normal bias 5 mm" ($ball + @("-Name", "specks_raybias", "-Set", "shading.mega_lights_ray_normal_bias_m=0.005"))
        Invoke-Step "specks: visible LOD error 0.25 px" ($ball + @("-Name", "specks_lod", "-Set", "visibility.lod_error_px=0.25"))
        Invoke-Step "specks: sun receiver bias 3 texels" ($ball + @("-Name", "specks_sunbias", "-Set", "shadow.vsm.receiver_bias_texels=3"))
    }
    if ($Variants -contains "nopass") {
        Final "the frame without per-pass timestamps (lobby 1080p, 4K)" "nopass" @("-Only", "bt_lobby", "-Resolutions", "1080p,4K", "-SkipPictures", "-GateArgs", "--no-pass-timestamps")
    }
    if ($Variants -contains "grids") {
        # the froxel tiles and the translucency volume's cells at their pixel sizes of before (what the angular sizes save at 4K)
        Final "pixel-sized grids (lobby 4K timings)" "grids_px" @("-Only", "bt_lobby", "-Resolutions", "4K", "-SkipPictures",
            "-Set", "atmosphere.froxels.tile_reference_height=0,lumen.translucency_volume_grid_reference_height=0")
        Final "pixel-sized grids (lobby 4K pictures)" "grids_px" @("-Only", "bt_lobby", "-Resolutions", "4K", "-SkipTimings",
            "-Set", "atmosphere.froxels.tile_reference_height=0,lumen.translucency_volume_grid_reference_height=0")
        Final "angular grids (lobby 4K pictures)" "grids_angle" @("-Only", "bt_lobby", "-Resolutions", "4K", "-SkipTimings")
    }
}
& python Tools\Verify\batch_summary.py $outDir (Join-Path $root "Cache\Ue6Diag") | Out-File -Encoding utf8 (Join-Path $outDir "summary.txt")
"batch finished $(Get-Date -Format s)" | Out-File -Encoding utf8 -Append $batchLog
Write-Host "done: $outDir\summary.txt"
