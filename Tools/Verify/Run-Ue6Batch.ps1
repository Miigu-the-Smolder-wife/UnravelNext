# The one batch of hardware runs after a stretch of code work (Docs/Status/UE6_PORT_STATUS_KO.md 5): everything the code
# work needs answered, in one go, with one summary sheet - so that nothing is run piece by piece afterwards.
#   1  the furnace room, night and day outside, and the day room without probe occlusion (furnace.py: every stage's light
#      against its closed form; the day room's excess is what came through the walls);
#   2  the four game scenes: cut pictures at 1080p with the gi and direct layers (which stage a cut frame's blotch is in),
#      timings at 1080p, 1440p and 4K;
#   3  scenegen's scenes (outdoors, night, forest, water, interior): cut pictures and timings at 1080p, timings at 4K;
#   4  variants, in groups (-Variants high,fog,fogab,fogvol,far,clouds,thin,bandb,specks,nopass,grids,ab; default all): the high tier's timings; the
#      height fog on (pictures, timings) and each of its parts off in turn; the far field off; the cloud layer with and
#      without its temporal accumulation; the frame without per-pass timestamps; the view-angle grids against pixel-sized;
#   5  ab (a variant group): every switch written since 2026-10-03 without a run, one at a time against the defaults, on
#      the scenes that exercise it ($abGroups below; -Ab picks groups, default all). Per scene of a group: the base run,
#      then one run per switch - ONE captured frame each (the camera still, or turning for the upscaler's switches) and,
#      where the group asks for it, a turning timing run. A variant's frame is compared with the base's at once
#      (ab_compare.py -> <out>\ab\pictures.txt) and deleted; the base's frame goes when its scene is done, so at most two
#      captures of the group are on disk at a time (the disk filled up once). The summary lists the differences and each
#      variant's GPU frame against its base's. Not in it, because the gate cannot make their inputs: the particles'
#      switches (fx.particles.soft / near_fade: the host's particle systems) and the instances' shadow flags.
# The summary (<out>\summary.txt): the furnace sheets, every timing run's GPU frame and largest pass groups, the A/B
# sheet, the gates that failed. A device removal stops the batch.
# Disk: the batch stops before it starts when the drive has under -MinFreeGB free; after the summary the captures' raw
# frames (*.pfm, 25 MB each at 1080p, 100 MB at 4K) under <out> and the furnace runs' are deleted unless -KeepRaw (the
# PNGs of the cut pictures stay).
#   powershell -File Tools\Verify\Run-Ue6Batch.ps1 [-Out Cache\Ue6Batch] [-Skip furnace,game,generated,variants] [-Variants high,fog,...]
#                                                  [-Ab cull,vsm,...] [-AbFrames 300] [-KeepRaw] [-MinFreeGB 30]
param(
    [string]$Out = "Cache\Ue6Batch",
    [string[]]$Skip = @(),
    [string]$GeneratedScenes = "city_block,forest_thin,waterside,interior,city_night,ridge_sunset,forest_combat",
    [string[]]$Variants = @("high", "fog", "fogab", "fogvol", "far", "clouds", "thin", "bandb", "specks", "nopass", "grids", "ab"),
    [string[]]$Ab = @(),          # the ab group's sub-groups by name (empty: all)
    [int]$AbFrames = 300,         # frames of an A/B run (the capture is its last frame; the timing run's length)
    [string]$Scenes = "C:\Users\USER\UnravelNext-refl\Cache\ReflJudge\scenes",  # the saved game scenes (as Run-Ue6Final.ps1)
    [switch]$KeepRaw,
    [int]$MinFreeGB = 30
)
$ErrorActionPreference = "Stop"
$Skip = @($Skip | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$Variants = @($Variants | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$Ab = @($Ab | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
$freeGB = [math]::Floor((Get-PSDrive -Name $root.Substring(0, 1)).Free / 1GB)
if ($freeGB -lt $MinFreeGB) { throw "only $freeGB GB free on $($root.Substring(0, 2)) (-MinFreeGB $MinFreeGB): make room before the batch" }
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
    if ($Variants -contains "ab") {
        # Every switch written since 2026-10-03 without a run (UE6_WORKPLAN_KO.md 8.1, 8.3; 2 (c); UE6_PORT_STATUS_KO.md 1.2,
        # 2.3.1), one at a time against the defaults. A group: the scenes that exercise its switches, what every run of
        # it sets besides (Base) and passes to the gate (Gate), whether the captured frame is taken while turning (Turn:
        # the temporal upscale's and the blur's switches) and whether a timing run is made (Time), the layers captured
        # beside the final picture, and its rows - N the variant's name, S what it sets (the other path of the switch), G
        # what it passes to the gate besides (a feature the frame's inputs switch on: a cirrus sheet, a fog volume, an HDR
        # display), E what the picture should do: "same" (the switch changes structure, not the picture: a difference is
        # a defect or noise), "differs" (the switch is the feature: the number says how much) or "unseen" (the stage lies
        # after the captured image - the output encoding: the timing alone says something).
        # The captured picture is the upscaler's output before the post chain (--capture-output); the chain layer is
        # what the chain takes - after the motion blur that follows the upscale.
        $abGroups = @(
            @{ Name = "cull"; Scenes = "city_block,forest_thin"; Time = $true; Rows = @(
                    @{ N = "queue_off"; S = "visibility.traversal_work_queue=false"; E = "same" },
                    @{ N = "merge_off"; S = "visibility.cull_pass_merge=false"; E = "same" },
                    @{ N = "fold_off"; S = "visibility.fold_small_passes=false,shadow.vsm.fold_small_passes=false,atmosphere.froxels.fold_small_passes=false,lumen.radiance_cache_fold_passes=false,surface_cache.mesh_cards_fold_passes=false"; E = "same" }) },
            # the sun's pages with moving casters (--moving) and the wind's trees: the cache's parts and the occlusion
            # the ray scene without its see-through instances' exclusion (glass in the GI and shadow rays again)
            @{ Name = "rays"; Scenes = "bt_lobby,te_lounge"; Time = $true; Layers = "gi,refl,direct"; Rows = @(
                    @{ N = "see_through_off"; S = "raytracing.see_through_translucent=false"; E = "differs" }) },
            @{ Name = "vsm"; Scenes = "city_block,city_night,forest_thin"; Gate = "--moving"; Time = $true; Layers = "shadow"; Rows = @(
                    @{ N = "separate_off"; S = "shadow.vsm.static_separate=false"; E = "same" },
                    @{ N = "hzbcull_off"; S = "shadow.vsm.static_hzb_cull=false"; E = "same" },
                    @{ N = "twophase_off"; S = "shadow.vsm.static_occlusion_two_phase=false"; E = "same" },
                    @{ N = "hzbfilter_off"; S = "shadow.vsm.cache_hzb_filter=false"; E = "same" },
                    @{ N = "coarse_off"; S = "shadow.vsm.coarse_pages=0,shadow.vsm.page_dilation=0"; E = "differs" },
                    @{ N = "contact_off"; S = "shadow.vsm.screen_ray_length=0"; E = "differs" }) },
            # the levels coarser than their casters: every caster drawn (the reference), left out, or as proxies (default)
            @{ Name = "forest"; Scenes = "forest_thin,forest_combat,waterside"; Time = $true; Layers = "shadow"; Rows = @(
                    @{ N = "every_caster"; S = "shadow.vsm.min_caster_texels=0"; E = "differs" },
                    @{ N = "small_left_out"; S = "shadow.vsm.aggregate_small_casters=false"; E = "differs" }) },
            @{ Name = "lights"; Scenes = "city_night,te_lounge"; Time = $true; Rows = @(
                    @{ N = "candidates_twice"; S = "atmosphere.froxels.candidates_once=false"; E = "same" },
                    @{ N = "head_sorted"; S = "atmosphere.froxels.sort_head_for_slots_only=false"; E = "differs" }) },
            # the local lights' pages: only without the sampled lights (S draws no local page with them)
            @{ Name = "local"; Scenes = "city_night,te_lounge"; Base = "shading.mega_lights=false"; Gate = "--moving"; Time = $true; Layers = "shadow"; Rows = @(
                    @{ N = "six_lights_a_request"; S = "shadow.vsm.local_request_views=252"; E = "same" },
                    @{ N = "local_separate_off"; S = "shadow.vsm.local_static_separate=false"; E = "same" }) },
            # glass casters (the generated scenes have none): the game scenes
            @{ Name = "tint"; Scenes = "bt_lobby,te_lounge"; Time = $true; Layers = "shadow"; Rows = @(
                    @{ N = "glass_opaque"; S = "shadow.vsm.translucent_tint=false"; E = "differs" }) },
            @{ Name = "cards"; Scenes = "bt_lobby,interior"; Time = $true; Layers = "gi,cardalbedo,cardfinal"; Rows = @(
                    @{ N = "capture_clusters"; S = "surface_cache.mesh_cards_capture_clusters=true"; E = "same" },
                    @{ N = "feedback_off"; S = "surface_cache.feedback=false"; E = "differs" },
                    @{ N = "feedback_gather"; S = "surface_cache.feedback_gather=true"; E = "differs" },
                    @{ N = "hit_indirect_off"; S = "lumen.hit_indirect=false"; E = "differs" }) },
            @{ Name = "coverage"; Scenes = "waterside,forest_thin"; Time = $true; Rows = @(
                    @{ N = "one_bucket"; S = "visibility.coverage_depth_buckets=1"; E = "same" },
                    @{ N = "triangle_cull_off"; S = "visibility.coverage_triangle_cull=false"; E = "same" },
                    @{ N = "keep_weightless"; S = "visibility.coverage_drop_weightless=false"; E = "same" },
                    @{ N = "compute_raster"; S = "visibility.coverage_compute_raster=true"; E = "same" },
                    @{ N = "composite_in_place"; S = "shading.coverage_compact=false"; E = "same" },
                    @{ N = "counters"; S = "visibility.coverage_statistics=true"; E = "same" }) },
            # the upscaler and the blur after it: a frame while the camera turns
            @{ Name = "tsr"; Scenes = "bt_lobby,waterside"; Turn = $true; Time = $true; Layers = "chain"; Rows = @(
                    @{ N = "kernel_reference"; S = "output.upscale_tsr_kernel_by_samples=false"; E = "differs" },
                    @{ N = "flickering_off"; S = "output.upscale_tsr_flickering=false"; E = "differs" },
                    @{ N = "reprojection_field_off"; S = "output.upscale_tsr_reprojection_field=false"; E = "differs" },
                    @{ N = "thin_geometry_off"; S = "output.upscale_tsr_thin_geometry=false"; E = "differs" },
                    @{ N = "resurrection"; S = "output.upscale_tsr_resurrection=true"; E = "differs" },
                    @{ N = "history_200"; S = "output.upscale_tsr_history_percent=200"; E = "differs" },
                    @{ N = "hole_filling_off"; S = "output.upscale_tsr_hole_filling=false"; E = "differs" },
                    @{ N = "thin_anti_flicker_off"; S = "output.upscale_tsr_thin_geometry_anti_flickering=false"; E = "differs" },
                    @{ N = "layer_motion_off"; S = "output.upscale_layer_motion=false"; E = "differs" },
                    @{ N = "panini"; S = "output.lens_panini_d=1.0"; E = "differs" },
                    @{ N = "blur_before_upscale"; S = "shading.motion_blur_after_upscale=false"; E = "differs" },
                    @{ N = "blur_rotation_off"; S = "shading.motion_blur_after_upscale_rotation=false"; E = "differs" }) },
            # the lens: the gate's camera is a pinhole without --lens (25 mm aperture focused at 3 m); the base is the
            # octave path, the rows the diaphragm path and its parts
            @{ Name = "dof"; Scenes = "bt_lobby,interior"; Gate = "--lens 0.025,3"; Time = $true; Rows = @(
                    @{ N = "diaphragm"; S = "shading.dof_diaphragm=true"; E = "differs" },
                    @{ N = "diaphragm_no_scatter"; S = "shading.dof_diaphragm=true,shading.dof_diaphragm_scatter=false"; E = "differs" },
                    @{ N = "diaphragm_no_prefilter"; S = "shading.dof_diaphragm=true,shading.dof_diaphragm_prefilter=false"; E = "differs" }) },
            # an HDR frame through the ST 2084 encoding against the SDR frame (the capture is taken before the chain)
            @{ Name = "hdr"; Scenes = "bt_lobby"; Time = $true; Rows = @(
                    @{ N = "hdr10"; S = "output.hdr_encoding=2"; G = "--display-peak 5"; E = "unseen" }) },
            # the material inputs' plates (shading_ball's inputs camera): the bricks' height map
            @{ Name = "material"; Scenes = "shading_ball"; Gate = "--camera inputs"; Time = $true; Rows = @(
                    @{ N = "parallax_off"; S = "material.parallax_steps=0"; E = "differs" },
                    @{ N = "parallax_64_steps"; S = "material.parallax_steps=64"; E = "differs" },
                    @{ N = "parallax_shadow"; S = "material.parallax_shadow=true"; E = "differs" }) },
            @{ Name = "clouds"; Scenes = "ridge_sunset,city_block"; Gate = "--clouds 0.5"; Time = $true; Rows = @(
                    @{ N = "veil_off"; S = "atmosphere.clouds.veil=false"; E = "differs" },
                    @{ N = "steps_unfiltered"; S = "atmosphere.clouds.filtered_steps=false"; E = "differs" },
                    @{ N = "ground_light_off"; S = "atmosphere.clouds.ground_light=false"; E = "differs" },
                    @{ N = "powder"; S = "atmosphere.clouds.powder=1.0"; E = "differs" },
                    @{ N = "cirrus"; G = "--cirrus 0.6"; E = "differs" }) },
            # under an overcast, in rain (8 mm/h): the far slices' sky light and the rain's veil
            @{ Name = "weather"; Scenes = "ridge_sunset,city_block"; Base = "atmosphere.fog.enabled=true"; Gate = "--clouds 0.8 --rain 8"; Time = $true; Rows = @(
                    @{ N = "far_sky_light_off"; S = "atmosphere.fog.far_sky_light=false"; E = "differs" },
                    @{ N = "rain_veil_off"; S = "atmosphere.fog.rain_veil=false"; E = "differs" }) },
            # a local fog volume in the interior's view (centre 1, 0.8, -2; radii 1, 0.8, 1; 0.8 per m): still, then as
            # steam (height falloff 2, rising 0.3 m/s, turbulence 0.6 at 0.4 m) - against the room without it
            @{ Name = "steam"; Scenes = "interior"; Time = $true; Rows = @(
                    @{ N = "mist"; G = "--fog-volume 1,0.8,-2,1,0.8,1,0.8"; E = "differs" },
                    @{ N = "steam"; G = "--fog-volume 1,0.8,-2,1,0.8,1,0.8,0,2,0.3,0.6,0.4"; E = "differs" }) },
            @{ Name = "fog"; Scenes = "ridge_sunset,city_night"; Base = "atmosphere.fog.enabled=true"; Rows = @(
                    @{ N = "air_order_off"; S = "atmosphere.fog.air_order=false"; E = "differs" },
                    @{ N = "second_layer"; S = "atmosphere.fog.second_density_per_m=0.02,atmosphere.fog.second_height_falloff_per_m=0.2"; E = "differs" }) },
            @{ Name = "hair"; Scenes = "hair_ball"; Layers = "gi,refl"; Rows = @(
                    @{ N = "hair_off_rays"; S = "raytracing.hair=false"; E = "differs" }) },
            @{ Name = "eye"; Scenes = "shading_ball"; Gate = "--camera eye_close"; Rows = @(
                    @{ N = "eye_plain"; S = "shading.eye_model=false"; E = "differs" }) }
        )
        if (Test-Path "C:\Users\USER\UnravelNext\.gpulock\HOLD") { throw "the GPU lock is on HOLD (C:\Users\USER\UnravelNext\.gpulock\HOLD): no hardware runs until it is removed" }
        $exe = Join-Path $root "build\all\bin\unx_gate_shadow_renderergate.exe"
        if (-not (Test-Path $exe)) { throw "build first: Tools\CI\Build.ps1 -Track all" }
        $abDir = Join-Path $outDir "ab"
        New-Item -ItemType Directory -Force -Path $abDir | Out-Null
        $abSheet = Join-Path $abDir "pictures.txt"
        "A/B pictures: a variant's frame against its base's (ab_compare.py); expected = what the switch should do to the picture" | Out-File -Encoding utf8 $abSheet
        $manifest = @()
        # One gate run through the GPU lock, its log kept; a device removal stops the batch.
        function Invoke-AbGate([string]$kind, [string]$log, [string[]]$gateArgs) {
            $env:UNX_DRED = if ($kind -eq "timing") { "0" } else { "1" }
            $ErrorActionPreference = "Continue"
            & powershell -NoProfile -File Tools\CI\GpuLock.ps1 -Track R -Kind $kind -- $exe @gateArgs 2>&1 | ForEach-Object { "$_" } | Out-File -Encoding utf8 $log
            $code = $LASTEXITCODE
            $ErrorActionPreference = "Stop"
            if ((Get-Content $log -Raw) -match "DEVICE_REMOVED|DEVICE_HUNG|DEVICE_RESET|device removed|device hung") { throw "device removal: $log" }
            if ($code -ne 0) { "   GATE FAILED ($code): $log" | Out-File -Encoding utf8 -Append $batchLog }
        }
        foreach ($group in $abGroups) {
            if ($Ab.Count -gt 0 -and $Ab -notcontains $group.Name) { continue }
            $res = if ($group.Res) { $group.Res } else { "1080p" }
            $layers = @(); if ($group.Layers) { $layers = @($group.Layers -split ",") }
            foreach ($scene in ($group.Scenes -split ",")) {
                # a saved game scene by its name's start, else a generated scene by name (as Run-Ue6Still.ps1)
                $file = Get-ChildItem -Path $Scenes -Filter "$scene*.unxscene" -ErrorAction SilentlyContinue | Select-Object -First 1
                $sceneArg = if ($file) { $file.FullName } else { $scene }
                $capture = $AbFrames - 1
                $rows = @(@{ N = "base"; S = ""; G = ""; E = "" }) + $group.Rows
                $basePicture = ""
                foreach ($row in $rows) {
                    $title = "ab $($group.Name) / $scene / $($row.N)"
                    "== $title" | Out-File -Encoding utf8 -Append $batchLog
                    Write-Host "== $title"
                    $dir = Join-Path $abDir (Join-Path $group.Name (Join-Path $scene $row.N))
                    New-Item -ItemType Directory -Force -Path $dir | Out-Null
                    $sets = @("gi.deterministic=true")
                    if ($group.Base) { $sets += @($group.Base -split ",") }
                    if ($row.S) { $sets += @($row.S -split ",") }
                    $common = @("--scene", $sceneArg, "--resolution", $res, "--auto-exposure")
                    foreach ($s in $sets) { $common += @("--set", $s) }
                    if ($group.Gate) { $common += @($group.Gate -split " " | Where-Object { $_ }) }
                    if ($row.G) { $common += @($row.G -split " " | Where-Object { $_ }) }
                    # the picture: one frame (the last), the camera still unless the group turns it
                    $pictureArgs = $common + @("--frames", "$AbFrames", "--warmup-frames", "0", "--capture-output", (Join-Path $dir "frame.pfm"), "--capture-frames", "$capture")
                    if ($group.Turn) { $pictureArgs += @("--path-rotate", "20") }
                    if ($layers.Count -gt 0) { $pictureArgs += @("--capture-layers", ((@("final") + $layers) -join ",")) }
                    Invoke-AbGate "correctness" (Join-Path $dir "picture.log") $pictureArgs
                    if ($group.Time) {
                        Invoke-AbGate "timing" (Join-Path $dir "timing_turning.log") ($common + @("--frames", "$AbFrames", "--path-rotate", "20", "--out", (Join-Path $dir "timing_turning")))
                    }
                    $picture = Join-Path $dir "frame_f$capture.pfm"
                    if ($row.N -eq "base") { $basePicture = $picture }
                    else {
                        $label = "$($group.Name) / $scene / $($row.N) [expected: $($row.E)]"
                        & python Tools\Verify\ab_compare.py $basePicture $picture $label | Out-File -Encoding utf8 -Append $abSheet
                        # (the layers: each against the base's of the same name)
                        foreach ($layer in $layers) {
                            $a = Get-ChildItem -Path (Split-Path -Parent $basePicture) -Filter "frame_${layer}_f$capture.pfm" -ErrorAction SilentlyContinue | Select-Object -First 1
                            $b = Get-ChildItem -Path $dir -Filter "frame_${layer}_f$capture.pfm" -ErrorAction SilentlyContinue | Select-Object -First 1
                            if ($a -and $b) { & python Tools\Verify\ab_compare.py $a.FullName $b.FullName "$label ($layer)" | Out-File -Encoding utf8 -Append $abSheet }
                        }
                        if (-not $KeepRaw) { Get-ChildItem -Path $dir -Filter *.pfm -ErrorAction SilentlyContinue | Remove-Item -Force }
                    }
                    $manifest += @(@{ group = $group.Name; scene = $scene; row = $row.N; set = (@($row.S, $row.G) | Where-Object { $_ }) -join " "; base = $group.Base; expected = $row.E;
                            dir = (Join-Path "ab" (Join-Path $group.Name (Join-Path $scene $row.N))) })
                }
                if (-not $KeepRaw -and $basePicture) { Get-ChildItem -Path (Split-Path -Parent $basePicture) -Filter *.pfm -ErrorAction SilentlyContinue | Remove-Item -Force }
            }
        }
        $manifest | ConvertTo-Json -Depth 4 | Out-File -Encoding utf8 (Join-Path $abDir "manifest.json")
    }
}
& python Tools\Verify\batch_summary.py $outDir (Join-Path $root "Cache\Ue6Diag") | Out-File -Encoding utf8 (Join-Path $outDir "summary.txt")
if (-not $KeepRaw) {
    # the raw frames go once the summary is written (the cut pictures' PNGs stay): the batch's own, and its furnace and
    # speck stills under Cache\Ue6Diag
    Get-ChildItem -Path $outDir -Recurse -Filter *.pfm -ErrorAction SilentlyContinue | Remove-Item -Force
    foreach ($d in (Get-ChildItem -Path (Join-Path $root "Cache\Ue6Diag") -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -like "batch_*" -or $_.Name -like "specks_*" })) {
        Get-ChildItem -Path $d.FullName -Filter *.pfm -ErrorAction SilentlyContinue | Remove-Item -Force
    }
}
"batch finished $(Get-Date -Format s)" | Out-File -Encoding utf8 -Append $batchLog
Write-Host "done: $outDir\summary.txt"
