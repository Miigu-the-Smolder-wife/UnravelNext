#!/bin/sh
# S2: the GPU pieces to run once the coordinator lifts .gpulock/HOLD and says so (README_KO.md section 3), one lock hold
# per piece. Run from the worktree root (Git Bash): sh Results/Local/Refl/after_game.sh
# Tags (bath hall still, 1080p; every tag also captures reflmode):
#   A0  the path before 2026-10-01: layers off, hit_cone_lobes off
#   A   layers off, hit_cone_lobes on (HEAD defaults)
#   AO  A + hit_oriented_lights
#   P   A + GI's hit accumulator (pool) read by reflection hits   P0  the same with reflection.hit_accumulator off
#   B   layers on (HEAD defaults: residual whole, history bound on)
#   BD  B with the difference residual (design 1.2's L_g)         N   B without the history bound
#   C   B + layer_mirror_lobe                                      CN  C without the history bound
#   PC  C + the accumulator                                        (the combination expected to matter most)
#   Bv*  layer views of B (2 stochastic', 4 residual', 6 base, 7 history frames)
J="python Results\\Local\\Refl\\judge_run.py --cases bath_hall --layers final,refl,reflmode"
OFF="--set reflection.layers=false"
ON="--set reflection.layers=true"
LOBE="--set reflection.layer_mirror_lobe=true"
NOB="--set reflection.layer_history_bound=false"
ACC="--set gi.hit_accumulator=true --set gi.hit_accumulator_pool=true"
T="powershell -NoProfile -ExecutionPolicy Bypass -File Results\\Local\\Refl\\run_tests.ps1 -Tag t1 -Only"
python Results/Local/Refl/queue.py after1 \
 "$J --tag A0 --modes still $OFF --set reflection.hit_cone_lobes=false" \
 "$J --tag A --modes still $OFF" \
 "$J --tag B --modes still $ON" \
 "$J --tag C --modes still $ON $LOBE" \
 "$J --tag P --modes still $OFF $ACC" \
 "$J --tag PC --modes still $ON $LOBE $ACC" \
 "$T reflectionanalytic_prev" "$T reflectionanalytic_off" "$T reflectionanalytic_on" "$T planarmirror" "$T hostmotion" \
 "$J --tag P0 --modes still $OFF $ACC --set reflection.hit_accumulator=false" \
 "$J --tag AO --modes still $OFF --set reflection.hit_oriented_lights=true" \
 "$J --tag BD --modes still $ON --set reflection.layer_residual_whole=false" \
 "$J --tag N --modes still $ON $NOB" \
 "$J --tag CN --modes still $ON $LOBE $NOB" \
 "$J --tag Bv7 --modes diag $ON --set reflection.layer_view=7" "$J --tag Bv2 --modes diag $ON --set reflection.layer_view=2" \
 "$J --tag Bv4 --modes diag $ON --set reflection.layer_view=4" "$J --tag Bv6 --modes diag $ON --set reflection.layer_view=6" \
 "$T reflectionanalytic_oriented" "$T reflectionanalytic_on_nohistory" "$T gianalytic"
