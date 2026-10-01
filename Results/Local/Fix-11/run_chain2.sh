#!/bin/bash
# Queue 11 GPU runs, one lock hold at a time (GpuLock v1.86, track A): the four test suites, exactness A/B (city_block),
# bath layers A/B, lounge new capture, timing 1080p, timing 1440p.
cd /c/Users/USER/UnravelNext-fix
exec > Results/Local/Fix-11/chain.log 2>&1
L="powershell -NoProfile -ExecutionPolicy Bypass -File Tools/CI/GpuLock.ps1 -Track A"
P="powershell -NoProfile -ExecutionPolicy Bypass -File"
date
$L -Kind correctness -TimeoutMinutes 10 -- $P Results/Local/Fix-11/run_tests.ps1 > Results/Local/Fix-11/lock_tests2.log 2>&1; echo "tests exit $? $(date +%T)"
$L -Kind correctness -TimeoutMinutes 10 -- $P Results/Local/Fix-11/run_exact.ps1 > Results/Local/Fix-11/lock_exact.log 2>&1; echo "exact exit $? $(date +%T)"
$L -Kind correctness -TimeoutMinutes 10 -- $P Results/Local/Fix-11/run_layers.ps1 > Results/Local/Fix-11/lock_layers.log 2>&1; echo "layers exit $? $(date +%T)"
$L -Kind correctness -TimeoutMinutes 8 -- $P Results/Local/Fix-11/run_ab2.ps1 > Results/Local/Fix-11/lock_ab2.log 2>&1; echo "lounge exit $? $(date +%T)"
$L -Kind timing -TimeoutMinutes 10 -- $P Results/Local/Fix-11/run_timing.ps1 -Res 1920x1080 -Tag 1080 > Results/Local/Fix-11/lock_timing_1080.log 2>&1; echo "timing1080 exit $? $(date +%T)"
$L -Kind timing -TimeoutMinutes 10 -- $P Results/Local/Fix-11/run_timing.ps1 -Res 2560x1440 -Tag 1440 > Results/Local/Fix-11/lock_timing_1440.log 2>&1; echo "timing1440 exit $? $(date +%T)"
echo "chain done $(date +%T)"
