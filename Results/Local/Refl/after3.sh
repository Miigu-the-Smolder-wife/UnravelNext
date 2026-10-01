#!/bin/sh
# S2 batch 3 (2026-10-01 evening, frozen scenes of Cache/ReflJudge/scenes): the deploy decision for
# reflection.layers + reflection.layer_mirror_lobe with the dense filter pass. One lock hold per piece.
# Run from the worktree root (Git Bash): sh Results/Local/Refl/after3.sh
# Question of each piece:
#   1-2   bath hall still, A0 against C: is C's reflection layer sigma at f15 / f299 no worse than A0's and the pattern
#         index at A0's level (the dot grid of C_v1 gone)? f0 / f3 better as in C_v1?
#   3-6   lounge and train still, A0 against C: no frame of C worse than A0.
#   7-12  the three views turning (90 degrees from frame 60): no frame of C worse than A0 in motion.
#   13-17 tests: the analytic reflection values with the previous path, layers off and layers on; the planar mirror;
#         host motion.
#   18    PC (C + GI's hit accumulator), for the record only (not deployed).
# Tags: A0 = layers off, hit_cone_lobes off (the path before 2026-10-01); C = layers + mirror lobe (cross mode and the
# dense pass are the defaults of this tree).
J="python Results\\Local\\Refl\\judge_run.py --layers final,refl,reflmode"
A0="--tag A0 --set reflection.layers=false --set reflection.hit_cone_lobes=false"
C="--tag C --set reflection.layers=true --set reflection.layer_mirror_lobe=true"
ACC="--set gi.hit_accumulator=true --set gi.hit_accumulator_pool=true"
T="powershell -NoProfile -ExecutionPolicy Bypass -File Results\\Local\\Refl\\run_tests.ps1 -Tag t3 -Only"
python Results/Local/Refl/queue.py after3 \
 "$J --cases bath_hall --modes still $A0" \
 "$J --cases bath_hall --modes still $C" \
 "$J --cases bath_lounge --modes still $A0" \
 "$J --cases bath_lounge --modes still $C" \
 "$J --cases train_lounge --modes still $A0" \
 "$J --cases train_lounge --modes still $C" \
 "$J --cases bath_hall --modes rot $A0" \
 "$J --cases bath_hall --modes rot $C" \
 "$J --cases bath_lounge --modes rot $A0" \
 "$J --cases bath_lounge --modes rot $C" \
 "$J --cases train_lounge --modes rot $A0" \
 "$J --cases train_lounge --modes rot $C" \
 "$T reflectionanalytic_prev" "$T reflectionanalytic_off" "$T reflectionanalytic_on" "$T planarmirror" "$T hostmotion" \
 "$J --cases bath_hall --modes still --tag PC --set reflection.layers=true --set reflection.layer_mirror_lobe=true $ACC"
