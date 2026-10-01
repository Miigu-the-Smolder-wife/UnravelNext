#!/bin/bash
# Queue 11, the minimum GPU before the user's game (coordinator 14:40): the four test suites, the city_block exactness
# A/B, the bath layers A/B. One lock hold at a time (GpuLock v1.87, track A). Lounge and timing run after the game.
cd /c/Users/USER/UnravelNext-fix
exec > Results/Local/Fix-11/chain.log 2>&1
L="powershell -NoProfile -ExecutionPolicy Bypass -File Tools/CI/GpuLock.ps1 -Track A"
P="powershell -NoProfile -ExecutionPolicy Bypass -File"
date
$L -Kind correctness -TimeoutMinutes 10 -- $P Results/Local/Fix-11/run_tests.ps1 > Results/Local/Fix-11/lock_tests2.log 2>&1; echo "tests exit $? $(date +%T)"
$L -Kind correctness -TimeoutMinutes 10 -- $P Results/Local/Fix-11/run_exact.ps1 > Results/Local/Fix-11/lock_exact.log 2>&1; echo "exact exit $? $(date +%T)"
$L -Kind correctness -TimeoutMinutes 10 -- $P Results/Local/Fix-11/run_layers.ps1 > Results/Local/Fix-11/lock_layers.log 2>&1; echo "layers exit $? $(date +%T)"
echo "chain done $(date +%T)"
