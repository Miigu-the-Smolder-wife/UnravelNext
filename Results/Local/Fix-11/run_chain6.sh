#!/bin/bash
# After chain 5: the ShadeOpaque split's before / after timing (run_timing_split.ps1), one hold per resolution.
cd /c/Users/USER/UnravelNext-fix
exec > Results/Local/Fix-11/chain6.log 2>&1
while ! grep -q "chain5 done" Results/Local/Fix-11/chain5.log 2>/dev/null; do sleep 60; done
while ! grep -q "^exit 0" Results/Local/Fix-11/build-presplit.log 2>/dev/null; do sleep 60; done
echo "chain5 done and presplit built $(date +%T)"
T="powershell -NoProfile -ExecutionPolicy Bypass -File Tools/CI/GpuLock.ps1 -Track A -Kind timing -TimeoutMinutes 10 -WaitMinutes 600 --"
P="powershell -NoProfile -ExecutionPolicy Bypass -File"
R=Results/Local/Fix-11
$T $P $R/run_timing_split.ps1 -Res 1920x1080 -Tag 1080 > $R/postgame/lock_split_1080.log 2>&1; echo "split_1080 exit $? $(date +%T)"
$T $P $R/run_timing_split.ps1 -Res 2560x1440 -Tag 1440 > $R/postgame/lock_split_1440.log 2>&1; echo "split_1440 exit $? $(date +%T)"
echo "chain6 done $(date +%T)"
