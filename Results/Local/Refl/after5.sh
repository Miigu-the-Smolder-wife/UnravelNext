#!/bin/sh
# S2 batch 5 (2026-10-01 evening, frozen scenes): reflection.layer_whole_value (lobe pixels reconstructed as a whole, the
# first step of the Lumen-structure port, Docs/Status/UNREAL_COMPARISON_REFLECTION_KO.md). One lock hold per piece.
# Tags: A0 = layers off, hit_cone_lobes off (captured by batches 3 / 4 where present; the binary's change does not touch
# that path); W = layers + mirror lobe + whole value (this tree's defaults with layers on); WS = W with
# reflection.hit_strict_read=false; WN = W without the history bound.
#   1    W bath hall still. Question: is the f0 level of the ceiling's and the pillar's lobe pixels back at or under
#        A0's (2.0 / 1.5; C had 5.6 / 5.3), and the layer sigma at f15 / f299 at A0's (14 / 10 %; C had 35 / 36 %)?
#   2    WS bath hall still. Question: with no fill left for lobe pixels, which hit read gives the f0 / f3 level nearer 1.
#   3-4  lobby still A0, W (the glossy floor, the user's view). Question: W better than A0 at every frame?
#   5    timing 1080p: A0 against W, bath hall and lobby (the cost section of the port plan; nothing ran timing yet).
#   6-8  lounge W (A0 from batch 3), train A0, train W, still.
#   9-14 the three views turning, A0 and W.
#   15-16 timing 1440p, 4K.
#   17   WN bath hall still (the bound again, now that the layer it guards is the whole value).
#   18-22 tests.
J="python Results\Local\Refl\judge_run.py --layers final,refl,reflmode"
A0="--tag A0 --set reflection.layers=false --set reflection.hit_cone_lobes=false"
W="--set reflection.layers=true --set reflection.layer_mirror_lobe=true"
TM="timing:python Results\Local\Refl\timing_run.py --tag tm2 --cases bath_hall,bath_lobby --configs A0:reflection.layers=false,reflection.hit_cone_lobes=false W:reflection.layers=true,reflection.layer_mirror_lobe=true"
T="powershell -NoProfile -ExecutionPolicy Bypass -File Results\Local\Refl\run_tests.ps1 -Tag t5 -Only"
python Results/Local/Refl/queue.py after5 \
 "$J --cases bath_hall --modes still --tag W $W" \
 "$J --cases bath_hall --modes still --tag WS $W --set reflection.hit_strict_read=false" \
 "$J --cases bath_lobby --modes still $A0" \
 "$J --cases bath_lobby --modes still --tag W $W" \
 "$TM --res 1920x1080" \
 "$J --cases bath_lounge --modes still --tag W $W" \
 "$J --cases train_lounge --modes still $A0" \
 "$J --cases train_lounge --modes still --tag W $W" \
 "$J --cases bath_hall --modes rot $A0" \
 "$J --cases bath_hall --modes rot --tag W $W" \
 "$J --cases bath_lobby --modes rot $A0" \
 "$J --cases bath_lobby --modes rot --tag W $W" \
 "$J --cases train_lounge --modes rot $A0" \
 "$J --cases train_lounge --modes rot --tag W $W" \
 "$TM --res 2560x1440" \
 "$TM --res 3840x2160" \
 "$J --cases bath_hall --modes still --tag WN $W --set reflection.layer_history_bound=false" \
 "$T reflectionanalytic_prev" "$T reflectionanalytic_off" "$T reflectionanalytic_on" "$T planarmirror" "$T hostmotion"
