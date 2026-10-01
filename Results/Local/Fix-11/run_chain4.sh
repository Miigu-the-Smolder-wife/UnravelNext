#!/bin/bash
# After-game verification chain of A (coordinator 2026-10-01 evening): waits for the user's HOLD to lift, then one lock
# hold at a time (<= 10 min each, -WaitMinutes 600 so a long queue behind R and S2 does not time the waiters out as
# chain 3's did after 120 min):
#   1. queue 11 (chain 3 redone): the four test suites, the city_block exactness A/B, the bath layers A/B
#   2. the basin statistics tests (queue 10)
#   3. bath 1080p stills, switches off / each on / all on; lounge off / all
#   4. bath motion + cut, off / all
#   5. timing: queue 11 base vs new (1080p, 1440p), then the switches (1080p, 1440p)
cd /c/Users/USER/UnravelNext-fix
exec > Results/Local/Fix-11/chain4.log 2>&1
while [ -f /c/Users/USER/UnravelNext/.gpulock/HOLD ]; do sleep 60; done
echo "HOLD lifted $(date +%T)"
L="powershell -NoProfile -ExecutionPolicy Bypass -File Tools/CI/GpuLock.ps1 -Track A -Kind correctness -TimeoutMinutes 10 -WaitMinutes 600 --"
T="powershell -NoProfile -ExecutionPolicy Bypass -File Tools/CI/GpuLock.ps1 -Track A -Kind timing -TimeoutMinutes 10 -WaitMinutes 600 --"
P="powershell -NoProfile -ExecutionPolicy Bypass -File"
R=Results/Local/Fix-11
run() { local name=$1; shift; "$@" > $R/postgame/lock_$name.log 2>&1; echo "$name exit $? $(date +%T)"; }
mkdir -p $R/postgame
run tests2        $L $P $R/run_tests2.ps1
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
