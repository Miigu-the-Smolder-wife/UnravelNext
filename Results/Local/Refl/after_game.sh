#!/bin/sh
# S2: the GPU pieces to run once .gpulock/HOLD is gone (README_KO.md section 3), one lock hold per piece, in this order.
# Run from the worktree root (Git Bash): sh Results/Local/Refl/after_game.sh
# Tags: A = previous path, B = layers (HEAD defaults), C = B + layer_mirror_lobe, N = B without the history bound,
# CN = C without the history bound; Bv*/Cv* = layer views of B / C (2 stochastic', 4 residual', 6 base, 7 history frames).
J="python Results\\Local\\Refl\\judge_run.py"
ON="--set reflection.layers=true"
LOBE="--set reflection.layer_mirror_lobe=true"
NOB="--set reflection.layer_history_bound=false"
D="$J --cases bath_hall --modes diag --layers final,refl,reflmode $ON"
T="powershell -NoProfile -ExecutionPolicy Bypass -File Results\\Local\\Refl\\run_tests.ps1 -Tag t1 -Only"
python Results/Local/Refl/queue.py after1 \
 "$J --tag B --cases bath_hall --modes still --layers final,refl,reflmode $ON" \
 "$J --tag C --cases bath_hall --modes still --layers final,refl,reflmode $ON $LOBE" \
 "$J --tag A --cases bath_hall --modes still --layers final,refl,reflmode --set reflection.layers=false" \
 "$T reflectionanalytic_on" "$T reflectionanalytic_off" "$T planarmirror" "$T hostmotion" \
 "$J --tag N --cases bath_hall --modes still $ON $NOB" \
 "$J --tag CN --cases bath_hall --modes still $ON $LOBE $NOB" \
 "$D --tag Bv7 --set reflection.layer_view=7" "$D --tag Bv2 --set reflection.layer_view=2" \
 "$D --tag Bv4 --set reflection.layer_view=4" "$D --tag Bv6 --set reflection.layer_view=6" \
 "$T reflectionanalytic_on_nohistory" "$T gianalytic"
