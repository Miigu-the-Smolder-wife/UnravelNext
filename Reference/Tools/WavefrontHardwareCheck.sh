#!/bin/bash
# Hardware check of the staged-wavefront GPU tracer (README 3; coordination plan 2026-09-26), run after the user's game in a
# GPU window:
# fence timeout 5 s, the tracer takes its own GPU-lock slices (no wrapper), tiny cases first, every log kept and checked
# for device removal. Stops at the first failure.
S="${1:-$(cd "$(dirname "$0")/../.." && pwd)/Results/E/WavefrontHardware}"  # log folder (argument 1)
mkdir -p "$S"
B="$(cd "$(dirname "$0")/../.." && pwd)/build/E/bin"
export UNX_FENCE_TIMEOUT_S=5  # UNX_REFERENCE_TRACE_WAVE=1 logs every wavefront command list (diagnostics)
cd "$B" || exit 1
check() {  # $1 log
    if grep -qiE "DEVICE_HUNG|DEVICE_REMOVED|0x887A0006|0x887A0005|0x887A0001|UNX_FENCE_TIMEOUT|FAILED" "$1"; then
        echo "STOP: device error or failure in $1"; grep -iE "DEVICE_|0x887A|FENCE|FAILED" "$1" | head; exit 1
    fi
}
run() {  # name, command...
    local name=$1; shift
    echo "== $name $(date +%H:%M:%S)"
    "$@" > "$S/hw_$name.log" 2>&1
    local rc=$?
    tail -4 "$S/hw_$name.log"
    check "$S/hw_$name.log"
    [ $rc -ne 0 ] && { echo "STOP: $name exit $rc"; exit 1; }
}
run photo_tiny ./unx_test_reference_photo.exe tiny
run photo_atmtable ./unx_test_reference_photo.exe atmtable
run photo_progress ./unx_test_reference_photo.exe progress
D0=C:/Users/USER/Unravel/Artifacts/UnravelNext/D0/d0_level.unxscene
for spp in 16 64 256 1024; do
    run d0_$spp ./unx_reference.exe render --scene $D0 --camera host --res 1280x720 --spp $spp --device gpu --force
    grep -E "longest|wavefront bound" "$S/hw_d0_$spp.log" | tail -2
    L=$(grep -oE "dispatches \(longest [0-9.]+" "$S/hw_d0_$spp.log" | grep -oE "[0-9.]+$" | tail -1)
    if [ -n "$L" ] && awk "BEGIN{exit !($L >= 35)}"; then echo "STOP: longest dispatch $L ms near the 40 ms line"; exit 1; fi
done
run ridge_64 ./unx_reference.exe render --scene ridge_sunset --camera ridge --res 640x360 --spp 64 --device gpu --force
grep -E "longest|wavefront bound" "$S/hw_ridge_64.log" | tail -2
echo "== done $(date +%H:%M:%S)"
