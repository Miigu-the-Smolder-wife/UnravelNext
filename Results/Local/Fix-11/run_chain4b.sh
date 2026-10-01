#!/bin/bash
# chain 4, restarted 19:40 (A's waiter had starved behind the priority tracks since 17:29): the lobby defect attribution
# first (coordinator 19:35), then chain 4's remaining steps; appends to chain4.log and ends with "chain4 done" (chains 5
# and 6 wait for it).
cd /c/Users/USER/UnravelNext-fix
exec >> Results/Local/Fix-11/chain4.log 2>&1
echo "chain4b start $(date +%T)"
L="powershell -NoProfile -ExecutionPolicy Bypass -File Tools/CI/GpuLock.ps1 -Track A -Kind correctness -TimeoutMinutes 10 -WaitMinutes 600 --"
T="powershell -NoProfile -ExecutionPolicy Bypass -File Tools/CI/GpuLock.ps1 -Track A -Kind timing -TimeoutMinutes 10 -WaitMinutes 600 --"
P="powershell -NoProfile -ExecutionPolicy Bypass -File"
R=Results/Local/Fix-11
run() { local name=$1; shift; "$@" > $R/postgame/lock_$name.log 2>&1; echo "$name exit $? $(date +%T)"; }
run lobby_a       $L $P $R/run_lobby_diag.ps1 -Sets off,noslot,noair
run lobby_b       $L $P $R/run_lobby_diag.ps1 -Sets nolocal,noshadow
run exact         $L $P $R/run_exact.ps1
run layers        $L $P $R/run_layers.ps1
run tests_water   $L $P $R/run_tests_water.ps1
run sw_bath_a     $L $P $R/run_switches.ps1 -Scene bath -Sets off,cls,tile
run sw_bath_b     $L $P $R/run_switches.ps1 -Scene bath -Sets covtl,emis,omit
run sw_bath_c     $L $P $R/run_switches.ps1 -Scene bath -Sets all
run sw_lounge     $L $P $R/run_switches.ps1 -Scene lounge -Sets off,all
run motion        $L $P $R/run_motion.ps1
run timing_1080   $T $P $R/run_timing.ps1 -Res 1920x1080 -Tag 1080
run timing_1440   $T $P $R/run_timing.ps1 -Res 2560x1440 -Tag 1440
run timing_sw_1080 $T $P $R/run_timing_sw.ps1 -Res 1920x1080 -Tag 1080
run timing_sw_1440 $T $P $R/run_timing_sw.ps1 -Res 2560x1440 -Tag 1440
echo "chain4 done $(date +%T)"
