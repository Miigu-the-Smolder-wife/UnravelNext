#!/bin/sh
# S2 batch 4 (2026-10-01 evening, frozen scenes): the two defects of C found by batch 3's first pair (bath hall).
# One lock hold per piece. Run from the worktree root: sh Results/Local/Refl/after4.sh
#   1  CN  bath hall still, C without the history bound. Question: does the reflection layer sigma at f15 / f299 fall to
#          A0's (14 / 10 %)? [expected about 13 %: the bound ties the history to the frame's own reconstruction]
#   2  Cv1..Cv5  bath hall, frames 0 and 3, layer views. Question: at f0, over the ceiling's and the pillar's G pixels,
#          which layer carries the x 2.8 gain over A0 - albedo x stochastic' against albedo x stochastic, or residual'
#          against residual - and what share of the stochastic layer is 0 before the filter.
#   3  CS  bath hall still, C with reflection.hit_strict_read=false. Question: is the f0 level back at A0's (2.0 on the
#          ceiling)? Then the strict read's 'no data' pixels and their fill are the cause.
#   4-6  lobby still (the glossy floor): A0, C, CN. Question: is C / CN better than A0 at f0, f3, f15, f299.
#   7-10 A0 of the views still to measure (train still; the three views turning): the baselines do not depend on the fix.
J="python Results\Local\Refl\judge_run.py --layers final,refl,reflmode"
A0="--tag A0 --set reflection.layers=false --set reflection.hit_cone_lobes=false"
C="--set reflection.layers=true --set reflection.layer_mirror_lobe=true"
python Results/Local/Refl/queue.py after4 \
 "$J --cases bath_hall --modes still --tag CN $C --set reflection.layer_history_bound=false" \
 "python Results\Local\Refl\diag0.py C bath_hall" \
 "$J --cases bath_hall --modes still --tag CS $C --set reflection.hit_strict_read=false" \
 "$J --cases bath_lobby --modes still $A0" \
 "$J --cases bath_lobby --modes still --tag C $C" \
 "$J --cases bath_lobby --modes still --tag CN $C --set reflection.layer_history_bound=false" \
 "$J --cases train_lounge --modes still $A0" \
 "$J --cases bath_hall --modes rot $A0" \
 "$J --cases bath_lounge --modes rot $A0" \
 "$J --cases train_lounge --modes rot $A0"
