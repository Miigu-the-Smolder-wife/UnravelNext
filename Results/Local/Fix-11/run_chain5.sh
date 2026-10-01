#!/bin/bash
# After chain 4 (waits for its "chain4 done"): the re-run of the fixed suites and the new items' suites (run_tests3.ps1),
# one lock hold (<= 10 min).
cd /c/Users/USER/UnravelNext-fix
exec > Results/Local/Fix-11/chain5.log 2>&1
while ! grep -q "chain4 done" Results/Local/Fix-11/chain4.log 2>/dev/null; do sleep 60; done
echo "chain4 done seen $(date +%T)"
L="powershell -NoProfile -ExecutionPolicy Bypass -File Tools/CI/GpuLock.ps1 -Track A -Kind correctness -TimeoutMinutes 10 -WaitMinutes 600 --"
P="powershell -NoProfile -ExecutionPolicy Bypass -File"
R=Results/Local/Fix-11
$L $P $R/run_tests3.ps1 > $R/postgame/lock_tests3.log 2>&1; echo "tests3 exit $? $(date +%T)"
echo "chain5 done $(date +%T)"
